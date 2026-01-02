/*
 * Ducktape Erlang NIF - JavaScript engine for Erlang
 *
 * Copyright (c) 2025 Benoit Chesneau
 * Licensed under the Apache License, Version 2.0
 */

#include <string.h>
#include "erl_nif.h"

/* Atoms */
static ERL_NIF_TERM atom_ok;
static ERL_NIF_TERM atom_error;

/* NIF initialization */
static int
on_load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info)
{
    (void)priv_data;
    (void)load_info;

    /* Initialize atoms */
    atom_ok = enif_make_atom(env, "ok");
    atom_error = enif_make_atom(env, "error");

    return 0;
}

static int
on_upgrade(ErlNifEnv *env, void **priv_data, void **old_priv_data, ERL_NIF_TERM load_info)
{
    (void)env;
    (void)priv_data;
    (void)old_priv_data;
    (void)load_info;
    return 0;
}

static void
on_unload(ErlNifEnv *env, void *priv_data)
{
    (void)env;
    (void)priv_data;
}

/* Stub function to verify NIF loads correctly */
static ERL_NIF_TERM
nif_info(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;
    (void)argv;

    return enif_make_tuple2(env, atom_ok,
        enif_make_string(env, "ducktape nif loaded", ERL_NIF_LATIN1));
}

/* NIF function table */
static ErlNifFunc nif_funcs[] = {
    {"nif_info", 0, nif_info, 0}
};

ERL_NIF_INIT(ducktape, nif_funcs, on_load, NULL, on_upgrade, on_unload)
