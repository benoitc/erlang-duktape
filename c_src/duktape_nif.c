/*
 * Duktape Erlang NIF - JavaScript engine for Erlang
 *
 * Copyright (c) 2025 Benoit Chesneau
 * Licensed under the Apache License, Version 2.0
 */

#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "erl_nif.h"
#include "duktape.h"
#include "duk_module_duktape.h"

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

/* Memory metrics for tracking heap usage */
typedef struct {
    size_t heap_bytes;          /* Current allocated bytes */
    size_t heap_peak;           /* Peak memory usage */
    size_t alloc_count;         /* Total allocation count */
    size_t realloc_count;       /* Total reallocation count */
    size_t free_count;          /* Total free count */
    size_t gc_runs;             /* Number of GC runs triggered */
} duktape_metrics_t;

/* Duktape context wrapper with reference counting */
typedef struct {
    duk_context *ctx;           /* The Duktape heap/context */
    ErlNifMutex *lock;          /* Mutex for thread safety */
    int ref_count;              /* Reference count */
    int destroyed;              /* Flag indicating context was explicitly destroyed */
    /* Memory metrics */
    duktape_metrics_t metrics;  /* Memory tracking stats */
    /* Event handling */
    ErlNifEnv *event_env;       /* Env for sending events to Erlang */
    ErlNifPid handler_pid;      /* Handler process for events */
    int handler_enabled;        /* Whether handler is set */
    /* Pending Erlang function call (for trampoline pattern) */
    int pending_call;           /* Flag: Erlang call pending */
    char pending_func[256];     /* Function name being called */
    ERL_NIF_TERM pending_args;  /* Arguments list for pending call */
    ErlNifEnv *pending_env;     /* Env for pending call args */
    ERL_NIF_TERM call_result;   /* Result from completed Erlang call */
    int has_call_result;        /* Flag: result is available */
    /* Original code for resumption */
    char *resume_code;          /* Original JS code being evaluated */
    size_t resume_code_len;     /* Length of resume_code */
    /* Call indexing for nested calls */
    int call_index;             /* Current call index in eval sequence */
    int pending_index;          /* Index of the pending call */
    /* Timeout support */
    int timeout_enabled;        /* Whether timeout checking is active */
    uint64_t timeout_ms;        /* Timeout in milliseconds */
    uint64_t exec_start_ns;     /* Start time in nanoseconds (monotonic) */
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
/* Event atoms */
static ERL_NIF_TERM atom_duktape;
static ERL_NIF_TERM atom_log;
static ERL_NIF_TERM atom_debug;
static ERL_NIF_TERM atom_info;
static ERL_NIF_TERM atom_warning;
static ERL_NIF_TERM atom_level;
static ERL_NIF_TERM atom_message;
static ERL_NIF_TERM atom_handler;
/* Erlang function call atoms */
static ERL_NIF_TERM atom_call_erlang;

/* Metrics atoms */
static ERL_NIF_TERM atom_heap_bytes;
static ERL_NIF_TERM atom_heap_peak;
static ERL_NIF_TERM atom_alloc_count;
static ERL_NIF_TERM atom_realloc_count;
static ERL_NIF_TERM atom_free_count;
static ERL_NIF_TERM atom_gc_runs;

/* Timeout atom */
static ERL_NIF_TERM atom_timeout;

/* ============================================================================
 * Timeout support
 * ============================================================================ */

/* Get monotonic time in nanoseconds */
static uint64_t get_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Timeout check callback - called by Duktape's interrupt handler.
 * Returns non-zero if execution should be aborted.
 * This is called from duktape.c via DUK_USE_EXEC_TIMEOUT_CHECK macro.
 */
int duktape_check_timeout(void *udata) {
    duktape_ctx_t *res = (duktape_ctx_t *)udata;
    if (res == NULL || !res->timeout_enabled) {
        return 0;
    }

    uint64_t elapsed_ms = (get_monotonic_ns() - res->exec_start_ns) / 1000000ULL;
    return elapsed_ms > res->timeout_ms ? 1 : 0;
}

/* Start timeout timer for an execution */
static void start_timeout(duktape_ctx_t *res, uint64_t timeout_ms) {
    if (timeout_ms > 0 && timeout_ms != UINT64_MAX) {
        res->timeout_enabled = 1;
        res->timeout_ms = timeout_ms;
        res->exec_start_ns = get_monotonic_ns();
    } else {
        res->timeout_enabled = 0;
    }
}

/* Stop timeout timer */
static void stop_timeout(duktape_ctx_t *res) {
    res->timeout_enabled = 0;
}

/* ============================================================================
 * Custom memory allocator for metrics tracking
 * ============================================================================ */

/*
 * Memory block header - prepended to each allocation to track size.
 * This allows us to track memory usage accurately on free().
 */
typedef struct {
    size_t size;
} mem_header_t;

#define MEM_HEADER_SIZE sizeof(mem_header_t)

/*
 * Custom allocator function for Duktape.
 * Allocates memory with a header to track size.
 */
static void *
metrics_alloc(void *udata, duk_size_t size)
{
    duktape_ctx_t *res = (duktape_ctx_t *)udata;

    if (size == 0) {
        return NULL;
    }

    /* Check for integer overflow before adding header size */
    if (size > SIZE_MAX - MEM_HEADER_SIZE) {
        return NULL;
    }

    /* Allocate with header */
    mem_header_t *header = (mem_header_t *)enif_alloc(MEM_HEADER_SIZE + size);
    if (!header) {
        return NULL;
    }

    header->size = size;

    /* Update metrics */
    res->metrics.heap_bytes += size;
    res->metrics.alloc_count++;
    if (res->metrics.heap_bytes > res->metrics.heap_peak) {
        res->metrics.heap_peak = res->metrics.heap_bytes;
    }

    return (void *)(header + 1);
}

/*
 * Custom realloc function for Duktape.
 */
static void *
metrics_realloc(void *udata, void *ptr, duk_size_t size)
{
    duktape_ctx_t *res = (duktape_ctx_t *)udata;

    /* realloc(NULL, size) is like malloc(size) */
    if (ptr == NULL) {
        return metrics_alloc(udata, size);
    }

    /* realloc(ptr, 0) is like free(ptr) */
    if (size == 0) {
        mem_header_t *header = ((mem_header_t *)ptr) - 1;
        res->metrics.heap_bytes -= header->size;
        res->metrics.free_count++;
        enif_free(header);
        return NULL;
    }

    mem_header_t *old_header = ((mem_header_t *)ptr) - 1;
    size_t old_size = old_header->size;

    /* Check for integer overflow before adding header size */
    if (size > SIZE_MAX - MEM_HEADER_SIZE) {
        return NULL;
    }

    /* Reallocate with header */
    mem_header_t *new_header = (mem_header_t *)enif_realloc(old_header, MEM_HEADER_SIZE + size);
    if (!new_header) {
        return NULL;
    }

    new_header->size = size;

    /* Update metrics */
    res->metrics.heap_bytes = res->metrics.heap_bytes - old_size + size;
    res->metrics.realloc_count++;
    if (res->metrics.heap_bytes > res->metrics.heap_peak) {
        res->metrics.heap_peak = res->metrics.heap_bytes;
    }

    return (void *)(new_header + 1);
}

/*
 * Custom free function for Duktape.
 */
static void
metrics_free(void *udata, void *ptr)
{
    duktape_ctx_t *res = (duktape_ctx_t *)udata;

    if (ptr == NULL) {
        return;
    }

    mem_header_t *header = ((mem_header_t *)ptr) - 1;

    /* Update metrics */
    res->metrics.heap_bytes -= header->size;
    res->metrics.free_count++;

    enif_free(header);
}

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

        /* Clean up event environment */
        if (res->event_env) {
            enif_free_env(res->event_env);
            res->event_env = NULL;
        }

        /* Clean up pending call environment */
        if (res->pending_env) {
            enif_free_env(res->pending_env);
            res->pending_env = NULL;
        }

        /* Clean up resume code */
        if (res->resume_code) {
            enif_free(res->resume_code);
            res->resume_code = NULL;
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
            /* Handle special values that Erlang can't represent as floats */
            if (isnan(num)) {
                return enif_make_atom(env, "nan");
            }
            if (isinf(num)) {
                if (num > 0) {
                    return enif_make_atom(env, "infinity");
                } else {
                    return enif_make_atom(env, "neg_infinity");
                }
            }
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
 * CommonJS module support
 * ============================================================================ */

/* Key used to store registered modules in the global stash */
#define MODULE_STASH_KEY "\xff" "erlang_modules"

/*
 * Duktape.modSearch callback - looks up modules from the stash.
 * Called as: modSearch(resolved_id, require, exports, module)
 * Should return module source code as string, or undefined if module
 * was populated directly into exports.
 */
static duk_ret_t
mod_search(duk_context *ctx)
{
    /* Get resolved module ID */
    const char *mod_id = duk_require_string(ctx, 0);

    /* Look up in global stash */
    duk_push_global_stash(ctx);
    if (!duk_get_prop_string(ctx, -1, MODULE_STASH_KEY)) {
        /* No modules registered */
        duk_pop_2(ctx);
        return duk_type_error(ctx, "module not found: %s", mod_id);
    }

    /* Get module source from stash */
    if (!duk_get_prop_string(ctx, -1, mod_id)) {
        /* Module not found */
        duk_pop_3(ctx);
        return duk_type_error(ctx, "module not found: %s", mod_id);
    }

    /* Return the source code (string on top of stack) */
    return 1;
}

/* ============================================================================
 * Event handling - JS to Erlang communication
 * ============================================================================ */

/*
 * Send event to Erlang handler.
 * Creates: {duktape, Type, Data}
 */
static void
emit_event(duktape_ctx_t *res, ERL_NIF_TERM type, ERL_NIF_TERM data)
{
    if (!res->handler_enabled || !res->event_env) return;

    ERL_NIF_TERM tuple = enif_make_tuple3(res->event_env,
        atom_duktape, type, data);
    enif_send(NULL, &res->handler_pid, res->event_env, tuple);
    enif_clear_env(res->event_env);
}

/*
 * Erlang.emit(type, data) - send event to Erlang
 * type: string (event type)
 * data: any JS value (converted to Erlang term)
 */
static duk_ret_t
erlang_emit(duk_context *ctx)
{
    /* Get context from stash */
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "duktape_ctx");
    duktape_ctx_t *res = (duktape_ctx_t *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);

    if (!res || !res->handler_enabled || !res->event_env) {
        return 0;  /* No handler, silently ignore */
    }

    /* Get event type (string → binary to prevent atom table exhaustion) */
    duk_size_t type_len;
    const char *type_str = duk_get_lstring(ctx, 0, &type_len);
    if (!type_str) {
        return 0;
    }
    ERL_NIF_TERM type_bin = make_binary_from_string(res->event_env, type_str, type_len, atom_enomem);

    /* Convert data to Erlang term */
    ERL_NIF_TERM data = duk_to_erlang(res->event_env, ctx, 1);

    emit_event(res, type_bin, data);

    return 0;
}

/*
 * Erlang.log(level, ...args) - convenience for logging
 * Emits: {duktape, log, #{level => Level, message => Message}}
 */
static duk_ret_t
erlang_log(duk_context *ctx)
{
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "duktape_ctx");
    duktape_ctx_t *res = (duktape_ctx_t *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);

    if (!res || !res->handler_enabled || !res->event_env) {
        return 0;
    }

    /* Get level - use atoms for known levels, binary for unknown to prevent atom exhaustion */
    const char *level_str = duk_require_string(ctx, 0);
    ERL_NIF_TERM level_term;
    if (strcmp(level_str, "debug") == 0) level_term = atom_debug;
    else if (strcmp(level_str, "info") == 0) level_term = atom_info;
    else if (strcmp(level_str, "warning") == 0) level_term = atom_warning;
    else if (strcmp(level_str, "error") == 0) level_term = atom_error;
    else {
        /* Unknown level - use binary to prevent atom table exhaustion */
        level_term = make_binary_from_string(res->event_env, level_str, strlen(level_str), atom_enomem);
    }

    /* Build message from remaining args */
    int nargs = duk_get_top(ctx);
    if (nargs > 1) {
        duk_push_string(ctx, "");
        for (int i = 1; i < nargs; i++) {
            if (i > 1) duk_push_string(ctx, " ");
            duk_dup(ctx, i);
            duk_safe_to_string(ctx, -1);
        }
        duk_concat(ctx, (nargs - 1) * 2 - 1);
    } else {
        duk_push_string(ctx, "");
    }

    duk_size_t msg_len;
    const char *msg = duk_get_lstring(ctx, -1, &msg_len);

    /* Create message binary */
    ERL_NIF_TERM msg_bin = make_binary_from_string(res->event_env, msg, msg_len, atom_enomem);

    /* Create data map: #{level => atom|binary, message => binary} */
    ERL_NIF_TERM keys[2];
    ERL_NIF_TERM vals[2];
    keys[0] = atom_level;
    keys[1] = atom_message;
    vals[0] = level_term;
    vals[1] = msg_bin;
    ERL_NIF_TERM data;
    enif_make_map_from_arrays(res->event_env, keys, vals, 2, &data);

    emit_event(res, atom_log, data);
    duk_pop(ctx);

    return 0;
}

