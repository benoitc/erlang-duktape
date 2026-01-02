%%% -*- erlang -*-
%%%
%%% Copyright (c) 2025 Benoit Chesneau
%%%
%%% Licensed under the Apache License, Version 2.0 (the "License");
%%% you may not use this file except in compliance with the License.
%%% You may obtain a copy of the License at
%%%
%%% http://www.apache.org/licenses/LICENSE-2.0
%%%
%%% Unless required by applicable law or agreed to in writing, software
%%% distributed under the License is distributed on an "AS IS" BASIS,
%%% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
%%% See the License for the specific language governing permissions and
%%% limitations under the License.

%% @doc Duktape JavaScript engine for Erlang
-module(duktape).

%% API
-export([
    info/0,
    new_context/0,
    destroy_context/1,
    eval/2,
    eval/3
]).

%% Types
-export_type([context/0, js_value/0, bindings/0]).

-opaque context() :: reference().

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

%% @doc Explicitly destroy a JavaScript context.
%% This is optional - contexts are automatically cleaned up on GC.
%% Calling destroy on an already-destroyed context is safe (idempotent).
-spec destroy_context(context()) -> ok | {error, term()}.
destroy_context(Ctx) ->
    nif_destroy_context(Ctx).

%% @doc Evaluate JavaScript code in a context.
%% Returns the result of the last expression.
%%
%% Examples:
%% ```
%% {ok, 3} = duktape:eval(Ctx, <<"1 + 2">>).
%% {ok, <<"hello">>} = duktape:eval(Ctx, <<"'hello'">>).
%% {error, {js_error, _}} = duktape:eval(Ctx, <<"throw 'oops'">>).
%% '''
-spec eval(context(), iodata()) -> {ok, js_value()} | {error, term()}.
eval(Ctx, Code) ->
    nif_eval(Ctx, Code).

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
    nif_eval_bindings(Ctx, Code, Bindings).

%% ============================================================================
%% Internal NIF stubs
%% ============================================================================

nif_info() -> ?nif_stub.
nif_new_context() -> ?nif_stub.
nif_destroy_context(_Ctx) -> ?nif_stub.
nif_eval(_Ctx, _Code) -> ?nif_stub.
nif_eval_bindings(_Ctx, _Code, _Bindings) -> ?nif_stub.
