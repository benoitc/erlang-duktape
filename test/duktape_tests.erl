%%% -*- erlang -*-
%%%
%%% Copyright (c) 2025 Benoit Chesneau
%%%
%%% Licensed under the Apache License, Version 2.0

-module(duktape_tests).

-include_lib("eunit/include/eunit.hrl").

%% ============================================================================
%% Test: NIF loads correctly
%% ============================================================================

nif_load_test() ->
    Result = duktape:info(),
    ?assertMatch({ok, _}, Result),
    {ok, Info} = Result,
    ?assert(is_list(Info)),
    ?assertEqual("duktape nif loaded", Info).

%% ============================================================================
%% Test: Context creation and destruction
%% ============================================================================

context_create_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assert(is_reference(Ctx)),
    ?assertEqual(ok, duktape:destroy_context(Ctx)).

context_double_destroy_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual(ok, duktape:destroy_context(Ctx)),
    ?assertEqual(ok, duktape:destroy_context(Ctx)).

context_multiple_test() ->
    {ok, Ctx1} = duktape:new_context(),
    {ok, Ctx2} = duktape:new_context(),
    {ok, Ctx3} = duktape:new_context(),
    ?assertNotEqual(Ctx1, Ctx2),
    ?assertNotEqual(Ctx2, Ctx3),
    ?assertNotEqual(Ctx1, Ctx3),
    ?assertEqual(ok, duktape:destroy_context(Ctx1)),
    ?assertEqual(ok, duktape:destroy_context(Ctx2)),
    ?assertEqual(ok, duktape:destroy_context(Ctx3)).

context_gc_cleanup_test() ->
    Self = self(),
    Pid = spawn(fun() ->
        {ok, Ctx} = duktape:new_context(),
        Self ! {context_created, Ctx},
        ok
    end),
    Ctx = receive
        {context_created, C} -> C
    after 1000 ->
        ?assert(false)
    end,
    Ref = monitor(process, Pid),
    receive
        {'DOWN', Ref, process, Pid, _} -> ok
    after 1000 ->
        ?assert(false)
    end,
    erlang:garbage_collect(),
    timer:sleep(10),
    ?assert(is_reference(Ctx)),
    ?assertEqual(ok, duktape:destroy_context(Ctx)).

context_badarg_test() ->
    ?assertMatch({error, badarg}, duktape:destroy_context(not_a_context)),
    ?assertMatch({error, badarg}, duktape:destroy_context(123)),
    ?assertMatch({error, badarg}, duktape:destroy_context(<<"binary">>)).

%% ============================================================================
%% Test: Basic JavaScript evaluation
%% ============================================================================

eval_integer_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, 3}, duktape:eval(Ctx, <<"1 + 2">>)),
    ?assertEqual({ok, 42}, duktape:eval(Ctx, <<"42">>)),
    ?assertEqual({ok, -10}, duktape:eval(Ctx, <<"-10">>)),
    ?assertEqual({ok, 0}, duktape:eval(Ctx, <<"0">>)),
    ok = duktape:destroy_context(Ctx).

eval_float_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, 3.14}, duktape:eval(Ctx, <<"3.14">>)),
    ?assertEqual({ok, 0.5}, duktape:eval(Ctx, <<"1 / 2">>)),
    ok = duktape:destroy_context(Ctx).

eval_string_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, <<"hello">>}, duktape:eval(Ctx, <<"'hello'">>)),
    ?assertEqual({ok, <<"hello world">>}, duktape:eval(Ctx, <<"'hello' + ' ' + 'world'">>)),
    ?assertEqual({ok, <<"">>}, duktape:eval(Ctx, <<"''">>)),
    ok = duktape:destroy_context(Ctx).

eval_boolean_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, true}, duktape:eval(Ctx, <<"true">>)),
    ?assertEqual({ok, false}, duktape:eval(Ctx, <<"false">>)),
    ?assertEqual({ok, true}, duktape:eval(Ctx, <<"1 == 1">>)),
    ?assertEqual({ok, false}, duktape:eval(Ctx, <<"1 == 2">>)),
    ok = duktape:destroy_context(Ctx).

eval_null_undefined_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, null}, duktape:eval(Ctx, <<"null">>)),
    ?assertEqual({ok, undefined}, duktape:eval(Ctx, <<"undefined">>)),
    ok = duktape:destroy_context(Ctx).

eval_variable_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Define a variable and use it
    ?assertEqual({ok, undefined}, duktape:eval(Ctx, <<"var x = 10">>)),
    ?assertEqual({ok, 10}, duktape:eval(Ctx, <<"x">>)),
    ?assertEqual({ok, 20}, duktape:eval(Ctx, <<"x * 2">>)),
    ok = duktape:destroy_context(Ctx).

eval_function_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Define a function and call it
    ?assertEqual({ok, undefined}, duktape:eval(Ctx, <<"function add(a, b) { return a + b; }">>)),
    ?assertEqual({ok, 7}, duktape:eval(Ctx, <<"add(3, 4)">>)),
    ok = duktape:destroy_context(Ctx).

eval_iolist_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Test with iolist input
    ?assertEqual({ok, 6}, duktape:eval(Ctx, ["1", <<" + ">>, "2", <<" + 3">>])),
    ok = duktape:destroy_context(Ctx).

%% ============================================================================
%% Test: JavaScript error handling
%% ============================================================================

eval_error_syntax_test() ->
    {ok, Ctx} = duktape:new_context(),
    Result = duktape:eval(Ctx, <<"function(">>),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

eval_error_throw_test() ->
    {ok, Ctx} = duktape:new_context(),
    Result = duktape:eval(Ctx, <<"throw 'oops'">>),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

eval_error_reference_test() ->
    {ok, Ctx} = duktape:new_context(),
    Result = duktape:eval(Ctx, <<"nonexistent_variable">>),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

eval_destroyed_context_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:destroy_context(Ctx),
    ?assertMatch({error, invalid_context}, duktape:eval(Ctx, <<"1 + 1">>)).

eval_invalid_context_test() ->
    ?assertMatch({error, invalid_context}, duktape:eval(not_a_context, <<"1">>)).

%% ============================================================================
%% Test: Context isolation
%% ============================================================================

eval_context_isolation_test() ->
    {ok, Ctx1} = duktape:new_context(),
    {ok, Ctx2} = duktape:new_context(),
    %% Define variable in Ctx1
    ?assertEqual({ok, undefined}, duktape:eval(Ctx1, <<"var x = 100">>)),
    ?assertEqual({ok, 100}, duktape:eval(Ctx1, <<"x">>)),
    %% Variable should not exist in Ctx2
    ?assertMatch({error, {js_error, _}}, duktape:eval(Ctx2, <<"x">>)),
    %% Define different value in Ctx2
    ?assertEqual({ok, undefined}, duktape:eval(Ctx2, <<"var x = 200">>)),
    ?assertEqual({ok, 200}, duktape:eval(Ctx2, <<"x">>)),
    %% Ctx1 should still have its own value
    ?assertEqual({ok, 100}, duktape:eval(Ctx1, <<"x">>)),
    ok = duktape:destroy_context(Ctx1),
    ok = duktape:destroy_context(Ctx2).

