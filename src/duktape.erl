%%% -*- erlang -*-
%%%
%%% Copyright (c) 2025 Benoit Chesneau
%%%
%%% This source code is licensed under the MIT license found in the
%%% LICENSE file in the root directory of this source tree.

%% @doc Duktape JavaScript engine for Erlang
-module(duktape).

%% API
-export([
    info/0,
    new_context/0,
    new_context/1,
    destroy_context/1,
    eval/2,
    eval/3,
    call/2,
    call/3,
    register_module/3,
    require/2,
    send/3,
    register_function/3,
    cbor_encode/2,
    cbor_decode/2,
    %% Metrics
    get_memory_stats/1,
    gc/1
]).

%% Types
-export_type([context/0, js_value/0, bindings/0, context_opts/0, memory_stats/0]).

-opaque context() :: reference().

-type context_opts() :: #{handler => pid()}.

-type js_value() :: integer()
                  | float()
                  | binary()
                  | true
                  | false
                  | null
                  | undefined
                  | list(js_value())
                  | #{binary() | atom() => js_value()}.

-type bindings() :: #{atom() | binary() => term()}.

-type memory_stats() :: #{
    heap_bytes := non_neg_integer(),
    heap_peak := non_neg_integer(),
    alloc_count := non_neg_integer(),
    realloc_count := non_neg_integer(),
    free_count := non_neg_integer(),
    gc_runs := non_neg_integer()
}.

%% NIF loading
-compile(no_native).
-on_load(on_load/0).

-define(nif_stub, nif_stub_error(?LINE)).
nif_stub_error(Line) ->
    erlang:nif_error({nif_not_loaded, module, ?MODULE, line, Line}).

-spec on_load() -> ok | {error, any()}.
on_load() ->
    SoName = case code:priv_dir(?MODULE) of
        {error, bad_name} ->
            case code:which(?MODULE) of
                Filename when is_list(Filename) ->
                    filename:join([filename:dirname(Filename), "../priv", "duktape"]);
                _ ->
                    filename:join("../priv", "duktape")
            end;
        Dir ->
            filename:join(Dir, "duktape")
    end,
    erlang:load_nif(SoName, application:get_all_env(duktape)).

%% @doc Get NIF information. Used to verify NIF is loaded correctly.
-spec info() -> {ok, string()}.
info() ->
    nif_info().

%% @doc Create a new JavaScript context.
%% The context will be automatically cleaned up when garbage collected.
-spec new_context() -> {ok, context()} | {error, term()}.
new_context() ->
    nif_new_context().