/*
 * Erlang.on(event, callback) - register callback for event
 */
static duk_ret_t
erlang_on(duk_context *ctx)
{
    const char *event = duk_require_string(ctx, 0);
    if (!duk_is_function(ctx, 1)) {
        return duk_type_error(ctx, "callback must be a function");
    }

    /* Store callback: Erlang._callbacks[event] = fn */
    duk_get_global_string(ctx, "Erlang");
    duk_get_prop_string(ctx, -1, "_callbacks");
    duk_dup(ctx, 1);  /* callback function */
    duk_put_prop_string(ctx, -2, event);
    duk_pop_2(ctx);

    return 0;
}

/*
 * Erlang.off(event) - unregister callback for event
 */
static duk_ret_t
erlang_off(duk_context *ctx)
{
    const char *event = duk_require_string(ctx, 0);

    duk_get_global_string(ctx, "Erlang");
    duk_get_prop_string(ctx, -1, "_callbacks");
    duk_del_prop_string(ctx, -1, event);
    duk_pop_2(ctx);

    return 0;
}

/* ============================================================================
 * Erlang function trampoline - for JS calling Erlang functions
 * ============================================================================ */

/*
 * Called when JavaScript invokes a registered Erlang function.
 * This C function:
 * 1. First checks if there's a cached result available (for resumed execution)
 * 2. If not, stores the function name and arguments in the context
 * 3. Throws an error to unwind the JS stack
 * 4. nif_eval will catch this and return {call_erlang, Name, Args} to Erlang
 */
