/*
 * Duktape Erlang NIF - JavaScript engine for Erlang
 *
 * Copyright (c) 2025 Benoit Chesneau
 * Licensed under the Apache License, Version 2.0
 */

#include <string.h>
#include <stdint.h>
#include <math.h>
#include "erl_nif.h"
#include "duktape.h"

/* ============================================================================
 * Safe memory operations
 * ============================================================================ */

/*
 * Safe memory copy with NULL and bounds checking.
 * Returns 0 on success, -1 on error (NULL pointers or invalid size).
 * Uses memmove internally which safely handles overlapping regions.
 */
static inline int
safe_memcpy(void *dest, size_t dest_size, const void *src, size_t count)
{
    if (dest == NULL || src == NULL) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (count > dest_size) {
        return -1;
    }
    memmove(dest, src, count);
    return 0;
}

/*
 * Helper to create an Erlang binary from a string with safe copying.
 * Returns the binary term on success, or a fallback term on failure.
 */
static ERL_NIF_TERM
make_binary_from_string(ErlNifEnv *env, const char *str, size_t len, ERL_NIF_TERM fallback)
{
    if (str == NULL) {
        return fallback;
    }

    ERL_NIF_TERM bin;
    unsigned char *buf = enif_make_new_binary(env, len, &bin);
    if (buf == NULL) {
        return fallback;
    }

    if (len > 0) {
        if (safe_memcpy(buf, len, str, len) != 0) {
            return fallback;
        }
    }

    return bin;
}

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
static ERL_NIF_TERM atom_js_error;

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
 * Type conversion: Duktape -> Erlang
 * ============================================================================ */

/* Convert a Duktape value at stack index to an Erlang term */
static ERL_NIF_TERM
duk_to_erlang(ErlNifEnv *env, duk_context *ctx, duk_idx_t idx)
{
    switch (duk_get_type(ctx, idx)) {
        case DUK_TYPE_UNDEFINED:
            return atom_undefined;

        case DUK_TYPE_NULL:
            return atom_null;

        case DUK_TYPE_BOOLEAN:
            return duk_get_boolean(ctx, idx) ? atom_true : atom_false;

        case DUK_TYPE_NUMBER: {
            double num = duk_get_number(ctx, idx);
            /* Check if it's an integer */
            if (floor(num) == num && num >= INT64_MIN && num <= INT64_MAX) {
                return enif_make_int64(env, (int64_t)num);
            }
            return enif_make_double(env, num);
        }

        case DUK_TYPE_STRING: {
            duk_size_t len;
            const char *str = duk_get_lstring(ctx, idx, &len);
            return make_binary_from_string(env, str, len, atom_enomem);
        }

        case DUK_TYPE_OBJECT: {
            /* For now, return a string representation */
            /* Full object conversion will be added in Step 6 */
            ERL_NIF_TERM result;
            duk_dup(ctx, idx);
            const char *str = duk_safe_to_string(ctx, -1);
            size_t len = str ? strlen(str) : 0;
            result = make_binary_from_string(env, str, len, atom_enomem);
            duk_pop(ctx);
            return result;
        }

        default: {
            /* Unknown type - return string representation */
            ERL_NIF_TERM result;
            duk_dup(ctx, idx);
            const char *str = duk_safe_to_string(ctx, -1);
            size_t len = str ? strlen(str) : 0;
            result = make_binary_from_string(env, str, len, atom_enomem);
            duk_pop(ctx);
            return result;
        }
    }
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

/* Evaluate JavaScript code */
static ERL_NIF_TERM
nif_eval(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;
    ErlNifBinary js_code;
    ERL_NIF_TERM result;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get the JavaScript code as binary */
    if (!enif_inspect_binary(env, argv[1], &js_code)) {
        /* Try iolist */
        if (!enif_inspect_iolist_as_binary(env, argv[1], &js_code)) {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Push the code as a string and evaluate */
    duk_push_lstring(res->ctx, (const char *)js_code.data, js_code.size);

    if (duk_peval(res->ctx) != 0) {
        /* Error occurred */
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop(res->ctx);  /* Pop error */
        enif_mutex_unlock(res->lock);

        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    /* Convert result to Erlang term */
    result = duk_to_erlang(env, res->ctx, -1);
    duk_pop(res->ctx);  /* Pop result */

    enif_mutex_unlock(res->lock);

    return enif_make_tuple2(env, atom_ok, result);
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
    atom_js_error = enif_make_atom(env, "js_error");

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
    {"nif_destroy_context", 1, nif_destroy_context, 0},
    {"nif_eval", 2, nif_eval, 0}
};

ERL_NIF_INIT(duktape, nif_funcs, on_load, NULL, on_upgrade, on_unload)
