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
 * Type conversion: Erlang -> Duktape
 * ============================================================================ */

/* Forward declaration for recursive conversion */
static int erlang_to_duk(ErlNifEnv *env, duk_context *ctx, ERL_NIF_TERM term);

/*
 * Push an Erlang term onto the Duktape stack.
 * Returns 0 on success, -1 on error.
 */
static int
erlang_to_duk(ErlNifEnv *env, duk_context *ctx, ERL_NIF_TERM term)
{
    /* Check for atoms first */
    if (enif_is_atom(env, term)) {
        char atom_buf[256];
        if (enif_get_atom(env, term, atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1) <= 0) {
            return -1;
        }

        if (strcmp(atom_buf, "true") == 0) {
            duk_push_boolean(ctx, 1);
        } else if (strcmp(atom_buf, "false") == 0) {
            duk_push_boolean(ctx, 0);
        } else if (strcmp(atom_buf, "null") == 0) {
            duk_push_null(ctx);
        } else if (strcmp(atom_buf, "undefined") == 0) {
            duk_push_undefined(ctx);
        } else {
            /* Other atoms become strings */
            duk_push_string(ctx, atom_buf);
        }
        return 0;
    }

    /* Check for integers */
    ErlNifSInt64 i64;
    if (enif_get_int64(env, term, &i64)) {
        duk_push_number(ctx, (double)i64);
        return 0;
    }

    /* Check for doubles */
    double d;
    if (enif_get_double(env, term, &d)) {
        duk_push_number(ctx, d);
        return 0;
    }

    /* Check for binaries (become strings) */
    ErlNifBinary bin;
    if (enif_inspect_binary(env, term, &bin)) {
        duk_push_lstring(ctx, (const char *)bin.data, bin.size);
        return 0;
    }

    /* Check for lists */
    if (enif_is_list(env, term)) {
        /* Could be a string (iolist) or an array */
        /* Try as iolist first */
        if (enif_inspect_iolist_as_binary(env, term, &bin)) {
            duk_push_lstring(ctx, (const char *)bin.data, bin.size);
            return 0;
        }

        /* Otherwise treat as array */
        unsigned int len;
        if (!enif_get_list_length(env, term, &len)) {
            return -1;
        }

        duk_idx_t arr_idx = duk_push_array(ctx);
        ERL_NIF_TERM head, tail;
        unsigned int i = 0;
        tail = term;

        while (enif_get_list_cell(env, tail, &head, &tail)) {
            if (erlang_to_duk(env, ctx, head) != 0) {
                duk_pop(ctx);  /* Pop the array */
                return -1;
            }
            duk_put_prop_index(ctx, arr_idx, i++);
        }
        return 0;
    }

    /* Check for maps (become objects) */
    if (enif_is_map(env, term)) {
        duk_push_object(ctx);

        ErlNifMapIterator iter;
        if (!enif_map_iterator_create(env, term, &iter, ERL_NIF_MAP_ITERATOR_FIRST)) {
            duk_pop(ctx);
            return -1;
        }

        ERL_NIF_TERM key, value;
        while (enif_map_iterator_get_pair(env, &iter, &key, &value)) {
            /* Get key as string */
            char key_buf[256];
            ErlNifBinary key_bin;

            if (enif_get_atom(env, key, key_buf, sizeof(key_buf), ERL_NIF_LATIN1) > 0) {
                /* Atom key */
                duk_push_string(ctx, key_buf);
            } else if (enif_inspect_binary(env, key, &key_bin)) {
                /* Binary key */
                duk_push_lstring(ctx, (const char *)key_bin.data, key_bin.size);
            } else if (enif_inspect_iolist_as_binary(env, key, &key_bin)) {
                /* Iolist key */
                duk_push_lstring(ctx, (const char *)key_bin.data, key_bin.size);
            } else {
                /* Unsupported key type */
                enif_map_iterator_destroy(env, &iter);
                duk_pop(ctx);
                return -1;
            }

            /* Convert value */
            if (erlang_to_duk(env, ctx, value) != 0) {
                duk_pop_2(ctx);  /* Pop key and object */
                enif_map_iterator_destroy(env, &iter);
                return -1;
            }

            /* Set property */
            duk_put_prop(ctx, -3);

            enif_map_iterator_next(env, &iter);
        }

        enif_map_iterator_destroy(env, &iter);
        return 0;
    }

    /* Check for tuples - convert to array */
    int arity;
    const ERL_NIF_TERM *tuple_elements;
    if (enif_get_tuple(env, term, &arity, &tuple_elements)) {
        duk_idx_t arr_idx = duk_push_array(ctx);
        for (int i = 0; i < arity; i++) {
            if (erlang_to_duk(env, ctx, tuple_elements[i]) != 0) {
                duk_pop(ctx);
                return -1;
            }
            duk_put_prop_index(ctx, arr_idx, (duk_uarridx_t)i);
        }
        return 0;
    }

    /* Unsupported type */
    return -1;
}

/* ============================================================================
 * Type conversion: Duktape -> Erlang
 * ============================================================================ */

/* Maximum nesting depth to prevent stack overflow on circular references */
#define MAX_CONVERSION_DEPTH 100

/* Forward declaration for recursive conversion */
static ERL_NIF_TERM duk_to_erlang_depth(ErlNifEnv *env, duk_context *ctx, duk_idx_t idx, int depth);

/*
 * Convert a Duktape array at stack index to an Erlang list.
 * Returns the list term, or atom_enomem on failure.
 */
