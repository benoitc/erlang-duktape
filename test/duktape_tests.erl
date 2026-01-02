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
    %% Create a context
    Result = duktape:new_context(),
    ?assertMatch({ok, _}, Result),
    {ok, Ctx} = Result,
    ?assert(is_reference(Ctx)),

    %% Destroy the context
    ?assertEqual(ok, duktape:destroy_context(Ctx)).

context_double_destroy_test() ->
    %% Create a context
    {ok, Ctx} = duktape:new_context(),

    %% Destroy it twice - should be idempotent
    ?assertEqual(ok, duktape:destroy_context(Ctx)),
    ?assertEqual(ok, duktape:destroy_context(Ctx)).

context_multiple_test() ->
    %% Create multiple contexts
    {ok, Ctx1} = duktape:new_context(),
    {ok, Ctx2} = duktape:new_context(),
    {ok, Ctx3} = duktape:new_context(),

    %% They should all be different references
    ?assertNotEqual(Ctx1, Ctx2),
    ?assertNotEqual(Ctx2, Ctx3),
    ?assertNotEqual(Ctx1, Ctx3),

    %% Destroy them all
    ?assertEqual(ok, duktape:destroy_context(Ctx1)),
    ?assertEqual(ok, duktape:destroy_context(Ctx2)),
    ?assertEqual(ok, duktape:destroy_context(Ctx3)).

%% ============================================================================
%% Test: Context cleanup when process dies
%% ============================================================================

context_gc_cleanup_test() ->
    %% Spawn a process that creates a context and then dies
    Self = self(),
    Pid = spawn(fun() ->
        {ok, Ctx} = duktape:new_context(),
        Self ! {context_created, Ctx},
        %% Exit without explicitly destroying
        ok
    end),

    %% Wait for the context to be created
    Ctx = receive
        {context_created, C} -> C
    after 1000 ->
        ?assert(false)
    end,

    %% Wait for the process to die
    Ref = monitor(process, Pid),
    receive
        {'DOWN', Ref, process, Pid, _} -> ok
    after 1000 ->
        ?assert(false)
    end,

    %% Force garbage collection
    erlang:garbage_collect(),
    timer:sleep(10),

    %% The context should still be a valid reference type
    %% (but the underlying Duktape heap should be cleaned up)
    ?assert(is_reference(Ctx)),

    %% Destroying an already-GC'd context should still be safe
    %% (it's idempotent and handles already-destroyed contexts)
    ?assertEqual(ok, duktape:destroy_context(Ctx)).

context_badarg_test() ->
    %% Passing invalid argument should return error
    ?assertMatch({error, badarg}, duktape:destroy_context(not_a_context)),
    ?assertMatch({error, badarg}, duktape:destroy_context(123)),
    ?assertMatch({error, badarg}, duktape:destroy_context(<<"binary">>)).