static duk_ret_t
erlang_function_trampoline(duk_context *ctx)
{
    /* Get function name from current function's _erlang_name property */
    duk_push_current_function(ctx);
    duk_get_prop_string(ctx, -1, "_erlang_name");
    const char *func_name = duk_require_string(ctx, -1);
    duk_pop_2(ctx);

    /* Get context from stash */
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "duktape_ctx");
    duktape_ctx_t *res = (duktape_ctx_t *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);

    if (!res) {
        return duk_type_error(ctx, "internal error: context not found");
    }

    /* Check if there's a cached result for this specific call index */
    /* Results are stored in __erlang_results__ object keyed by call index */
    int current_index = res->call_index;
    res->call_index++;  /* Increment for next call */

    duk_get_global_string(ctx, "__erlang_results__");
    if (duk_is_object(ctx, -1)) {
        duk_get_prop_index(ctx, -1, (duk_uarridx_t)current_index);
        if (!duk_is_undefined(ctx, -1)) {
            /* Found cached result for this call index */
            /* Check if it's an error marker */
            if (duk_is_string(ctx, -1)) {
                const char *str = duk_get_string(ctx, -1);
                if (str && strncmp(str, "__ERROR__:", 10) == 0) {
                    /* It's an error - throw it */
                    const char *err_msg = str + 10;
                    duk_pop_2(ctx);  /* Pop string and object */
                    return duk_error(ctx, DUK_ERR_ERROR, "error: %s", err_msg);
                }
            }
            duk_remove(ctx, -2);  /* Remove the object, keep the result */
            return 1;  /* Return the cached result */
        }
        duk_pop(ctx);  /* Pop undefined */
    }
    duk_pop(ctx);  /* Pop the object */

    /* No cached result - set up pending call and throw */
    res->pending_call = 1;
    res->pending_index = current_index;
    strncpy(res->pending_func, func_name, sizeof(res->pending_func) - 1);
    res->pending_func[sizeof(res->pending_func) - 1] = '\0';

    /* Clear the pending env and convert all arguments to an Erlang list */
    enif_clear_env(res->pending_env);

    int nargs = duk_get_top(ctx);
    ERL_NIF_TERM *arg_terms = NULL;

    if (nargs > 0) {
        arg_terms = enif_alloc(sizeof(ERL_NIF_TERM) * nargs);
        if (!arg_terms) {
            res->pending_call = 0;
            return duk_error(ctx, DUK_ERR_ERROR, "out of memory");
        }

        for (int i = 0; i < nargs; i++) {
            arg_terms[i] = duk_to_erlang(res->pending_env, ctx, i);
        }

        res->pending_args = enif_make_list_from_array(res->pending_env, arg_terms, (unsigned int)nargs);
        enif_free(arg_terms);
    } else {
        res->pending_args = enif_make_list(res->pending_env, 0);
    }

    /* Throw special error to unwind - will be caught by nif_eval */
    return duk_error(ctx, DUK_ERR_ERROR, "__ERLANG_CALL__");
}

/*
 * Initialize Erlang global object and console wrapper
 */