static ERL_NIF_TERM
duk_array_to_erlang_list(ErlNifEnv *env, duk_context *ctx, duk_idx_t idx, int depth)
{
    duk_size_t len = duk_get_length(ctx, idx);

    if (len == 0) {
        return enif_make_list(env, 0);
    }

    /* Allocate array for list elements */
    ERL_NIF_TERM *elements = enif_alloc(sizeof(ERL_NIF_TERM) * len);
    if (elements == NULL) {
        return atom_enomem;
    }

    for (duk_size_t i = 0; i < len; i++) {
        duk_get_prop_index(ctx, idx, (duk_uarridx_t)i);
        elements[i] = duk_to_erlang_depth(env, ctx, -1, depth + 1);
        duk_pop(ctx);
    }

    ERL_NIF_TERM result = enif_make_list_from_array(env, elements, (unsigned int)len);
    enif_free(elements);

    return result;
}

/*
 * Convert a Duktape object at stack index to an Erlang map.
 * Returns the map term, or atom_enomem on failure.
 */
static ERL_NIF_TERM
duk_object_to_erlang_map(ErlNifEnv *env, duk_context *ctx, duk_idx_t idx, int depth)
{
    ERL_NIF_TERM map = enif_make_new_map(env);

    /* Enumerate own properties */
    duk_enum(ctx, idx, DUK_ENUM_OWN_PROPERTIES_ONLY);

    while (duk_next(ctx, -1, 1)) {  /* key at -2, value at -1 */
        /* Get key as string */
        duk_size_t key_len;
        const char *key_str = duk_get_lstring(ctx, -2, &key_len);
        ERL_NIF_TERM key = make_binary_from_string(env, key_str, key_len, atom_enomem);

        /* Convert value */
        ERL_NIF_TERM value = duk_to_erlang_depth(env, ctx, -1, depth + 1);

        /* Add to map */
        enif_make_map_put(env, map, key, value, &map);

        duk_pop_2(ctx);  /* Pop key and value */
    }

    duk_pop(ctx);  /* Pop enum object */

    return map;
}

/*
 * Convert a Duktape value at stack index to an Erlang term.
 * Uses depth tracking to prevent stack overflow on circular references.
 */
static ERL_NIF_TERM
duk_to_erlang_depth(ErlNifEnv *env, duk_context *ctx, duk_idx_t idx, int depth)
{
    /* Check depth limit */
    if (depth > MAX_CONVERSION_DEPTH) {
        return make_binary_from_string(env, "[max depth exceeded]", 20, atom_enomem);
    }

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
            /* Check if it's an array */
            if (duk_is_array(ctx, idx)) {
                return duk_array_to_erlang_list(env, ctx, idx, depth);
            }

            /* Check if it's a function - return string representation */
            if (duk_is_function(ctx, idx)) {
                ERL_NIF_TERM result;
                duk_dup(ctx, idx);
                const char *str = duk_safe_to_string(ctx, -1);
                size_t len = str ? strlen(str) : 0;
                result = make_binary_from_string(env, str, len, atom_enomem);
                duk_pop(ctx);
                return result;
            }

            /* Regular object - convert to map */
            return duk_object_to_erlang_map(env, ctx, idx, depth);
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

/* Public interface - starts with depth 0 */
static ERL_NIF_TERM
duk_to_erlang(ErlNifEnv *env, duk_context *ctx, duk_idx_t idx)
{
    return duk_to_erlang_depth(env, ctx, idx, 0);
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

/* Evaluate JavaScript code with bindings */
static ERL_NIF_TERM
nif_eval_bindings(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
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
        if (!enif_inspect_iolist_as_binary(env, argv[1], &js_code)) {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
    }

    /* Bindings must be a map */
    if (!enif_is_map(env, argv[2])) {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Set bindings as global variables */
    ErlNifMapIterator iter;
    if (!enif_map_iterator_create(env, argv[2], &iter, ERL_NIF_MAP_ITERATOR_FIRST)) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    ERL_NIF_TERM key, value;
    while (enif_map_iterator_get_pair(env, &iter, &key, &value)) {
        /* Get key as string for global variable name */
        char key_buf[256];
        ErlNifBinary key_bin;
        const char *var_name = NULL;
        size_t var_name_len = 0;

        if (enif_get_atom(env, key, key_buf, sizeof(key_buf), ERL_NIF_LATIN1) > 0) {
            var_name = key_buf;
            var_name_len = strlen(key_buf);
        } else if (enif_inspect_binary(env, key, &key_bin)) {
            var_name = (const char *)key_bin.data;
            var_name_len = key_bin.size;
        } else if (enif_inspect_iolist_as_binary(env, key, &key_bin)) {
            var_name = (const char *)key_bin.data;
            var_name_len = key_bin.size;
        } else {
            enif_map_iterator_destroy(env, &iter);
            enif_mutex_unlock(res->lock);
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }

        /* Convert value to Duktape and set as global */
        if (erlang_to_duk(env, res->ctx, value) != 0) {
            enif_map_iterator_destroy(env, &iter);
            enif_mutex_unlock(res->lock);
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }

        /* Set as global variable */
        duk_put_global_lstring(res->ctx, var_name, var_name_len);

        enif_map_iterator_next(env, &iter);
    }
    enif_map_iterator_destroy(env, &iter);

    /* Evaluate the code */
    duk_push_lstring(res->ctx, (const char *)js_code.data, js_code.size);

    if (duk_peval(res->ctx) != 0) {
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);

        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    /* Convert result to Erlang term */
    result = duk_to_erlang(env, res->ctx, -1);
    duk_pop(res->ctx);

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
    {"nif_eval", 2, nif_eval, 0},
    {"nif_eval_bindings", 3, nif_eval_bindings, 0}
};

ERL_NIF_INIT(duktape, nif_funcs, on_load, NULL, on_upgrade, on_unload)
