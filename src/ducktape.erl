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

%% @doc Ducktape - Duktape JavaScript engine for Erlang
-module(ducktape).

-export([info/0]).

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
                    filename:join([filename:dirname(Filename), "../priv", "ducktape"]);
                _ ->
                    filename:join("../priv", "ducktape")
            end;
        Dir ->
            filename:join(Dir, "ducktape")
    end,
    erlang:load_nif(SoName, application:get_all_env(ducktape)).

%% @doc Get NIF information. Used to verify NIF is loaded correctly.
-spec info() -> {ok, string()}.
info() ->
    nif_info().

%% Internal NIF stubs
nif_info() -> ?nif_stub.