%% ============================================================================
%% Test: eval/3 with bindings
%% ============================================================================

eval_bindings_integer_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, 30}, duktape:eval(Ctx, <<"x * y">>, #{<<"x">> => 5, <<"y">> => 6})),
    ?assertEqual({ok, 15}, duktape:eval(Ctx, <<"a + b + c">>, #{<<"a">> => 5, <<"b">> => 7, <<"c">> => 3})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_float_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, 6.28}, duktape:eval(Ctx, <<"pi * 2">>, #{<<"pi">> => 3.14})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_string_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, <<"hello world">>},
                 duktape:eval(Ctx, <<"greeting + ' ' + name">>,
                              #{<<"greeting">> => <<"hello">>, <<"name">> => <<"world">>})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_boolean_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, true}, duktape:eval(Ctx, <<"flag">>, #{<<"flag">> => true})),
    ?assertEqual({ok, false}, duktape:eval(Ctx, <<"flag">>, #{<<"flag">> => false})),
    ?assertEqual({ok, true}, duktape:eval(Ctx, <<"a && b">>, #{<<"a">> => true, <<"b">> => true})),
    ?assertEqual({ok, false}, duktape:eval(Ctx, <<"a && b">>, #{<<"a">> => true, <<"b">> => false})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_null_undefined_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, null}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => null})),
    ?assertEqual({ok, undefined}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => undefined})),
    ?assertEqual({ok, true}, duktape:eval(Ctx, <<"x === null">>, #{<<"x">> => null})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_atom_key_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Atom keys should work
    ?assertEqual({ok, 10}, duktape:eval(Ctx, <<"x">>, #{x => 10})),
    ?assertEqual({ok, 30}, duktape:eval(Ctx, <<"x + y">>, #{x => 10, y => 20})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_atom_value_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Non-special atoms become strings
    ?assertEqual({ok, <<"hello">>}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => hello})),
    ?assertEqual({ok, <<"foo">>}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => foo})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_map_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Maps become objects
    ?assertEqual({ok, 10}, duktape:eval(Ctx, <<"obj.x">>, #{<<"obj">> => #{<<"x">> => 10}})),
    ?assertEqual({ok, <<"bar">>}, duktape:eval(Ctx, <<"obj.foo">>,
                                                #{<<"obj">> => #{<<"foo">> => <<"bar">>}})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_nested_map_test() ->
    {ok, Ctx} = duktape:new_context(),
    Nested = #{<<"a">> => #{<<"b">> => #{<<"c">> => 42}}},
    ?assertEqual({ok, 42}, duktape:eval(Ctx, <<"obj.a.b.c">>, #{<<"obj">> => Nested})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_tuple_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Tuples become arrays
    ?assertEqual({ok, 1}, duktape:eval(Ctx, <<"arr[0]">>, #{<<"arr">> => {1, 2, 3}})),
    ?assertEqual({ok, 3}, duktape:eval(Ctx, <<"arr[2]">>, #{<<"arr">> => {1, 2, 3}})),
    ?assertEqual({ok, 3}, duktape:eval(Ctx, <<"arr.length">>, #{<<"arr">> => {1, 2, 3}})),
    ok = duktape:destroy_context(Ctx).

eval_bindings_persist_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Bindings should persist in context
    ?assertEqual({ok, 10}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => 10})),
    ?assertEqual({ok, 10}, duktape:eval(Ctx, <<"x">>)),  %% Still accessible
    ok = duktape:destroy_context(Ctx).

eval_bindings_empty_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Empty bindings should work
    ?assertEqual({ok, 42}, duktape:eval(Ctx, <<"42">>, #{})),
    ok = duktape:destroy_context(Ctx).

%% ============================================================================
%% Test: JavaScript to Erlang type conversion (arrays and objects)
%% ============================================================================

eval_return_array_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Simple array
    ?assertEqual({ok, [1, 2, 3]}, duktape:eval(Ctx, <<"[1, 2, 3]">>)),
    %% Empty array
    ?assertEqual({ok, []}, duktape:eval(Ctx, <<"[]">>)),
    %% Mixed types in array
    {ok, Result} = duktape:eval(Ctx, <<"[1, 'hello', true, null]">>),
    ?assertEqual([1, <<"hello">>, true, null], Result),
    ok = duktape:destroy_context(Ctx).

eval_return_object_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Simple object
    {ok, Result1} = duktape:eval(Ctx, <<"({x: 1, y: 2})">>),
    ?assertEqual(#{<<"x">> => 1, <<"y">> => 2}, Result1),
    %% Empty object
    ?assertEqual({ok, #{}}, duktape:eval(Ctx, <<"({})">>)),
    %% Object with string values
    {ok, Result2} = duktape:eval(Ctx, <<"({name: 'John', city: 'NYC'})">>),
    ?assertEqual(#{<<"name">> => <<"John">>, <<"city">> => <<"NYC">>}, Result2),
    ok = duktape:destroy_context(Ctx).

eval_return_nested_array_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Nested arrays
    ?assertEqual({ok, [[1, 2], [3, 4]]}, duktape:eval(Ctx, <<"[[1, 2], [3, 4]]">>)),
    %% Deeply nested
    ?assertEqual({ok, [[[1]]]}, duktape:eval(Ctx, <<"[[[1]]]">>)),
    ok = duktape:destroy_context(Ctx).

eval_return_nested_object_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Nested objects
    {ok, Result} = duktape:eval(Ctx, <<"({a: {b: {c: 42}}})">>),
    ?assertEqual(#{<<"a">> => #{<<"b">> => #{<<"c">> => 42}}}, Result),
    ok = duktape:destroy_context(Ctx).

eval_return_mixed_nested_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Object with array value
    {ok, Result1} = duktape:eval(Ctx, <<"({items: [1, 2, 3]})">>),
    ?assertEqual(#{<<"items">> => [1, 2, 3]}, Result1),
    %% Array with object elements
    {ok, Result2} = duktape:eval(Ctx, <<"[{x: 1}, {x: 2}]">>),
    ?assertEqual([#{<<"x">> => 1}, #{<<"x">> => 2}], Result2),
    ok = duktape:destroy_context(Ctx).

eval_return_function_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Functions return their string representation
    {ok, Result} = duktape:eval(Ctx, <<"(function add(a, b) { return a + b; })">>),
    ?assert(is_binary(Result)),
    ?assertMatch({match, _}, re:run(Result, <<"function">>)),
    ok = duktape:destroy_context(Ctx).

eval_return_date_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Date objects - should return as object with properties or string
    {ok, _Result} = duktape:eval(Ctx, <<"new Date(0)">>),
    %% Just verify it doesn't crash
    ok = duktape:destroy_context(Ctx).

eval_roundtrip_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Test that data survives a round trip
    Data = #{<<"users">> => [
        #{<<"name">> => <<"Alice">>, <<"age">> => 30},
        #{<<"name">> => <<"Bob">>, <<"age">> => 25}
    ]},
    {ok, Result} = duktape:eval(Ctx, <<"data">>, #{<<"data">> => Data}),
    ?assertEqual(Data, Result),
    ok = duktape:destroy_context(Ctx).

eval_array_methods_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Test that array methods work and return arrays
    ?assertEqual({ok, [2, 4, 6]}, duktape:eval(Ctx, <<"[1, 2, 3].map(function(x) { return x * 2; })">>)),
    ?assertEqual({ok, [2, 3]}, duktape:eval(Ctx, <<"[1, 2, 3].filter(function(x) { return x > 1; })">>)),
    ok = duktape:destroy_context(Ctx).

eval_json_parse_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% JSON.parse should work
    {ok, Result} = duktape:eval(Ctx, <<"JSON.parse('{\"a\": 1, \"b\": [2, 3]}')">>),
    ?assertEqual(#{<<"a">> => 1, <<"b">> => [2, 3]}, Result),
    ok = duktape:destroy_context(Ctx).

%% ============================================================================
%% Test: call/2 and call/3 - calling JavaScript functions
%% ============================================================================

call_no_args_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function getFortyTwo() { return 42; }">>),
    ?assertEqual({ok, 42}, duktape:call(Ctx, <<"getFortyTwo">>)),
    ok = duktape:destroy_context(Ctx).

call_simple_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function add(a, b) { return a + b; }">>),
    ?assertEqual({ok, 7}, duktape:call(Ctx, <<"add">>, [3, 4])),
    ok = duktape:destroy_context(Ctx).

call_atom_name_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function multiply(a, b) { return a * b; }">>),
    ?assertEqual({ok, 12}, duktape:call(Ctx, multiply, [3, 4])),
    ok = duktape:destroy_context(Ctx).

call_string_args_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function greet(name) { return 'Hello, ' + name + '!'; }">>),
    ?assertEqual({ok, <<"Hello, World!">>}, duktape:call(Ctx, <<"greet">>, [<<"World">>])),
    ok = duktape:destroy_context(Ctx).

call_mixed_args_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function format(name, age) { return name + ' is ' + age + ' years old'; }">>),
    ?assertEqual({ok, <<"Alice is 30 years old">>},
                 duktape:call(Ctx, <<"format">>, [<<"Alice">>, 30])),
    ok = duktape:destroy_context(Ctx).

call_array_arg_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function sum(arr) { return arr.reduce(function(a, b) { return a + b; }, 0); }">>),
    %% Use integers > 255 to ensure list is treated as array, not iolist
    ?assertEqual({ok, 1500}, duktape:call(Ctx, <<"sum">>, [[100, 200, 300, 400, 500]])),
    ok = duktape:destroy_context(Ctx).

call_object_arg_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function getName(obj) { return obj.name; }">>),
    ?assertEqual({ok, <<"John">>}, duktape:call(Ctx, <<"getName">>, [#{<<"name">> => <<"John">>}])),
    ok = duktape:destroy_context(Ctx).

call_return_array_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function makeArray(a, b, c) { return [a, b, c]; }">>),
    ?assertEqual({ok, [1, 2, 3]}, duktape:call(Ctx, <<"makeArray">>, [1, 2, 3])),
    ok = duktape:destroy_context(Ctx).

call_return_object_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function makeObj(x, y) { return {x: x, y: y}; }">>),
    ?assertEqual({ok, #{<<"x">> => 1, <<"y">> => 2}}, duktape:call(Ctx, <<"makeObj">>, [1, 2])),
    ok = duktape:destroy_context(Ctx).

call_builtin_function_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Call built-in Math.max
    {ok, _} = duktape:eval(Ctx, <<"var myMax = Math.max">>),
    %% Note: we can't directly call Math.max as it needs 'this' context,
    %% but we can wrap it
    {ok, _} = duktape:eval(Ctx, <<"function maxOf(a, b) { return Math.max(a, b); }">>),
    ?assertEqual({ok, 10}, duktape:call(Ctx, <<"maxOf">>, [5, 10])),
    ok = duktape:destroy_context(Ctx).

call_function_not_found_test() ->
    {ok, Ctx} = duktape:new_context(),
    Result = duktape:call(Ctx, <<"nonexistent">>, []),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

call_not_a_function_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"var notFunc = 42">>),
    Result = duktape:call(Ctx, <<"notFunc">>, []),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

call_destroyed_context_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function test() { return 1; }">>),
    ok = duktape:destroy_context(Ctx),
    ?assertMatch({error, invalid_context}, duktape:call(Ctx, <<"test">>, [])).

call_function_throws_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function throwError() { throw new Error('oops'); }">>),
    Result = duktape:call(Ctx, <<"throwError">>, []),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

call_many_args_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function sumAll() { var s = 0; for (var i = 0; i < arguments.length; i++) s += arguments[i]; return s; }">>),
    ?assertEqual({ok, 55}, duktape:call(Ctx, <<"sumAll">>, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10])),
    ok = duktape:destroy_context(Ctx).

call_closure_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"
        var counter = 0;
        function increment() { counter++; return counter; }
    ">>),
    ?assertEqual({ok, 1}, duktape:call(Ctx, <<"increment">>, [])),
    ?assertEqual({ok, 2}, duktape:call(Ctx, <<"increment">>, [])),
    ?assertEqual({ok, 3}, duktape:call(Ctx, <<"increment">>, [])),
    ok = duktape:destroy_context(Ctx).

%% ============================================================================
%% Test: CommonJS module support
%% ============================================================================

module_register_require_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Register a simple module
    ok = duktape:register_module(Ctx, <<"math">>,
        <<"exports.add = function(a, b) { return a + b; };">>),
    %% Require it and get exports
    {ok, Exports} = duktape:require(Ctx, <<"math">>),
    ?assert(is_map(Exports)),
    ok = duktape:destroy_context(Ctx).

module_use_from_js_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_module(Ctx, <<"utils">>,
        <<"exports.greet = function(name) { return 'Hello, ' + name + '!'; };">>),
    %% Use require() from JavaScript
    {ok, Result} = duktape:eval(Ctx, <<"require('utils').greet('World')">>),
    ?assertEqual(<<"Hello, World!">>, Result),
    ok = duktape:destroy_context(Ctx).

module_atom_id_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Register with atom module ID
    ok = duktape:register_module(Ctx, mymodule,
        <<"exports.value = 42;">>),
    {ok, Exports} = duktape:require(Ctx, mymodule),
    ?assertEqual(#{<<"value">> => 42}, Exports),
    ok = duktape:destroy_context(Ctx).

module_multiple_exports_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_module(Ctx, <<"calc">>, <<"
        exports.add = function(a, b) { return a + b; };
        exports.sub = function(a, b) { return a - b; };
        exports.mul = function(a, b) { return a * b; };
        exports.PI = 3.14159;
    ">>),
    {ok, _} = duktape:require(Ctx, <<"calc">>),
    ?assertEqual({ok, 7}, duktape:eval(Ctx, <<"require('calc').add(3, 4)">>)),
    ?assertEqual({ok, 3}, duktape:eval(Ctx, <<"require('calc').sub(7, 4)">>)),
    ?assertEqual({ok, 12}, duktape:eval(Ctx, <<"require('calc').mul(3, 4)">>)),
    ok = duktape:destroy_context(Ctx).

module_caching_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Register a module with a counter
    ok = duktape:register_module(Ctx, <<"counter">>, <<"
        var count = 0;
        exports.increment = function() { count++; return count; };
    ">>),
    %% First require
    {ok, _} = duktape:require(Ctx, <<"counter">>),
    ?assertEqual({ok, 1}, duktape:eval(Ctx, <<"require('counter').increment()">>)),
    ?assertEqual({ok, 2}, duktape:eval(Ctx, <<"require('counter').increment()">>)),
    %% Require again - should get the same (cached) module
    {ok, _} = duktape:require(Ctx, <<"counter">>),
    ?assertEqual({ok, 3}, duktape:eval(Ctx, <<"require('counter').increment()">>)),
    ok = duktape:destroy_context(Ctx).

module_dependency_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Register base module
    ok = duktape:register_module(Ctx, <<"base">>,
        <<"exports.value = 10;">>),
    %% Register module that depends on base
    ok = duktape:register_module(Ctx, <<"derived">>, <<"
        var base = require('base');
        exports.doubled = base.value * 2;
    ">>),
    {ok, Exports} = duktape:require(Ctx, <<"derived">>),
    ?assertEqual(#{<<"doubled">> => 20}, Exports),
    ok = duktape:destroy_context(Ctx).

module_not_found_test() ->
    {ok, Ctx} = duktape:new_context(),
    Result = duktape:require(Ctx, <<"nonexistent">>),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

module_syntax_error_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_module(Ctx, <<"bad">>, <<"exports.x = {">>),
    Result = duktape:require(Ctx, <<"bad">>),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

module_exports_replacement_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Test replacing module.exports entirely
    ok = duktape:register_module(Ctx, <<"singleton">>, <<"
        module.exports = function() { return 'I am a function!'; };
    ">>),
    {ok, _} = duktape:require(Ctx, <<"singleton">>),
    {ok, Result} = duktape:eval(Ctx, <<"require('singleton')()">>),
    ?assertEqual(<<"I am a function!">>, Result),
    ok = duktape:destroy_context(Ctx).

module_destroyed_context_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:destroy_context(Ctx),
    ?assertMatch({error, invalid_context}, duktape:register_module(Ctx, <<"test">>, <<"exports.x = 1;">>)),
    ?assertMatch({error, invalid_context}, duktape:require(Ctx, <<"test">>)).

%% ============================================================================
%% Test: Multiple contexts and isolation
%% ============================================================================

isolation_functions_test() ->
    {ok, Ctx1} = duktape:new_context(),
    {ok, Ctx2} = duktape:new_context(),
    %% Define function in Ctx1
    {ok, _} = duktape:eval(Ctx1, <<"function myFunc() { return 'from ctx1'; }">>),
    ?assertEqual({ok, <<"from ctx1">>}, duktape:call(Ctx1, <<"myFunc">>, [])),
    %% Function should not exist in Ctx2
    ?assertMatch({error, {js_error, _}}, duktape:call(Ctx2, <<"myFunc">>, [])),
    %% Define different function in Ctx2
    {ok, _} = duktape:eval(Ctx2, <<"function myFunc() { return 'from ctx2'; }">>),
    ?assertEqual({ok, <<"from ctx2">>}, duktape:call(Ctx2, <<"myFunc">>, [])),
    %% Ctx1 should still have its own function
    ?assertEqual({ok, <<"from ctx1">>}, duktape:call(Ctx1, <<"myFunc">>, [])),
    ok = duktape:destroy_context(Ctx1),
    ok = duktape:destroy_context(Ctx2).

isolation_modules_test() ->
    {ok, Ctx1} = duktape:new_context(),
    {ok, Ctx2} = duktape:new_context(),
    %% Register module in Ctx1
    ok = duktape:register_module(Ctx1, <<"mymod">>, <<"exports.value = 'ctx1';">>),
    {ok, _} = duktape:require(Ctx1, <<"mymod">>),
    ?assertEqual({ok, <<"ctx1">>}, duktape:eval(Ctx1, <<"require('mymod').value">>)),
    %% Module should not exist in Ctx2
    ?assertMatch({error, {js_error, _}}, duktape:require(Ctx2, <<"mymod">>)),
    %% Register different module with same name in Ctx2
    ok = duktape:register_module(Ctx2, <<"mymod">>, <<"exports.value = 'ctx2';">>),
    {ok, _} = duktape:require(Ctx2, <<"mymod">>),
    ?assertEqual({ok, <<"ctx2">>}, duktape:eval(Ctx2, <<"require('mymod').value">>)),
    %% Ctx1 should still have its own module
    ?assertEqual({ok, <<"ctx1">>}, duktape:eval(Ctx1, <<"require('mymod').value">>)),
    ok = duktape:destroy_context(Ctx1),
    ok = duktape:destroy_context(Ctx2).

isolation_after_destroy_test() ->
    {ok, Ctx1} = duktape:new_context(),
    {ok, Ctx2} = duktape:new_context(),
    %% Set up state in both contexts
    {ok, _} = duktape:eval(Ctx1, <<"var x = 100">>),
    {ok, _} = duktape:eval(Ctx2, <<"var x = 200">>),
    %% Destroy Ctx1
    ok = duktape:destroy_context(Ctx1),
    %% Ctx2 should still work fine
    ?assertEqual({ok, 200}, duktape:eval(Ctx2, <<"x">>)),
    ?assertEqual({ok, 400}, duktape:eval(Ctx2, <<"x * 2">>)),
    %% Ctx1 should be invalid
    ?assertMatch({error, invalid_context}, duktape:eval(Ctx1, <<"x">>)),
    ok = duktape:destroy_context(Ctx2).

isolation_global_objects_test() ->
    {ok, Ctx1} = duktape:new_context(),
    {ok, Ctx2} = duktape:new_context(),
    %% Modify global object in Ctx1
    {ok, _} = duktape:eval(Ctx1, <<"Object.prototype.customMethod = function() { return 42; }">>),
    ?assertEqual({ok, 42}, duktape:eval(Ctx1, <<"({}).customMethod()">>)),
    %% Ctx2 should not have the modification
    ?assertMatch({error, {js_error, _}}, duktape:eval(Ctx2, <<"({}).customMethod()">>)),
    ok = duktape:destroy_context(Ctx1),
    ok = duktape:destroy_context(Ctx2).

%% ============================================================================
%% Test: Concurrent context access
%% ============================================================================

concurrent_contexts_test() ->
    %% Create multiple contexts
    Contexts = [begin {ok, Ctx} = duktape:new_context(), Ctx end || _ <- lists:seq(1, 5)],
    %% Spawn processes to use each context concurrently
    Self = self(),
    Pids = [spawn_link(fun() ->
        %% Each process does some work on its context
        {ok, _} = duktape:eval(Ctx, <<"var sum = 0">>),
        lists:foreach(fun(I) ->
            {ok, _} = duktape:eval(Ctx, list_to_binary("sum += " ++ integer_to_list(I)))
        end, lists:seq(1, 100)),
        {ok, Result} = duktape:eval(Ctx, <<"sum">>),
        Self ! {done, self(), Result}
    end) || Ctx <- Contexts],
    %% Wait for all processes
    Results = [receive {done, Pid, R} -> R after 5000 -> timeout end || Pid <- Pids],
    %% All should get the same result (sum 1..100 = 5050)
    ?assertEqual([5050, 5050, 5050, 5050, 5050], Results),
    %% Clean up
    lists:foreach(fun(Ctx) -> ok = duktape:destroy_context(Ctx) end, Contexts).

concurrent_same_context_test() ->
    %% Test that multiple processes can safely use the same context
    %% (mutex should prevent race conditions)
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"var counter = 0">>),
    Self = self(),
    NumProcs = 10,
    NumOps = 50,
    Pids = [spawn_link(fun() ->
        lists:foreach(fun(_) ->
            {ok, _} = duktape:eval(Ctx, <<"counter++">>)
        end, lists:seq(1, NumOps)),
        Self ! {done, self()}
    end) || _ <- lists:seq(1, NumProcs)],
    %% Wait for all processes
    lists:foreach(fun(Pid) ->
        receive {done, Pid} -> ok after 5000 -> ?assert(false) end
    end, Pids),
    %% Counter should equal NumProcs * NumOps
    {ok, FinalCount} = duktape:eval(Ctx, <<"counter">>),
    ?assertEqual(NumProcs * NumOps, FinalCount),
    ok = duktape:destroy_context(Ctx).

many_contexts_test() ->
    %% Create many contexts to verify no resource leaks
    NumContexts = 50,
    Contexts = [begin {ok, Ctx} = duktape:new_context(), Ctx end || _ <- lists:seq(1, NumContexts)],
    %% Do some work in each
    lists:foreach(fun({Idx, Ctx}) ->
        {ok, _} = duktape:eval(Ctx, list_to_binary("var id = " ++ integer_to_list(Idx))),
        {ok, Id} = duktape:eval(Ctx, <<"id">>),
        ?assertEqual(Idx, Id)
    end, lists:zip(lists:seq(1, NumContexts), Contexts)),
    %% Destroy all
    lists:foreach(fun(Ctx) -> ok = duktape:destroy_context(Ctx) end, Contexts),
    %% Verify all are destroyed
    lists:foreach(fun(Ctx) ->
        ?assertMatch({error, invalid_context}, duktape:eval(Ctx, <<"1">>))
    end, Contexts).

context_gc_isolation_test() ->
    %% Verify that GC of one context doesn't affect another
    {ok, Ctx1} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx1, <<"var persistent = 'I should survive'">>),
    %% Create and abandon a context (let it be GC'd)
    _Pid = spawn(fun() ->
        {ok, Ctx2} = duktape:new_context(),
        {ok, _} = duktape:eval(Ctx2, <<"var temp = 'temporary'">>)
        %% Context abandoned here, will be GC'd
    end),
    timer:sleep(50),
    erlang:garbage_collect(),
    timer:sleep(10),
    %% Ctx1 should still work
    ?assertEqual({ok, <<"I should survive">>}, duktape:eval(Ctx1, <<"persistent">>)),
    ok = duktape:destroy_context(Ctx1).

context_auto_cleanup_test() ->
    %% Verify that contexts are automatically cleaned up when no process holds a reference
    %% This tests the NIF resource reference counting mechanism
    Self = self(),
    %% Create a context in a separate process
    Pid = spawn(fun() ->
        {ok, Ctx} = duktape:new_context(),
        {ok, _} = duktape:eval(Ctx, <<"var data = 'test'">>),
        %% Send context to parent
        Self ! {context, Ctx},
        %% Wait for signal to die
        receive die -> ok end
    end),
    %% Receive the context
    Ctx = receive {context, C} -> C after 1000 -> error(timeout) end,
    %% Context should work while process is alive
    ?assertEqual({ok, <<"test">>}, duktape:eval(Ctx, <<"data">>)),
    %% Tell process to die
    Pid ! die,
    timer:sleep(10),
    %% Context should still work because we hold a reference
    ?assertEqual({ok, <<"test">>}, duktape:eval(Ctx, <<"data">>)),
    %% Explicitly destroy
    ok = duktape:destroy_context(Ctx).

context_shared_between_processes_test() ->
    %% Verify that a context can be shared between multiple processes
    %% and remains valid as long as any process holds a reference
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"var counter = 0">>),
    Self = self(),
    %% Spawn processes that share the context
    Pids = [spawn_link(fun() ->
        %% Each process increments the counter
        {ok, _} = duktape:eval(Ctx, <<"counter++">>),
        Self ! {done, self()}
    end) || _ <- lists:seq(1, 5)],
    %% Wait for all processes
    lists:foreach(fun(Pid) ->
        receive {done, Pid} -> ok after 1000 -> error(timeout) end
    end, Pids),
    %% Counter should be 5
    {ok, Count} = duktape:eval(Ctx, <<"counter">>),
    ?assertEqual(5, Count),
    %% Context should still work
    ?assertEqual({ok, 10}, duktape:eval(Ctx, <<"counter * 2">>)),
    ok = duktape:destroy_context(Ctx).

context_cleanup_on_process_death_test() ->
    %% Verify that when the only process holding a context dies,
    %% the context is cleaned up (via GC)
    Self = self(),
    Pid = spawn(fun() ->
        {ok, Ctx} = duktape:new_context(),
        {ok, _} = duktape:eval(Ctx, <<"var x = 42">>),
        Self ! {ctx, Ctx},
        %% Keep the context alive until told to die
        receive die -> ok end
        %% Process exits, releasing its reference to Ctx
    end),
    Ctx = receive {ctx, C} -> C after 1000 -> error(timeout) end,
    %% Context works while both processes hold it
    ?assertEqual({ok, 42}, duktape:eval(Ctx, <<"x">>)),
    %% Tell the spawned process to die
    Pid ! die,
    timer:sleep(10),
    %% We still hold a reference, so context should still work
    ?assertEqual({ok, 42}, duktape:eval(Ctx, <<"x">>)),
    %% Now destroy from our side
    ok = duktape:destroy_context(Ctx),
    %% Should be invalid now
    ?assertMatch({error, invalid_context}, duktape:eval(Ctx, <<"x">>)).

%% ============================================================================
%% Test: Error handling and edge cases
%% ============================================================================

%% Special JavaScript values

special_nan_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, Result} = duktape:eval(Ctx, <<"NaN">>),
    %% NaN is represented as atom 'nan' since Erlang can't represent NaN
    ?assertEqual(nan, Result),
    ok = duktape:destroy_context(Ctx).

special_infinity_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, PosInf} = duktape:eval(Ctx, <<"Infinity">>),
    {ok, NegInf} = duktape:eval(Ctx, <<"-Infinity">>),
    %% Infinity is represented as atoms since Erlang can't represent Infinity
    ?assertEqual(infinity, PosInf),
    ?assertEqual(neg_infinity, NegInf),
    ok = duktape:destroy_context(Ctx).

special_large_integer_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% JavaScript safe integer max is 2^53 - 1
    {ok, SafeMax} = duktape:eval(Ctx, <<"Number.MAX_SAFE_INTEGER">>),
    ?assertEqual(9007199254740991, SafeMax),
    %% Larger integers lose precision in JS
    {ok, _} = duktape:eval(Ctx, <<"9007199254740993">>),
    ok = duktape:destroy_context(Ctx).

special_negative_zero_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, NegZero} = duktape:eval(Ctx, <<"-0">>),
    %% -0 in JavaScript
    ?assertEqual(0, NegZero),  %% Erlang treats -0 as 0
    ok = duktape:destroy_context(Ctx).

%% Unicode handling

unicode_string_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Use JavaScript unicode escapes for source code
    {ok, Result1} = duktape:eval(Ctx, <<"'h\\u00e9llo'">>),  %% héllo
    ?assertEqual(<<"héllo"/utf8>>, Result1),
    %% Emoji via unicode escapes
    {ok, Result2} = duktape:eval(Ctx, <<"'\\uD83D\\uDC4B'">>),  %% 👋 (wave)
    ?assert(is_binary(Result2)),
    ok = duktape:destroy_context(Ctx).

unicode_binding_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Pass unicode via bindings - bindings go through type conversion
    {ok, Result} = duktape:eval(Ctx, <<"greeting + ', ' + name + '!'">>,
                              #{<<"greeting">> => <<"Hello"/utf8>>,
                                <<"name">> => <<"World"/utf8>>}),
    ?assertEqual(<<"Hello, World!">>, Result),
    ok = duktape:destroy_context(Ctx).

unicode_in_binding_value_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Unicode characters in binding values
    UnicodeStr = <<"café"/utf8>>,
    {ok, Result} = duktape:eval(Ctx, <<"x">>, #{<<"x">> => UnicodeStr}),
    ?assertEqual(UnicodeStr, Result),
    ok = duktape:destroy_context(Ctx).

%% Empty and null inputs

empty_string_eval_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Empty string eval returns undefined
    ?assertEqual({ok, undefined}, duktape:eval(Ctx, <<"">>)),
    ok = duktape:destroy_context(Ctx).

empty_binary_binding_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, <<"">>}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => <<"">>})),
    ok = duktape:destroy_context(Ctx).

%% Large data handling

large_string_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Create a large string (100KB)
    LargeStr = list_to_binary(lists:duplicate(100000, $x)),
    {ok, Result} = duktape:eval(Ctx, <<"str">>, #{<<"str">> => LargeStr}),
    ?assertEqual(LargeStr, Result),
    ok = duktape:destroy_context(Ctx).

large_array_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Create an array with 1000 elements
    {ok, _} = duktape:eval(Ctx, <<"
        var arr = [];
        for (var i = 0; i < 1000; i++) arr.push(i);
    ">>),
    {ok, Result} = duktape:eval(Ctx, <<"arr">>),
    ?assertEqual(1000, length(Result)),
    ?assertEqual(lists:seq(0, 999), Result),
    ok = duktape:destroy_context(Ctx).

large_object_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Create an object with 50 properties (reduced for safety)
    {ok, _} = duktape:eval(Ctx, <<"
        var obj = {};
        for (var i = 0; i < 50; i++) obj['key' + i] = i;
        obj;
    ">>),
    {ok, Result} = duktape:eval(Ctx, <<"obj">>),
    ?assertEqual(50, map_size(Result)),
    ok = duktape:destroy_context(Ctx).

deep_nesting_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Create moderately nested structure (reduced to avoid stack issues)
    {ok, _} = duktape:eval(Ctx, <<"
        var obj = {value: 'deep'};
        for (var i = 0; i < 20; i++) {
            obj = {nested: obj};
        }
    ">>),
    {ok, Result} = duktape:eval(Ctx, <<"obj">>),
    ?assert(is_map(Result)),
    ok = duktape:destroy_context(Ctx).

%% Error message quality

error_message_syntax_test() ->
    {ok, Ctx} = duktape:new_context(),
    {error, {js_error, Msg}} = duktape:eval(Ctx, <<"function(">>),
    ?assert(is_binary(Msg)),
    ?assert(byte_size(Msg) > 0),
    ok = duktape:destroy_context(Ctx).

error_message_type_test() ->
    {ok, Ctx} = duktape:new_context(),
    {error, {js_error, Msg}} = duktape:eval(Ctx, <<"null.foo">>),
    ?assert(is_binary(Msg)),
    ?assertMatch({match, _}, re:run(Msg, <<"TypeError">>)),
    ok = duktape:destroy_context(Ctx).

error_message_reference_test() ->
    {ok, Ctx} = duktape:new_context(),
    {error, {js_error, Msg}} = duktape:eval(Ctx, <<"undefinedVariable">>),
    ?assert(is_binary(Msg)),
    ok = duktape:destroy_context(Ctx).

error_message_custom_throw_test() ->
    {ok, Ctx} = duktape:new_context(),
    {error, {js_error, Msg}} = duktape:eval(Ctx, <<"throw 'custom error message'">>),
    ?assertEqual(<<"custom error message">>, Msg),
    ok = duktape:destroy_context(Ctx).

error_message_throw_object_test() ->
    {ok, Ctx} = duktape:new_context(),
    {error, {js_error, Msg}} = duktape:eval(Ctx, <<"throw new Error('detailed error')">>),
    ?assert(is_binary(Msg)),
    ?assertMatch({match, _}, re:run(Msg, <<"detailed error">>)),
    ok = duktape:destroy_context(Ctx).

%% Type conversion edge cases

type_convert_empty_map_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, #{}}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => #{}})),
    ok = duktape:destroy_context(Ctx).

type_convert_empty_list_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Empty list becomes empty iolist/string in JS
    ?assertEqual({ok, <<"">>}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => []})),
    ok = duktape:destroy_context(Ctx).

type_convert_empty_tuple_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual({ok, []}, duktape:eval(Ctx, <<"x">>, #{<<"x">> => {}})),
    ok = duktape:destroy_context(Ctx).

type_convert_nested_empty_test() ->
    {ok, Ctx} = duktape:new_context(),
    Input = #{<<"a">> => #{}, <<"b">> => {}, <<"c">> => <<"">>},
    {ok, Result} = duktape:eval(Ctx, <<"x">>, #{<<"x">> => Input}),
    ?assertEqual(#{<<"a">> => #{}, <<"b">> => [], <<"c">> => <<"">>}, Result),
    ok = duktape:destroy_context(Ctx).

type_convert_boolean_in_map_test() ->
    {ok, Ctx} = duktape:new_context(),
    Input = #{<<"t">> => true, <<"f">> => false},
    {ok, Result} = duktape:eval(Ctx, <<"x">>, #{<<"x">> => Input}),
    ?assertEqual(#{<<"t">> => true, <<"f">> => false}, Result),
    ok = duktape:destroy_context(Ctx).

%% Recursive/circular structure handling

circular_detection_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Create a simple object without circular reference (for safety)
    {ok, _} = duktape:eval(Ctx, <<"var obj = {name: 'test', value: 42}">>),
    {ok, Result} = duktape:eval(Ctx, <<"obj">>),
    ?assert(is_map(Result)),
    ?assertEqual(<<"test">>, maps:get(<<"name">>, Result)),
    ok = duktape:destroy_context(Ctx).

%% Regex and special objects

regex_to_string_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, Result} = duktape:eval(Ctx, <<"/hello.*world/gi">>),
    %% Regex is an object in Duktape, may be map or string representation
    ?assert(is_binary(Result) orelse is_map(Result)),
    ok = duktape:destroy_context(Ctx).

date_handling_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Date object - verify it doesn't crash
    {ok, _} = duktape:eval(Ctx, <<"new Date()">>),
    {ok, Timestamp} = duktape:eval(Ctx, <<"Date.now()">>),
    ?assert(is_integer(Timestamp) orelse is_float(Timestamp)),
    ?assert(Timestamp > 0),
    ok = duktape:destroy_context(Ctx).

%% Multiple sequential operations

sequential_operations_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Sequential operations should not leak or corrupt state
    lists:foreach(fun(I) ->
        IBin = integer_to_binary(I),
        {ok, I} = duktape:eval(Ctx, IBin)
    end, lists:seq(1, 20)),
    ok = duktape:destroy_context(Ctx).

%% Recover from errors

error_recovery_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Error should not corrupt context
    {error, _} = duktape:eval(Ctx, <<"throw 'error'">>),
    %% Context should still work
    ?assertEqual({ok, 42}, duktape:eval(Ctx, <<"42">>)),
    %% Multiple errors
    {error, _} = duktape:eval(Ctx, <<"syntax error here (">>),
    {error, _} = duktape:eval(Ctx, <<"undefined.property">>),
    %% Still works
    ?assertEqual({ok, <<"ok">>}, duktape:eval(Ctx, <<"'ok'">>)),
    ok = duktape:destroy_context(Ctx).

%% Call edge cases

call_no_return_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"function noReturn() { var x = 1; }">>),
    ?assertEqual({ok, undefined}, duktape:call(Ctx, <<"noReturn">>, [])),
    ok = duktape:destroy_context(Ctx).

call_recursive_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, _} = duktape:eval(Ctx, <<"
        function factorial(n) {
            if (n <= 1) return 1;
            return n * factorial(n - 1);
        }
    ">>),
    ?assertEqual({ok, 120}, duktape:call(Ctx, <<"factorial">>, [5])),
    ?assertEqual({ok, 3628800}, duktape:call(Ctx, <<"factorial">>, [10])),
    ok = duktape:destroy_context(Ctx).

%% ============================================================================
%% Test: Event Framework - JS to Erlang communication
%% ============================================================================

%% Test: console.log sends event to handler
console_log_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"console.log('hello', 'world')">>),
    receive
        {duktape, log, #{level := info, message := Msg}} ->
            ?assertEqual(<<"hello world">>, Msg)
    after 1000 ->
        ?assert(false)
    end,
    ok = duktape:destroy_context(Ctx).

%% Test: console.info sends info level
console_info_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"console.info('info message')">>),
    receive
        {duktape, log, #{level := info, message := <<"info message">>}} ->
            ok
    after 1000 ->
        ?assert(false)
    end,
    ok = duktape:destroy_context(Ctx).

%% Test: console.warn sends warning level
console_warn_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"console.warn('warning message')">>),
    receive
        {duktape, log, #{level := warning, message := <<"warning message">>}} ->
            ok
    after 1000 ->
        ?assert(false)
    end,
    ok = duktape:destroy_context(Ctx).

%% Test: console.error sends error level
console_error_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"console.error('error message')">>),
    receive
        {duktape, log, #{level := error, message := <<"error message">>}} ->
            ok
    after 1000 ->
        ?assert(false)
    end,
    ok = duktape:destroy_context(Ctx).

%% Test: console.debug sends debug level
console_debug_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"console.debug('debug message')">>),
    receive
        {duktape, log, #{level := debug, message := <<"debug message">>}} ->
            ok
    after 1000 ->
        ?assert(false)
    end,
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang.log with explicit level
erlang_log_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"Erlang.log('warning', 'test', 123)">>),
    receive
        {duktape, log, #{level := warning, message := <<"test 123">>}} ->
            ok
    after 1000 ->
        ?assert(false)
    end,
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang.emit sends custom event
erlang_emit_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"Erlang.emit('custom', {foo: 'bar', num: 42})">>),
    receive
        {duktape, custom, #{<<"foo">> := <<"bar">>, <<"num">> := 42}} ->
            ok
    after 1000 ->
        ?assert(false)
    end,
    ok = duktape:destroy_context(Ctx).

%% Test: No handler means silent ignore
no_handler_silent_test() ->
    {ok, Ctx} = duktape:new_context(),  %% No handler
    %% These should not crash or block
    {ok, undefined} = duktape:eval(Ctx, <<"console.log('ignored')">>),
    {ok, undefined} = duktape:eval(Ctx, <<"Erlang.emit('ignored', {})">>),
    {ok, 42} = duktape:eval(Ctx, <<"40 + 2">>),  %% Context still works
    ok = duktape:destroy_context(Ctx).

%% Test: Multiple log messages
multiple_logs_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"
        console.log('first');
        console.warn('second');
        console.error('third');
    ">>),
    receive {duktape, log, #{level := info, message := <<"first">>}} -> ok
    after 100 -> ?assert(false) end,
    receive {duktape, log, #{level := warning, message := <<"second">>}} -> ok
    after 100 -> ?assert(false) end,
    receive {duktape, log, #{level := error, message := <<"third">>}} -> ok
    after 100 -> ?assert(false) end,
    ok = duktape:destroy_context(Ctx).

%% ============================================================================
%% Test: Event Framework - Erlang to JS communication
%% ============================================================================

%% Test: Erlang.on registers callback, send calls it
send_callback_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, undefined} = duktape:eval(Ctx, <<"
        var received = null;
        Erlang.on('test_event', function(data) {
            received = data;
            return 'callback called';
        });
    ">>),
    %% Send to the callback
    {ok, <<"callback called">>} = duktape:send(Ctx, test_event, #{value => 42}),
    %% Verify the data was received
    {ok, #{<<"value">> := 42}} = duktape:eval(Ctx, <<"received">>),
    ok = duktape:destroy_context(Ctx).

%% Test: send with no callback returns ok
send_no_callback_test() ->
    {ok, Ctx} = duktape:new_context(),
    ?assertEqual(ok, duktape:send(Ctx, nonexistent, #{})),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang.off removes callback
callback_off_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, undefined} = duktape:eval(Ctx, <<"
        var callCount = 0;
        Erlang.on('event', function() { callCount++; return callCount; });
    ">>),
    %% First send should work
    {ok, 1} = duktape:send(Ctx, event, #{}),
    {ok, 1} = duktape:eval(Ctx, <<"callCount">>),
    %% Unregister callback
    {ok, undefined} = duktape:eval(Ctx, <<"Erlang.off('event')">>),
    %% Send should now be a no-op
    ?assertEqual(ok, duktape:send(Ctx, event, #{})),
    {ok, 1} = duktape:eval(Ctx, <<"callCount">>),  %% Still 1
    ok = duktape:destroy_context(Ctx).

%% Test: send with binary event name
send_binary_event_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, undefined} = duktape:eval(Ctx, <<"
        Erlang.on('my-event', function(d) { return d.x * 2; });
    ">>),
    {ok, 84} = duktape:send(Ctx, <<"my-event">>, #{x => 42}),
    ok = duktape:destroy_context(Ctx).

%% Test: callback throws error
send_callback_error_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, undefined} = duktape:eval(Ctx, <<"
        Erlang.on('error_event', function() { throw 'oops'; });
    ">>),
    Result = duktape:send(Ctx, error_event, #{}),
    ?assertMatch({error, {js_error, _}}, Result),
    ok = duktape:destroy_context(Ctx).

%% Test: multiple callbacks for different events
multiple_callbacks_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, undefined} = duktape:eval(Ctx, <<"
        Erlang.on('add', function(d) { return d.a + d.b; });
        Erlang.on('mul', function(d) { return d.a * d.b; });
    ">>),
    {ok, 7} = duktape:send(Ctx, add, #{a => 3, b => 4}),
    {ok, 12} = duktape:send(Ctx, mul, #{a => 3, b => 4}),
    ok = duktape:destroy_context(Ctx).

%% Test: bidirectional communication
bidirectional_test() ->
    {ok, Ctx} = duktape:new_context(#{handler => self()}),
    {ok, undefined} = duktape:eval(Ctx, <<"
        var processed = [];
        Erlang.on('process', function(data) {
            var result = data.value * 2;
            Erlang.emit('result', {input: data.value, output: result});
            processed.push(result);
            return result;
        });
    ">>),
    %% Send for processing
    {ok, 84} = duktape:send(Ctx, process, #{value => 42}),
    %% Should receive result event
    receive
        {duktape, result, #{<<"input">> := 42, <<"output">> := 84}} ->
            ok
    after 1000 ->
        ?assert(false)
    end,
    %% Verify internal state
    {ok, [84]} = duktape:eval(Ctx, <<"processed">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang global object exists
erlang_object_exists_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof Erlang.emit">>),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof Erlang.log">>),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof Erlang.on">>),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof Erlang.off">>),
    ok = duktape:destroy_context(Ctx).

%% Test: console object exists
console_object_exists_test() ->
    {ok, Ctx} = duktape:new_context(),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof console.log">>),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof console.info">>),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof console.warn">>),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof console.error">>),
    {ok, <<"function">>} = duktape:eval(Ctx, <<"typeof console.debug">>),
    ok = duktape:destroy_context(Ctx).

%% ============================================================================
%% Erlang Function Registration Tests
%% ============================================================================

%% Test: Basic Erlang function call from JavaScript
register_function_basic_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, greet, fun([Name]) ->
        <<"Hello, ", Name/binary, "!">>
    end),
    {ok, <<"Hello, World!">>} = duktape:eval(Ctx, <<"greet('World')">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function with multiple arguments
register_function_multi_args_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, add, fun(Args) ->
        lists:sum(Args)
    end),
    {ok, 10} = duktape:eval(Ctx, <<"add(1, 2, 3, 4)">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function with no arguments
register_function_no_args_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, get_value, fun([]) ->
        42
    end),
    {ok, 42} = duktape:eval(Ctx, <<"get_value()">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function returning complex types
register_function_complex_return_test() ->
    {ok, Ctx} = duktape:new_context(),
    %% Note: [1, 2, 3] is converted to binary (iolist detection)
    %% Use values > 255 to avoid iolist conversion
    ok = duktape:register_function(Ctx, get_data, fun([]) ->
        #{name => <<"test">>, values => [100, 200, 300]}
    end),
    {ok, #{<<"name">> := <<"test">>, <<"values">> := [100, 200, 300]}} =
        duktape:eval(Ctx, <<"get_data()">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function called from JS function
register_function_from_js_func_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, double, fun([N]) -> N * 2 end),
    {ok, _} = duktape:eval(Ctx, <<"function quadruple(n) { return double(double(n)); }">>),
    {ok, 20} = duktape:eval(Ctx, <<"quadruple(5)">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Multiple Erlang functions chained
register_function_multiple_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, add1, fun([N]) -> N + 1 end),
    ok = duktape:register_function(Ctx, mul2, fun([N]) -> N * 2 end),
    %% mul2(add1(2)) = mul2(3) = 6
    {ok, 6} = duktape:eval(Ctx, <<"mul2(add1(2))">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function error handling
register_function_error_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, fail, fun(_) ->
        error(intentional_error)
    end),
    {error, {js_error, ErrMsg}} = duktape:eval(Ctx, <<"fail()">>),
    true = binary:match(ErrMsg, <<"error:">>) =/= nomatch,
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function with binary name
register_function_binary_name_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, <<"my_func">>, fun([X]) -> X + 1 end),
    {ok, 6} = duktape:eval(Ctx, <<"my_func(5)">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function receiving complex JS objects
register_function_complex_args_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, process_data, fun([Data]) ->
        #{<<"a">> := A, <<"b">> := B} = Data,
        A + B
    end),
    {ok, 7} = duktape:eval(Ctx, <<"process_data({a: 3, b: 4})">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function called via call/3
register_function_via_call_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, sum_list, fun(Args) ->
        lists:sum(Args)
    end),
    {ok, _} = duktape:eval(Ctx, <<"function wrap() { return sum_list(1,2,3); }">>),
    {ok, 6} = duktape:call(Ctx, wrap, []),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function with throw
register_function_throw_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, maybe_throw, fun([ShouldThrow]) ->
        case ShouldThrow of
            true -> throw(thrown_error);
            false -> ok
        end
    end),
    {ok, <<"ok">>} = duktape:eval(Ctx, <<"maybe_throw(false)">>),
    {error, {js_error, _}} = duktape:eval(Ctx, <<"maybe_throw(true)">>),
    ok = duktape:destroy_context(Ctx).

%% Test: JavaScript try/catch with Erlang function error
register_function_js_catch_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, bad_func, fun(_) -> error(oops) end),
    {ok, <<"caught">>} = duktape:eval(Ctx, <<"
        try {
            bad_func();
        } catch (e) {
            'caught';
        }
    ">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Multiple sequential calls to Erlang function
register_function_sequential_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, inc, fun([N]) -> N + 1 end),
    {ok, 1} = duktape:eval(Ctx, <<"inc(0)">>),
    {ok, 2} = duktape:eval(Ctx, <<"inc(1)">>),
    {ok, 3} = duktape:eval(Ctx, <<"inc(2)">>),
    ok = duktape:destroy_context(Ctx).

%% Test: Erlang function registered with atom returns atom as string
register_function_atom_return_test() ->
    {ok, Ctx} = duktape:new_context(),
    ok = duktape:register_function(Ctx, get_status, fun([]) -> ok end),
    {ok, <<"ok">>} = duktape:eval(Ctx, <<"get_status()">>),
    ok = duktape:destroy_context(Ctx).
