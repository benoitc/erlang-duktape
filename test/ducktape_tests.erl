%%% -*- erlang -*-
%%%
%%% Copyright (c) 2025 Benoit Chesneau
%%%
%%% Licensed under the Apache License, Version 2.0

-module(ducktape_tests).

-include_lib("eunit/include/eunit.hrl").

%% Test that NIF loads and info/0 works
nif_load_test() ->
    Result = ducktape:info(),
    ?assertMatch({ok, _}, Result),
    {ok, Info} = Result,
    ?assert(is_list(Info)),
    ?assertEqual("ducktape nif loaded", Info).