%% @doc Create a new JavaScript context with options.
%%
%% Options:
%% - `handler => pid()': Process to receive events from JavaScript.
%%   The handler will receive messages of the form `{duktape, Type, Data}'
%%   where Type is an atom (e.g., `log') and Data is the event payload.
%%
%% JavaScript can send events to Erlang using:
%% - `Erlang.emit(type, data)' - send custom events
%% - `Erlang.log(level, ...)' - send log messages
%% - `console.log/info/warn/error/debug(...)' - convenience logging
%%
%% Example:
%% ```
%% {ok, Ctx} = duktape:new_context(#{handler => self()}),
%% {ok, _} = duktape:eval(Ctx, <<"console.log('hello')">>),
%% receive
%%     {duktape, log, #{level := info, message := Msg}} ->
%%         io:format("Got: ~s~n", [Msg])
%% end.
%% '''
-spec new_context(context_opts()) -> {ok, context()} | {error, term()}.
new_context(Opts) when is_map(Opts) ->
    nif_new_context_opts(Opts).

%% @doc Explicitly destroy a JavaScript context.
%% This is optional - contexts are automatically cleaned up on GC.
%% Calling destroy on an already-destroyed context is safe (idempotent).
-spec destroy_context(context()) -> ok | {error, term()}.
destroy_context(Ctx) ->
    nif_destroy_context(Ctx).

%% @doc Evaluate JavaScript code in a context.
%% Returns the result of the last expression.
%%
%% If the JavaScript code calls registered Erlang functions, they will be
%% dispatched automatically via the trampoline pattern.
%%
%% Examples:
%% ```
%% {ok, 3} = duktape:eval(Ctx, <<"1 + 2">>).
%% {ok, <<"hello">>} = duktape:eval(Ctx, <<"'hello'">>).
%% {error, {js_error, _}} = duktape:eval(Ctx, <<"throw 'oops'">>).
%% '''
-spec eval(context(), iodata()) -> {ok, js_value()} | {error, term()}.
eval(Ctx, Code) ->
    eval_loop(Ctx, nif_eval(Ctx, Code)).

%% @doc Evaluate JavaScript code with variable bindings.
%% Bindings are set as global variables before evaluation.
%%
%% Erlang to JavaScript type conversions:
%% - integers/floats -> numbers
%% - binaries -> strings
%% - atoms (true/false/null/undefined) -> JS primitives
%% - other atoms -> strings
%% - lists -> arrays (unless it's an iolist, then string)
%% - maps -> objects
%% - tuples -> arrays
%%
%% Examples:
%% ```
%% {ok, 30} = duktape:eval(Ctx, <<"x * y">>, #{<<"x">> => 5, <<"y">> => 6}).
%% {ok, <<"hello world">>} = duktape:eval(Ctx, <<"greeting + ' ' + name">>,
%%                                        #{greeting => <<"hello">>, name => <<"world">>}).
%% '''
-spec eval(context(), iodata(), bindings()) -> {ok, js_value()} | {error, term()}.
eval(Ctx, Code, Bindings) when is_map(Bindings) ->
    eval_loop(Ctx, nif_eval_bindings(Ctx, Code, Bindings)).

%% @doc Call a global JavaScript function with no arguments.
%% Equivalent to call(Ctx, FunctionName, []).
%%
%% Examples:
%% ```
%% {ok, _} = duktape:eval(Ctx, <<"function getTime() { return Date.now(); }">>).
%% {ok, Timestamp} = duktape:call(Ctx, <<"getTime">>).
%% '''
-spec call(context(), iodata() | atom()) -> {ok, js_value()} | {error, term()}.
call(Ctx, FunctionName) ->
    call(Ctx, FunctionName, []).

%% @doc Call a global JavaScript function with arguments.
%% The function must exist in the global scope.
%%
%% Examples:
%% ```
%% {ok, _} = duktape:eval(Ctx, <<"function add(a, b) { return a + b; }">>).
%% {ok, 7} = duktape:call(Ctx, <<"add">>, [3, 4]).
%% {ok, 7} = duktape:call(Ctx, add, [3, 4]).
%% '''
-spec call(context(), iodata() | atom(), [term()]) -> {ok, js_value()} | {error, term()}.
call(Ctx, FunctionName, Args) when is_list(Args) ->
    eval_loop(Ctx, nif_call(Ctx, FunctionName, Args)).

%% @doc Register a CommonJS module with source code.
%% The module can then be loaded with require/2 or via require() in JavaScript.
%%
%% Examples:
%% ```
%% ok = duktape:register_module(Ctx, <<"math">>, <<"exports.add = function(a, b) { return a + b; };">>).
%% {ok, Exports} = duktape:require(Ctx, <<"math">>).
%% '''
-spec register_module(context(), iodata() | atom(), iodata()) -> ok | {error, term()}.
register_module(Ctx, ModuleId, Source) ->
    nif_register_module(Ctx, ModuleId, Source).

%% @doc Load a CommonJS module and return its exports.
%% The module must have been registered with register_module/3.
%% Modules are cached - subsequent requires return the same exports object.
%%
%% Examples:
%% ```
%% ok = duktape:register_module(Ctx, <<"utils">>, <<"exports.greet = function(n) { return 'Hello, ' + n; };">>).
%% {ok, _} = duktape:require(Ctx, <<"utils">>).
%% {ok, <<"Hello, World">>} = duktape:eval(Ctx, <<"require('utils').greet('World')">>).
%% '''
-spec require(context(), iodata() | atom()) -> {ok, js_value()} | {error, term()}.
require(Ctx, ModuleId) ->
    nif_require(Ctx, ModuleId).

%% @doc Send data to a registered JavaScript callback.
%% If JavaScript code has registered a callback using `Erlang.on(event, fn)',
%% this function will call that callback with the provided data.
%%
%% Returns `{ok, Result}' where Result is the return value of the callback,
%% or `ok' if no callback is registered for the event (silently succeeds).
%%
%% Example:
%% ```
%% {ok, Ctx} = duktape:new_context(),
%% %% JavaScript registers a callback
%% {ok, _} = duktape:eval(Ctx, <<"
%%     var received = null;
%%     Erlang.on('data', function(d) { received = d; return 'got it'; });
%% ">>),
%% %% Erlang sends data to the callback
%% {ok, <<"got it">>} = duktape:send(Ctx, data, #{value => 42}),
%% {ok, #{<<"value">> := 42}} = duktape:eval(Ctx, <<"received">>).
%% '''
-spec send(context(), atom() | iodata(), term()) -> {ok, js_value()} | ok | {error, term()}.
send(Ctx, Event, Data) ->
    nif_send(Ctx, Event, Data).

%% @doc Encode an Erlang value to CBOR binary.
%% The value is first converted to a JavaScript value, then encoded to CBOR.
%%
%% Erlang to CBOR type mapping (via JavaScript):
%% - integers/floats -> CBOR numbers
%% - binaries -> CBOR text strings
%% - atoms (true/false/null/undefined) -> CBOR primitives
%% - other atoms -> CBOR text strings
%% - lists -> CBOR arrays
%% - maps -> CBOR maps
%% - tuples -> CBOR arrays
%%
%% Example:
%% ```
%% {ok, Ctx} = duktape:new_context(),
%% {ok, Bin} = duktape:cbor_encode(Ctx, #{name => <<"Alice">>, age => 30}),
%% {ok, #{<<"name">> := <<"Alice">>, <<"age">> := 30}} = duktape:cbor_decode(Ctx, Bin).
%% '''
-spec cbor_encode(context(), term()) -> {ok, binary()} | {error, term()}.
cbor_encode(Ctx, Value) ->
    nif_cbor_encode(Ctx, Value).

%% @doc Decode a CBOR binary to an Erlang value.
%% The CBOR is decoded to a JavaScript value, then converted to Erlang.
%%
%% CBOR to Erlang type mapping (via JavaScript):
%% - CBOR numbers (integer) -> integer
%% - CBOR numbers (float) -> float
%% - CBOR text strings -> binary
%% - CBOR byte strings -> binary
%% - CBOR true/false -> true/false atoms
%% - CBOR null -> null atom
%% - CBOR undefined -> undefined atom
%% - CBOR arrays -> lists
%% - CBOR maps -> maps
%%
%% Example:
%% ```
%% {ok, Ctx} = duktape:new_context(),
%% {ok, Bin} = duktape:cbor_encode(Ctx, [1, 2, 3]),
%% {ok, [1, 2, 3]} = duktape:cbor_decode(Ctx, Bin).
%% '''
-spec cbor_decode(context(), binary()) -> {ok, js_value()} | {error, term()}.
cbor_decode(Ctx, Binary) ->
    nif_cbor_decode(Ctx, Binary).

%% @doc Get memory statistics for a JavaScript context.
%% Returns a map with the following keys:
%% - `heap_bytes': Current allocated bytes in the Duktape heap
%% - `heap_peak': Peak memory usage since context creation
%% - `alloc_count': Total number of allocations
%% - `realloc_count': Total number of reallocations
%% - `free_count': Total number of frees
%% - `gc_runs': Number of garbage collection runs triggered
%%
%% Example:
%% ```
%% {ok, Ctx} = duktape:new_context(),
%% {ok, _} = duktape:eval(Ctx, <<"var x = []; for(var i=0; i<1000; i++) x.push(i);">>),
%% {ok, Stats} = duktape:get_memory_stats(Ctx),
%% io:format("Heap: ~p bytes~n", [maps:get(heap_bytes, Stats)]).
%% '''
-spec get_memory_stats(context()) -> {ok, memory_stats()} | {error, term()}.
get_memory_stats(Ctx) ->
    nif_get_memory_stats(Ctx).

%% @doc Trigger garbage collection on a JavaScript context.
%% Forces Duktape's mark-and-sweep garbage collector to run.
%% The gc_runs counter in memory stats will be incremented.
%%
%% Example:
%% ```
%% {ok, Ctx} = duktape:new_context(),
%% {ok, _} = duktape:eval(Ctx, <<"var x = {}; x = null;">>),
%% ok = duktape:gc(Ctx),
%% {ok, #{gc_runs := 1}} = duktape:get_memory_stats(Ctx).
%% '''
-spec gc(context()) -> ok | {error, term()}.
gc(Ctx) ->
    nif_gc(Ctx).

%% @doc Register an Erlang function callable from JavaScript.
%% The function receives a list of arguments passed from JavaScript.
%%
%% Supports both anonymous functions and {Module, Function} tuples.
%% The function must accept a single argument (the list of JS arguments).
%%
%% Example:
%% ```
%% {ok, Ctx} = duktape:new_context(),
%% ok = duktape:register_function(Ctx, greet, fun([Name]) ->
%%     <<"Hello, ", Name/binary, "!">>
%% end),
%% {ok, <<"Hello, World!">>} = duktape:eval(Ctx, <<"greet('World')">>).
%% '''
%%
%% With {Module, Function}:
%% ```
%% ok = duktape:register_function(Ctx, my_func, {my_module, my_function}).
%% '''
-spec register_function(context(), atom() | binary(), Fun | {atom(), atom()}) -> ok | {error, term()}
    when Fun :: fun(([term()]) -> term()).
register_function(Ctx, Name, Fun) when is_function(Fun, 1) ->
    %% Store function in process dictionary keyed by {context_hash, name}
    Key = make_function_key(Ctx, Name),
    put(Key, Fun),
    nif_register_erlang_function(Ctx, Name);
register_function(Ctx, Name, {M, F}) when is_atom(M), is_atom(F) ->
    %% Wrap {M, F} as a function that applies with Args as single argument
    Fun = fun(Args) -> apply(M, F, [Args]) end,
    register_function(Ctx, Name, Fun).

%% ============================================================================
%% Internal: Erlang function dispatch loop
%% ============================================================================

%% @private
%% Dispatch loop for handling Erlang function calls from JavaScript.
%% When JS calls a registered Erlang function, the NIF returns
%% {call_erlang, FuncName, Args}. We dispatch to the Erlang function,
%% store the result, and resume JS execution.
-spec eval_loop(context(), term()) -> {ok, js_value()} | {error, term()}.
eval_loop(_Ctx, {ok, Value}) ->
    {ok, Value};
eval_loop(_Ctx, {error, _} = Error) ->
    Error;
eval_loop(Ctx, {call_erlang, FuncName, Args}) ->
    %% Dispatch to registered Erlang function
    Result = dispatch_erlang_call(Ctx, FuncName, Args),
    %% Store result and continue
    ok = nif_call_complete(Ctx, Result),
    eval_loop(Ctx, nif_eval_resume(Ctx)).

%% @private
%% Dispatch a call to a registered Erlang function.
-spec dispatch_erlang_call(context(), atom(), list()) -> term().
dispatch_erlang_call(Ctx, FuncName, Args) ->
    case get_registered_function(Ctx, FuncName) of
        {ok, Fun} ->
            try Fun(Args)
            catch
                error:Reason -> {error, {erlang_error, Reason}};
                throw:Reason -> {error, {erlang_throw, Reason}};
                exit:Reason  -> {error, {erlang_exit, Reason}}
            end;
        error ->
            {error, {undefined_function, FuncName}}
    end.

%% @private
%% Get a registered function from the process dictionary.
-spec get_registered_function(context(), atom()) -> {ok, fun(([term()]) -> term())} | error.
get_registered_function(Ctx, FuncName) ->
    Key = make_function_key(Ctx, FuncName),
    case get(Key) of
        undefined -> error;
        Fun -> {ok, Fun}
    end.

%% @private
%% Create a unique key for storing functions in the process dictionary.
-spec make_function_key(context(), atom() | binary()) -> {duktape_function, integer(), atom()}.
make_function_key(Ctx, Name) when is_binary(Name) ->
    make_function_key(Ctx, binary_to_atom(Name, utf8));
make_function_key(Ctx, Name) when is_atom(Name) ->
    {duktape_function, erlang:phash2(Ctx), Name}.

%% ============================================================================
%% Internal NIF stubs
%% ============================================================================

nif_info() -> ?nif_stub.
nif_new_context() -> ?nif_stub.
nif_new_context_opts(_Opts) -> ?nif_stub.
nif_destroy_context(_Ctx) -> ?nif_stub.
nif_eval(_Ctx, _Code) -> ?nif_stub.
nif_eval_bindings(_Ctx, _Code, _Bindings) -> ?nif_stub.
nif_call(_Ctx, _FunctionName, _Args) -> ?nif_stub.
nif_register_module(_Ctx, _ModuleId, _Source) -> ?nif_stub.
nif_require(_Ctx, _ModuleId) -> ?nif_stub.
nif_send(_Ctx, _Event, _Data) -> ?nif_stub.
nif_register_erlang_function(_Ctx, _Name) -> ?nif_stub.
nif_call_complete(_Ctx, _Result) -> ?nif_stub.
nif_eval_resume(_Ctx) -> ?nif_stub.
nif_cbor_encode(_Ctx, _Value) -> ?nif_stub.
nif_cbor_decode(_Ctx, _Binary) -> ?nif_stub.
nif_get_memory_stats(_Ctx) -> ?nif_stub.
nif_gc(_Ctx) -> ?nif_stub.