static void
init_erlang_object(duk_context *ctx)
{
    /* Create Erlang global object */
    duk_push_object(ctx);

    /* Erlang.emit(type, data) */
    duk_push_c_function(ctx, erlang_emit, 2);
    duk_put_prop_string(ctx, -2, "emit");

    /* Erlang.log(level, ...args) */
    duk_push_c_function(ctx, erlang_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "log");

    /* Erlang.on(event, callback) */
    duk_push_c_function(ctx, erlang_on, 2);
    duk_put_prop_string(ctx, -2, "on");

    /* Erlang.off(event) */
    duk_push_c_function(ctx, erlang_off, 1);
    duk_put_prop_string(ctx, -2, "off");

    /* Erlang._callbacks = {} for storing registered callbacks */
    duk_push_object(ctx);
    duk_put_prop_string(ctx, -2, "_callbacks");

    duk_put_global_string(ctx, "Erlang");

    /* Create console object using JavaScript */
    duk_eval_string_noresult(ctx,
        "var console = {"
        "  log:   function() { Erlang.log.apply(null, ['info'].concat(Array.prototype.slice.call(arguments))); },"
        "  info:  function() { Erlang.log.apply(null, ['info'].concat(Array.prototype.slice.call(arguments))); },"
        "  warn:  function() { Erlang.log.apply(null, ['warning'].concat(Array.prototype.slice.call(arguments))); },"
        "  error: function() { Erlang.log.apply(null, ['error'].concat(Array.prototype.slice.call(arguments))); },"
        "  debug: function() { Erlang.log.apply(null, ['debug'].concat(Array.prototype.slice.call(arguments))); }"
        "};"
    );

    /* Initialize the results object for Erlang function call trampoline */
    /* Uses an object keyed by call index for proper nested call support */
    duk_eval_string_noresult(ctx, "var __erlang_results__ = {};");
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
    /* Initialize metrics */
    memset(&res->metrics, 0, sizeof(duktape_metrics_t));
    res->event_env = NULL;
    res->handler_enabled = 0;
    /* Initialize pending call fields */
    res->pending_call = 0;
    res->pending_func[0] = '\0';
    res->pending_env = NULL;
    res->has_call_result = 0;
    res->resume_code = NULL;
    res->resume_code_len = 0;
    res->call_index = 0;
    res->pending_index = 0;

    /* Initialize timeout fields */
    res->timeout_enabled = 0;
    res->timeout_ms = 0;
    res->exec_start_ns = 0;

    /* Create the mutex */
    res->lock = enif_mutex_create("duktape_ctx_lock");
    if (!res->lock) {
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Create the Duktape heap with custom allocator for metrics tracking */
    res->ctx = duk_create_heap(metrics_alloc, metrics_realloc, metrics_free,
                               (void *)res, NULL);
    if (!res->ctx) {
        enif_mutex_destroy(res->lock);
        res->lock = NULL;
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Allocate environment for pending call args */
    res->pending_env = enif_alloc_env();
    if (!res->pending_env) {
        duk_destroy_heap(res->ctx);
        enif_mutex_destroy(res->lock);
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Initialize CommonJS module system */
    duk_module_duktape_init(res->ctx);

    /* Set up the modSearch callback */
    duk_get_global_string(res->ctx, "Duktape");
    duk_push_c_function(res->ctx, mod_search, 4 /*nargs*/);
    duk_put_prop_string(res->ctx, -2, "modSearch");
    duk_pop(res->ctx);

    /* Initialize the module stash */
    duk_push_global_stash(res->ctx);
    duk_push_object(res->ctx);
    duk_put_prop_string(res->ctx, -2, MODULE_STASH_KEY);
    duk_pop(res->ctx);

    /* Store context pointer in stash for JS callbacks */
    duk_push_global_stash(res->ctx);
    duk_push_pointer(res->ctx, (void *)res);
    duk_put_prop_string(res->ctx, -2, "duktape_ctx");
    duk_pop(res->ctx);

    /* Initialize Erlang global object and console wrapper */
    init_erlang_object(res->ctx);

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

/* Create a new Duktape context with options */
static ERL_NIF_TERM
nif_new_context_opts(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

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
    /* Initialize metrics */
    memset(&res->metrics, 0, sizeof(duktape_metrics_t));
    res->event_env = NULL;
    res->handler_enabled = 0;
    /* Initialize pending call fields */
    res->pending_call = 0;
    res->pending_func[0] = '\0';
    res->pending_env = NULL;
    res->has_call_result = 0;
    res->resume_code = NULL;
    res->resume_code_len = 0;
    res->call_index = 0;
    res->pending_index = 0;

    /* Initialize timeout fields */
    res->timeout_enabled = 0;
    res->timeout_ms = 0;
    res->exec_start_ns = 0;

    /* Parse options map */
    if (!enif_is_map(env, argv[0])) {
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    /* Check for handler option */
    ERL_NIF_TERM handler_term;
    if (enif_get_map_value(env, argv[0], atom_handler, &handler_term)) {
        ErlNifPid handler_pid;
        if (enif_get_local_pid(env, handler_term, &handler_pid)) {
            res->handler_enabled = 1;
            res->handler_pid = handler_pid;
            res->event_env = enif_alloc_env();
        }
    }

    /* Create the mutex */
    res->lock = enif_mutex_create("duktape_ctx_lock");
    if (!res->lock) {
        if (res->event_env) enif_free_env(res->event_env);
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Create the Duktape heap with custom allocator for metrics tracking */
    res->ctx = duk_create_heap(metrics_alloc, metrics_realloc, metrics_free,
                               (void *)res, NULL);
    if (!res->ctx) {
        enif_mutex_destroy(res->lock);
        res->lock = NULL;
        if (res->event_env) enif_free_env(res->event_env);
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Allocate environment for pending call args */
    res->pending_env = enif_alloc_env();
    if (!res->pending_env) {
        duk_destroy_heap(res->ctx);
        enif_mutex_destroy(res->lock);
        if (res->event_env) enif_free_env(res->event_env);
        enif_release_resource(res);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Initialize CommonJS module system */
    duk_module_duktape_init(res->ctx);

    /* Set up the modSearch callback */
    duk_get_global_string(res->ctx, "Duktape");
    duk_push_c_function(res->ctx, mod_search, 4 /*nargs*/);
    duk_put_prop_string(res->ctx, -2, "modSearch");
    duk_pop(res->ctx);

    /* Initialize the module stash */
    duk_push_global_stash(res->ctx);
    duk_push_object(res->ctx);
    duk_put_prop_string(res->ctx, -2, MODULE_STASH_KEY);
    duk_pop(res->ctx);

    /* Store context pointer in stash for JS callbacks */
    duk_push_global_stash(res->ctx);
    duk_push_pointer(res->ctx, (void *)res);
    duk_put_prop_string(res->ctx, -2, "duktape_ctx");
    duk_pop(res->ctx);

    /* Initialize Erlang global object and console wrapper */
    init_erlang_object(res->ctx);

    /* Create the resource term */
    ERL_NIF_TERM res_term = enif_make_resource(env, res);

    /* Release our reference (the term now holds a reference) */
    enif_release_resource(res);

    return enif_make_tuple2(env, atom_ok, res_term);
}

/* Send data to a registered JavaScript callback */
static ERL_NIF_TERM
nif_send(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;
    ERL_NIF_TERM result;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get the event name */
    char event_buf[256];
    ErlNifBinary event_bin;
    const char *event_name = NULL;
    size_t event_len = 0;

    if (enif_get_atom(env, argv[1], event_buf, sizeof(event_buf), ERL_NIF_LATIN1) > 0) {
        event_name = event_buf;
        event_len = strlen(event_buf);
    } else if (enif_inspect_binary(env, argv[1], &event_bin)) {
        event_name = (const char *)event_bin.data;
        event_len = event_bin.size;
    } else if (enif_inspect_iolist_as_binary(env, argv[1], &event_bin)) {
        event_name = (const char *)event_bin.data;
        event_len = event_bin.size;
    } else {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get callback from Erlang._callbacks[event] */
    duk_get_global_string(res->ctx, "Erlang");
    duk_get_prop_string(res->ctx, -1, "_callbacks");
    duk_get_prop_lstring(res->ctx, -1, event_name, event_len);

    if (!duk_is_function(res->ctx, -1)) {
        /* No callback registered for this event */
        duk_pop_3(res->ctx);
        enif_mutex_unlock(res->lock);
        return atom_ok;  /* Silently succeed if no callback */
    }

    /* Push data argument */
    if (erlang_to_duk(env, res->ctx, argv[2]) != 0) {
        duk_pop_3(res->ctx);
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    /* Call the callback */
    if (duk_pcall(res->ctx, 1) != 0) {
        /* Error occurred */
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop_3(res->ctx);
        enif_mutex_unlock(res->lock);

        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    /* Convert result to Erlang term */
    result = duk_to_erlang(env, res->ctx, -1);
    duk_pop_3(res->ctx);  /* Pop result, _callbacks, Erlang */

    enif_mutex_unlock(res->lock);

    return enif_make_tuple2(env, atom_ok, result);
}

/* Register an Erlang function callable from JavaScript */
static ERL_NIF_TERM
nif_register_erlang_function(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get the function name */
    char name_buf[256];
    ErlNifBinary name_bin;
    const char *func_name = NULL;
    size_t func_name_len = 0;

    if (enif_get_atom(env, argv[1], name_buf, sizeof(name_buf), ERL_NIF_LATIN1) > 0) {
        func_name = name_buf;
        func_name_len = strlen(name_buf);
    } else if (enif_inspect_binary(env, argv[1], &name_bin)) {
        func_name = (const char *)name_bin.data;
        func_name_len = name_bin.size;
    } else if (enif_inspect_iolist_as_binary(env, argv[1], &name_bin)) {
        func_name = (const char *)name_bin.data;
        func_name_len = name_bin.size;
    } else {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Create JS function with the trampoline, storing name as property */
    duk_push_c_function(res->ctx, erlang_function_trampoline, DUK_VARARGS);
    duk_push_lstring(res->ctx, func_name, func_name_len);
    duk_put_prop_string(res->ctx, -2, "_erlang_name");
    duk_put_global_lstring(res->ctx, func_name, func_name_len);

    enif_mutex_unlock(res->lock);
    return atom_ok;
}

/* Set the result of an Erlang function call (for trampoline pattern) */
static ERL_NIF_TERM
nif_call_complete(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Check if the result is an error tuple {error, Reason} */
    int arity;
    const ERL_NIF_TERM *tuple_elements;
    int is_error = 0;
    if (enif_get_tuple(env, argv[1], &arity, &tuple_elements) &&
        arity == 2 && enif_is_atom(env, tuple_elements[0])) {
        char atom_buf[32];
        if (enif_get_atom(env, tuple_elements[0], atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1) > 0 &&
            strcmp(atom_buf, "error") == 0) {
            is_error = 1;
        }
    }

    if (is_error) {
        /* Store as error - will be handled in resume */
        enif_clear_env(res->pending_env);
        res->call_result = enif_make_copy(res->pending_env, argv[1]);
        res->has_call_result = 1;
    } else {
        /* Convert result to Duktape value */
        if (erlang_to_duk(env, res->ctx, argv[1]) != 0) {
            duk_push_undefined(res->ctx);
        }

        /* Store at __erlang_results__[pending_index] for indexed retrieval */
        duk_get_global_string(res->ctx, "__erlang_results__");
        duk_dup(res->ctx, -2);  /* Duplicate the result */
        duk_put_prop_index(res->ctx, -2, (duk_uarridx_t)res->pending_index);
        duk_pop_2(res->ctx);    /* Pop: object and original result */

        res->has_call_result = 0;  /* Result is in the JS object now */
    }

    enif_mutex_unlock(res->lock);
    return atom_ok;
}

/* Resume evaluation after Erlang function call completed */
static ERL_NIF_TERM
nif_eval_resume(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;
    ERL_NIF_TERM result;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Check if we have stored code to resume */
    if (!res->resume_code || res->resume_code_len == 0) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error,
            enif_make_atom(env, "no_resume_code"));
    }

    /* Handle error result from Erlang call */
    if (res->has_call_result) {
        int arity;
        const ERL_NIF_TERM *tuple_elements;
        if (enif_get_tuple(res->pending_env, res->call_result, &arity, &tuple_elements) &&
            arity == 2 && enif_is_atom(res->pending_env, tuple_elements[0])) {
            char atom_buf[32];
            if (enif_get_atom(res->pending_env, tuple_elements[0], atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1) > 0 &&
                strcmp(atom_buf, "error") == 0) {
                /* It's an error - store it so the trampoline can throw it */
                res->has_call_result = 0;

                /* Convert error reason to string for JS Error */
                if (erlang_to_duk(res->pending_env, res->ctx, tuple_elements[1]) != 0) {
                    duk_push_string(res->ctx, "unknown error");
                }
                const char *err_str = duk_safe_to_string(res->ctx, -1);

                /* Store error at the pending index so trampoline throws it */
                duk_get_global_string(res->ctx, "__erlang_results__");
                duk_push_sprintf(res->ctx, "__ERROR__:%s", err_str);
                duk_put_prop_index(res->ctx, -2, (duk_uarridx_t)res->pending_index);
                duk_pop_2(res->ctx);  /* Pop object and error string */
            }
        }
    }

    /* Reset call index for re-evaluation */
    res->call_index = 0;

    /* Re-evaluate the original code - trampoline will retrieve cached results */
    duk_push_lstring(res->ctx, res->resume_code, res->resume_code_len);

    if (duk_peval(res->ctx) != 0) {
        /* Check if this is another pending Erlang function call */
        if (res->pending_call) {
            ERL_NIF_TERM func_name = enif_make_atom(env, res->pending_func);
            ERL_NIF_TERM args = enif_make_copy(env, res->pending_args);
            res->pending_call = 0;

            duk_pop(res->ctx);  /* Pop the error */
            enif_mutex_unlock(res->lock);

            return enif_make_tuple3(env, atom_call_erlang, func_name, args);
        }

        /* Check if this is an error from a failed Erlang function */
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop(res->ctx);

        /* Clean up resume code on completion */
        enif_free(res->resume_code);
        res->resume_code = NULL;
        res->resume_code_len = 0;

        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    /* Success - get the result */
    result = duk_to_erlang(env, res->ctx, -1);
    duk_pop(res->ctx);

    /* Clean up resume code on completion */
    enif_free(res->resume_code);
    res->resume_code = NULL;
    res->resume_code_len = 0;

    enif_mutex_unlock(res->lock);

    return enif_make_tuple2(env, atom_ok, result);
}

/* Check if error message indicates a timeout */
static int is_timeout_error(const char *err_msg) {
    if (err_msg == NULL) return 0;
    /* Duktape throws RangeError with "execution timeout" message */
    return strstr(err_msg, "execution timeout") != NULL;
}

/* Evaluate JavaScript code */
static ERL_NIF_TERM
nif_eval(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;
    ErlNifBinary js_code;
    ERL_NIF_TERM result;
    uint64_t timeout_ms;

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

    /* Get timeout - either an integer or the atom 'infinity' */
    if (enif_is_atom(env, argv[2])) {
        /* Check if it's 'infinity' atom */
        char atom_buf[16];
        if (enif_get_atom(env, argv[2], atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1) > 0) {
            if (strcmp(atom_buf, "infinity") == 0) {
                timeout_ms = 0;  /* 0 means no timeout */
            } else {
                return enif_make_tuple2(env, atom_error, atom_badarg);
            }
        } else {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
    } else {
        ErlNifUInt64 t;
        if (!enif_get_uint64(env, argv[2], &t)) {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
        timeout_ms = (uint64_t)t;
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Start timeout if specified */
    start_timeout(res, timeout_ms);

    /* Reset call index and clear results for fresh evaluation */
    res->call_index = 0;
    duk_eval_string_noresult(res->ctx, "__erlang_results__ = {};");

    /* Push the code as a string and evaluate */
    duk_push_lstring(res->ctx, (const char *)js_code.data, js_code.size);

    if (duk_peval(res->ctx) != 0) {
        stop_timeout(res);

        /* Check if this is a timeout error */
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        if (is_timeout_error(err_msg)) {
            duk_pop(res->ctx);
            enif_mutex_unlock(res->lock);
            return enif_make_tuple2(env, atom_error, atom_timeout);
        }

        /* Check if this is a pending Erlang function call */
        if (res->pending_call) {
            /* Store the original code for resumption */
            if (res->resume_code) {
                enif_free(res->resume_code);
            }
            res->resume_code = enif_alloc(js_code.size + 1);
            if (res->resume_code) {
                memcpy(res->resume_code, js_code.data, js_code.size);
                res->resume_code[js_code.size] = '\0';
                res->resume_code_len = js_code.size;
            }

            /* Copy pending call info to return to Erlang */
            ERL_NIF_TERM func_name = enif_make_atom(env, res->pending_func);
            ERL_NIF_TERM args = enif_make_copy(env, res->pending_args);
            res->pending_call = 0;

            duk_pop(res->ctx);  /* Pop the error */
            enif_mutex_unlock(res->lock);

            return enif_make_tuple3(env, atom_call_erlang, func_name, args);
        }

        /* Regular error */
        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop(res->ctx);  /* Pop error */
        enif_mutex_unlock(res->lock);

        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    stop_timeout(res);

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
    uint64_t timeout_ms;

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

    /* Get timeout - either an integer or the atom 'infinity' */
    if (enif_is_atom(env, argv[3])) {
        char atom_buf[16];
        if (enif_get_atom(env, argv[3], atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1) > 0) {
            if (strcmp(atom_buf, "infinity") == 0) {
                timeout_ms = 0;
            } else {
                return enif_make_tuple2(env, atom_error, atom_badarg);
            }
        } else {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
    } else {
        ErlNifUInt64 t;
        if (!enif_get_uint64(env, argv[3], &t)) {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
        timeout_ms = (uint64_t)t;
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

        if (erlang_to_duk(env, res->ctx, value) != 0) {
            enif_map_iterator_destroy(env, &iter);
            enif_mutex_unlock(res->lock);
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }

        duk_put_global_lstring(res->ctx, var_name, var_name_len);
        enif_map_iterator_next(env, &iter);
    }
    enif_map_iterator_destroy(env, &iter);

    /* Start timeout if specified */
    start_timeout(res, timeout_ms);

    /* Reset call index and clear results for fresh evaluation */
    res->call_index = 0;
    duk_eval_string_noresult(res->ctx, "__erlang_results__ = {};");

    /* Evaluate the code */
    duk_push_lstring(res->ctx, (const char *)js_code.data, js_code.size);

    if (duk_peval(res->ctx) != 0) {
        stop_timeout(res);

        /* Check if this is a timeout error */
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        if (is_timeout_error(err_msg)) {
            duk_pop(res->ctx);
            enif_mutex_unlock(res->lock);
            return enif_make_tuple2(env, atom_error, atom_timeout);
        }

        /* Check if this is a pending Erlang function call */
        if (res->pending_call) {
            if (res->resume_code) {
                enif_free(res->resume_code);
            }
            res->resume_code = enif_alloc(js_code.size + 1);
            if (res->resume_code) {
                memcpy(res->resume_code, js_code.data, js_code.size);
                res->resume_code[js_code.size] = '\0';
                res->resume_code_len = js_code.size;
            }

            ERL_NIF_TERM func_name = enif_make_atom(env, res->pending_func);
            ERL_NIF_TERM args = enif_make_copy(env, res->pending_args);
            res->pending_call = 0;

            duk_pop(res->ctx);
            enif_mutex_unlock(res->lock);

            return enif_make_tuple3(env, atom_call_erlang, func_name, args);
        }

        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);

        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    stop_timeout(res);

    /* Convert result to Erlang term */
    result = duk_to_erlang(env, res->ctx, -1);
    duk_pop(res->ctx);

    enif_mutex_unlock(res->lock);

    return enif_make_tuple2(env, atom_ok, result);
}

/* Call a JavaScript function */
static ERL_NIF_TERM
nif_call(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;
    ERL_NIF_TERM result;
    uint64_t timeout_ms;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get the function name */
    char func_name_buf[256];
    ErlNifBinary func_name_bin;
    const char *func_name = NULL;
    size_t func_name_len = 0;

    if (enif_get_atom(env, argv[1], func_name_buf, sizeof(func_name_buf), ERL_NIF_LATIN1) > 0) {
        func_name = func_name_buf;
        func_name_len = strlen(func_name_buf);
    } else if (enif_inspect_binary(env, argv[1], &func_name_bin)) {
        func_name = (const char *)func_name_bin.data;
        func_name_len = func_name_bin.size;
    } else if (enif_inspect_iolist_as_binary(env, argv[1], &func_name_bin)) {
        func_name = (const char *)func_name_bin.data;
        func_name_len = func_name_bin.size;
    } else {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    /* Get the arguments list */
    if (!enif_is_list(env, argv[2])) {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    unsigned int args_len;
    if (!enif_get_list_length(env, argv[2], &args_len)) {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    /* Get timeout - either an integer or the atom 'infinity' */
    if (enif_is_atom(env, argv[3])) {
        char atom_buf[16];
        if (enif_get_atom(env, argv[3], atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1) > 0) {
            if (strcmp(atom_buf, "infinity") == 0) {
                timeout_ms = 0;
            } else {
                return enif_make_tuple2(env, atom_error, atom_badarg);
            }
        } else {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
    } else {
        ErlNifUInt64 t;
        if (!enif_get_uint64(env, argv[3], &t)) {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
        timeout_ms = (uint64_t)t;
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get the global function */
    if (!duk_get_global_lstring(res->ctx, func_name, func_name_len)) {
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);
        ERL_NIF_TERM err_msg = make_binary_from_string(env, "function not found", 18, atom_enomem);
        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_msg));
    }

    /* Check if it's actually a function */
    if (!duk_is_function(res->ctx, -1)) {
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);
        ERL_NIF_TERM err_msg = make_binary_from_string(env, "not a function", 14, atom_enomem);
        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_msg));
    }

    /* Reset call index and clear results for fresh call */
    res->call_index = 0;
    duk_eval_string_noresult(res->ctx, "__erlang_results__ = {};");

    /* Store function for potential resume */
    duk_dup(res->ctx, -1);
    duk_put_global_string(res->ctx, "__call_func__");

    /* Build __call_args__ array for potential resume */
    duk_push_array(res->ctx);
    ERL_NIF_TERM args_list = argv[2];
    ERL_NIF_TERM head, tail;
    unsigned int idx = 0;
    while (enif_get_list_cell(env, args_list, &head, &tail)) {
        if (erlang_to_duk(env, res->ctx, head) != 0) {
            duk_pop_2(res->ctx);
            enif_mutex_unlock(res->lock);
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
        duk_put_prop_index(res->ctx, -2, idx);
        idx++;
        args_list = tail;
    }
    duk_put_global_string(res->ctx, "__call_args__");

    /* Push arguments for the actual call */
    args_list = argv[2];
    unsigned int pushed_args = 0;
    while (enif_get_list_cell(env, args_list, &head, &tail)) {
        if (erlang_to_duk(env, res->ctx, head) != 0) {
            duk_pop_n(res->ctx, (duk_idx_t)(pushed_args + 1));
            enif_mutex_unlock(res->lock);
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
        pushed_args++;
        args_list = tail;
    }

    /* Start timeout if specified */
    start_timeout(res, timeout_ms);

    /* Call the function */
    if (duk_pcall(res->ctx, (duk_idx_t)pushed_args) != 0) {
        stop_timeout(res);

        /* Check if this is a timeout error */
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        if (is_timeout_error(err_msg)) {
            duk_pop(res->ctx);
            enif_mutex_unlock(res->lock);
            return enif_make_tuple2(env, atom_error, atom_timeout);
        }

        /* Check if this is a pending Erlang function call */
        if (res->pending_call) {
            const char *resume_str = "__call_func__.apply(null, __call_args__)";
            size_t resume_len = strlen(resume_str);
            if (res->resume_code) {
                enif_free(res->resume_code);
            }
            res->resume_code = enif_alloc(resume_len + 1);
            if (res->resume_code) {
                memcpy(res->resume_code, resume_str, resume_len + 1);
                res->resume_code_len = resume_len;
            }

            ERL_NIF_TERM erl_func_name = enif_make_atom(env, res->pending_func);
            ERL_NIF_TERM erl_args = enif_make_copy(env, res->pending_args);
            res->pending_call = 0;

            duk_pop(res->ctx);
            enif_mutex_unlock(res->lock);

            return enif_make_tuple3(env, atom_call_erlang, erl_func_name, erl_args);
        }

        /* Regular error */
        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);

        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    stop_timeout(res);

    /* Convert result to Erlang term */
    result = duk_to_erlang(env, res->ctx, -1);
    duk_pop(res->ctx);

    enif_mutex_unlock(res->lock);

    return enif_make_tuple2(env, atom_ok, result);
}

/* Register a CommonJS module */
static ERL_NIF_TERM
nif_register_module(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;
    duktape_ctx_t *res;
    ErlNifBinary mod_id_bin;
    ErlNifBinary source_bin;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get the module ID */
    char mod_id_buf[256];
    const char *mod_id = NULL;
    size_t mod_id_len = 0;

    if (enif_get_atom(env, argv[1], mod_id_buf, sizeof(mod_id_buf), ERL_NIF_LATIN1) > 0) {
        mod_id = mod_id_buf;
        mod_id_len = strlen(mod_id_buf);
    } else if (enif_inspect_binary(env, argv[1], &mod_id_bin)) {
        mod_id = (const char *)mod_id_bin.data;
        mod_id_len = mod_id_bin.size;
    } else if (enif_inspect_iolist_as_binary(env, argv[1], &mod_id_bin)) {
        mod_id = (const char *)mod_id_bin.data;
        mod_id_len = mod_id_bin.size;
    } else {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    /* Get the module source code */
    if (!enif_inspect_binary(env, argv[2], &source_bin)) {
        if (!enif_inspect_iolist_as_binary(env, argv[2], &source_bin)) {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Store module in the stash */
    duk_push_global_stash(res->ctx);
    duk_get_prop_string(res->ctx, -1, MODULE_STASH_KEY);
    duk_push_lstring(res->ctx, (const char *)source_bin.data, source_bin.size);
    duk_put_prop_lstring(res->ctx, -2, mod_id, mod_id_len);
    duk_pop_2(res->ctx);

    enif_mutex_unlock(res->lock);

    return atom_ok;
}

/* Require a CommonJS module */
static ERL_NIF_TERM
nif_require(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;
    duktape_ctx_t *res;
    ErlNifBinary mod_id_bin;
    ERL_NIF_TERM result;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get the module ID */
    char mod_id_buf[256];
    const char *mod_id = NULL;
    size_t mod_id_len = 0;

    if (enif_get_atom(env, argv[1], mod_id_buf, sizeof(mod_id_buf), ERL_NIF_LATIN1) > 0) {
        mod_id = mod_id_buf;
        mod_id_len = strlen(mod_id_buf);
    } else if (enif_inspect_binary(env, argv[1], &mod_id_bin)) {
        mod_id = (const char *)mod_id_bin.data;
        mod_id_len = mod_id_bin.size;
    } else if (enif_inspect_iolist_as_binary(env, argv[1], &mod_id_bin)) {
        mod_id = (const char *)mod_id_bin.data;
        mod_id_len = mod_id_bin.size;
    } else {
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Call require(mod_id) */
    duk_get_global_string(res->ctx, "require");
    duk_push_lstring(res->ctx, mod_id, mod_id_len);

    if (duk_pcall(res->ctx, 1) != 0) {
        /* Error occurred */
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);

        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    /* Convert result (module.exports) to Erlang term */
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
 * CBOR encoding/decoding
 * ============================================================================ */

/* Encode an Erlang value to CBOR binary */
static ERL_NIF_TERM
nif_cbor_encode(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Convert Erlang value to JS value on Duktape stack */
    if (erlang_to_duk(env, res->ctx, argv[1]) != 0) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_badarg);
    }

    /* Encode to CBOR - replaces top of stack with buffer */
    duk_cbor_encode(res->ctx, -1, 0);

    /* Get the buffer data */
    duk_size_t cbor_len;
    const void *cbor_data = duk_get_buffer_data(res->ctx, -1, &cbor_len);

    if (cbor_data == NULL) {
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    /* Create Erlang binary from CBOR data */
    ERL_NIF_TERM result_bin;
    unsigned char *bin_data = enif_make_new_binary(env, cbor_len, &result_bin);
    if (bin_data == NULL) {
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    if (cbor_len > 0) {
        safe_memcpy(bin_data, cbor_len, cbor_data, cbor_len);
    }

    duk_pop(res->ctx);
    enif_mutex_unlock(res->lock);

    return enif_make_tuple2(env, atom_ok, result_bin);
}

/* Helper for CBOR decode safe call */
static duk_ret_t
cbor_decode_safe_call(duk_context *ctx, void *udata)
{
    (void)udata;
    duk_cbor_decode(ctx, -1, 0);
    return 1;
}

/* Decode a CBOR binary to Erlang value */
static ERL_NIF_TERM
nif_cbor_decode(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;
    ErlNifBinary cbor_bin;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Get CBOR binary */
    if (!enif_inspect_binary(env, argv[1], &cbor_bin)) {
        if (!enif_inspect_iolist_as_binary(env, argv[1], &cbor_bin)) {
            return enif_make_tuple2(env, atom_error, atom_badarg);
        }
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed || res->ctx == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Push CBOR data as buffer */
    void *buf = duk_push_fixed_buffer(res->ctx, cbor_bin.size);
    if (buf == NULL) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_enomem);
    }

    if (cbor_bin.size > 0) {
        safe_memcpy(buf, cbor_bin.size, cbor_bin.data, cbor_bin.size);
    }

    /* Decode CBOR - use protected call to catch errors */
    if (duk_safe_call(res->ctx, cbor_decode_safe_call, NULL, 1, 1) != DUK_EXEC_SUCCESS) {
        /* Decoding failed - get error message */
        const char *err_msg = duk_safe_to_string(res->ctx, -1);
        size_t err_len = err_msg ? strlen(err_msg) : 0;
        ERL_NIF_TERM err_bin = make_binary_from_string(env, err_msg, err_len, atom_enomem);
        duk_pop(res->ctx);
        enif_mutex_unlock(res->lock);

        return enif_make_tuple2(env, atom_error,
            enif_make_tuple2(env, atom_js_error, err_bin));
    }

    /* Convert JS value to Erlang term */
    ERL_NIF_TERM result = duk_to_erlang(env, res->ctx, -1);

    duk_pop(res->ctx);
    enif_mutex_unlock(res->lock);

    return enif_make_tuple2(env, atom_ok, result);
}

/* ============================================================================
 * Metrics NIF functions
 * ============================================================================ */

/* Get memory statistics for a Duktape context */
static ERL_NIF_TERM
nif_get_memory_stats(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Build metrics map */
    ERL_NIF_TERM keys[6];
    ERL_NIF_TERM values[6];

    keys[0] = atom_heap_bytes;
    values[0] = enif_make_uint64(env, res->metrics.heap_bytes);

    keys[1] = atom_heap_peak;
    values[1] = enif_make_uint64(env, res->metrics.heap_peak);

    keys[2] = atom_alloc_count;
    values[2] = enif_make_uint64(env, res->metrics.alloc_count);

    keys[3] = atom_realloc_count;
    values[3] = enif_make_uint64(env, res->metrics.realloc_count);

    keys[4] = atom_free_count;
    values[4] = enif_make_uint64(env, res->metrics.free_count);

    keys[5] = atom_gc_runs;
    values[5] = enif_make_uint64(env, res->metrics.gc_runs);

    ERL_NIF_TERM map;
    enif_make_map_from_arrays(env, keys, values, 6, &map);

    enif_mutex_unlock(res->lock);

    return enif_make_tuple2(env, atom_ok, map);
}

/* Trigger garbage collection on a Duktape context */
static ERL_NIF_TERM
nif_gc(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    (void)argc;

    duktape_ctx_t *res;

    /* Get the context */
    res = get_context(env, argv[0]);
    if (!res) {
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    enif_mutex_lock(res->lock);

    if (res->destroyed) {
        enif_mutex_unlock(res->lock);
        return enif_make_tuple2(env, atom_error, atom_invalid_context);
    }

    /* Run garbage collection */
    duk_gc(res->ctx, 0);
    res->metrics.gc_runs++;

    enif_mutex_unlock(res->lock);

    return atom_ok;
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
    /* Event atoms */
    atom_duktape = enif_make_atom(env, "duktape");
    atom_log = enif_make_atom(env, "log");
    atom_debug = enif_make_atom(env, "debug");
    atom_info = enif_make_atom(env, "info");
    atom_warning = enif_make_atom(env, "warning");
    atom_level = enif_make_atom(env, "level");
    atom_message = enif_make_atom(env, "message");
    atom_handler = enif_make_atom(env, "handler");
    /* Erlang function call atoms */
    atom_call_erlang = enif_make_atom(env, "call_erlang");

    /* Metrics atoms */
    atom_heap_bytes = enif_make_atom(env, "heap_bytes");
    atom_heap_peak = enif_make_atom(env, "heap_peak");
    atom_alloc_count = enif_make_atom(env, "alloc_count");
    atom_realloc_count = enif_make_atom(env, "realloc_count");
    atom_free_count = enif_make_atom(env, "free_count");
    atom_gc_runs = enif_make_atom(env, "gc_runs");

    /* Timeout atom */
    atom_timeout = enif_make_atom(env, "timeout");

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

/* NIF function table
 *
 * CPU-bound operations that execute JavaScript or perform intensive
 * serialization are marked as dirty NIFs to prevent blocking the
 * Erlang scheduler. This is important because JavaScript execution
 * time is unbounded and user-provided code could run indefinitely.
 */
static ErlNifFunc nif_funcs[] = {
    /* Fast operations - run on normal scheduler */
    {"nif_info", 0, nif_info, 0},
    {"nif_new_context", 0, nif_new_context, 0},
    {"nif_new_context_opts", 1, nif_new_context_opts, 0},
    {"nif_destroy_context", 1, nif_destroy_context, 0},
    {"nif_register_module", 3, nif_register_module, 0},
    {"nif_send", 3, nif_send, 0},
    {"nif_register_erlang_function", 2, nif_register_erlang_function, 0},
    {"nif_call_complete", 2, nif_call_complete, 0},
    {"nif_get_memory_stats", 1, nif_get_memory_stats, 0},
    {"nif_gc", 1, nif_gc, 0},

    /* CPU-bound operations - run on dirty scheduler */
    {"nif_eval", 3, nif_eval, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"nif_eval_bindings", 4, nif_eval_bindings, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"nif_call", 4, nif_call, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"nif_require", 2, nif_require, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"nif_eval_resume", 1, nif_eval_resume, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"nif_cbor_encode", 2, nif_cbor_encode, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"nif_cbor_decode", 2, nif_cbor_decode, ERL_NIF_DIRTY_JOB_CPU_BOUND}
};

ERL_NIF_INIT(duktape, nif_funcs, on_load, NULL, on_upgrade, on_unload)
