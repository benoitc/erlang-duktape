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
