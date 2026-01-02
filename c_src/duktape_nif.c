/*
 * Duktape Erlang NIF - JavaScript engine for Erlang
 *
 * Copyright (c) 2025 Benoit Chesneau
 * Licensed under the Apache License, Version 2.0
 */

#include <string.h>
#include <stdint.h>
#include "erl_nif.h"
#include "duktape.h"

/* ============================================================================
 * Types and structures
 * ============================================================================ */

/* Duktape context wrapper with reference counting */
typedef struct {
    duk_context *ctx;           /* The Duktape heap/context */
    ErlNifMutex *lock;          /* Mutex for thread safety */
    int ref_count;              /* Reference count */
    int destroyed;              /* Flag indicating context was explicitly destroyed */
} duktape_ctx_t;

/* ============================================================================
 * Global state
 * ============================================================================ */

/* Resource type for duktape contexts */
static ErlNifResourceType *duktape_ctx_resource;

/* Atoms */
static ERL_NIF_TERM atom_ok;
static ERL_NIF_TERM atom_error;
static ERL_NIF_TERM atom_undefined;
static ERL_NIF_TERM atom_null;
static ERL_NIF_TERM atom_true;
static ERL_NIF_TERM atom_false;
static ERL_NIF_TERM atom_enomem;
static ERL_NIF_TERM atom_invalid_context;
static ERL_NIF_TERM atom_badarg;

/* ============================================================================
 * Resource management
 * ============================================================================ */

/* Destructor called when resource is garbage collected */
static void
duktape_ctx_destructor(ErlNifEnv *env, void *obj)
{
    (void)env;
    duktape_ctx_t *res = (duktape_ctx_t *)obj;

    if (res->lock) {
        enif_mutex_lock(res->lock);

        if (res->ctx && !res->destroyed) {
            duk_destroy_heap(res->ctx);
            res->ctx = NULL;
        }

        enif_mutex_unlock(res->lock);
        enif_mutex_destroy(res->lock);
        res->lock = NULL;
    }
}

/* Helper to get context from resource term */
static duktape_ctx_t *
get_context(ErlNifEnv *env, ERL_NIF_TERM term)
{
    duktape_ctx_t *res;

    if (!enif_get_resource(env, term, duktape_ctx_resource, (void **)&res)) {
        return NULL;
    }

    if (res->destroyed || res->ctx == NULL) {
        return NULL;
    }

    return res;
}

/* ============================================================================
 * NIF functions
 * ============================================================================ */

/* Create a new Duktape context */
static ERL_NIF_TERM
nif_new_context(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;
    (void)argv;

    /* Allocate the resource */
    duktape_ctx_t *res = enif_alloc_resource(duktape_ctx_resource, sizeof(duktape_ctx_t));
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Initialize the structure */
    res->ctx = NULL;
    res->lock = NULL;
    res->ref_count = 1;
    res->destroyed = 0;

    /* Create the mutex */
    res->lock = enif_mutex_create("duktape_ctx_lock");
    if (!res->lock) {
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Create the Duktape heap */
    res->ctx = duk_create_heap_default();
    if (!res->ctx) {
        enif_mutex_destroy(res->lock);
        res->lock = NULL;
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Create the resource term */
    ERL_NIF_TERM res_term = enif_make_resource(env, res);

    /* Release our reference (the term now holds a reference) */
    enif_release_resource(res);

    return enif_make_tuple2(env, atom_ok, res_term);
}

/* Destroy a Duktape context */
static ERL_NIF_TERM
nif_destroy_context(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;

    if (!enif_get_resource(env, argv[0], duktape_ctx_resource, (void **)&res)) {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed) {
        enif_mutex_unlock(res->lock);
        return atom_ok;  /* Already destroyed, idempotent */
    }

    if (res->ctx) {
        duk_destroy_heap(res->ctx);
        res->ctx = NULL;
    }
    res->destroyed = 1;

    enif_mutex_unlock(res->lock);

    return atom_ok;
}

/* Get NIF information */
static ERL_NIF_TERM
nif_info(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;
    (void)argv;

    return enif_make_tuple2(env, atom_ok,
        enif_make_string(env, "duktape nif loaded", ERL_NIF_LATIN1));
}

/* ============================================================================
 * NIF initialization
 * ============================================================================ */

static int
on_load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info)
{
    (void)priv_data;
    (void)load_info;

    /* Create the resource type for duktape contexts */
    duktape_ctx_resource = enif_open_resource_type(
        env,
        NULL,
        "duktape_context",
        duktape_ctx_destructor,
        ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER,
        NULL
    );

    if (!duktape_ctx_resource) {
        return -1;
    }

    /* Initialize atoms */
    atom_ok = enif_make_atom(env, "ok");
    atom_error = enif_make_atom(env, "error");
    atom_undefined = enif_make_atom(env, "undefined");
    atom_null = enif_make_atom(env, "null");
    atom_true = enif_make_atom(env, "true");
    atom_false = enif_make_atom(env, "false");
    atom_enomem = enif_make_atom(env, "enomem");
    atom_invalid_context = enif_make_atom(env, "invalid_context");
    atom_badarg = enif_make_atom(env, "badarg");

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

/* NIF function table */
static ErlNifFunc nif_funcs[] = {
    {"nif_info", 0, nif_info, 0},
    {"nif_new_context", 0, nif_new_context, 0},
    {"nif_destroy_context", 1, nif_destroy_context, 0}
};

ERL_NIF_INIT(duktape, nif_funcs, on_load, NULL, on_upgrade, on_unload)
