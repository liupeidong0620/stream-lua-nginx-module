// add by chrono

/*
 * Copyright (C) Xiaozhe Wang (chaoslawful)
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_stream_lua_filterby.h"
#include "ngx_stream_lua_exception.h"
#include "ngx_stream_lua_util.h"
#include "ngx_stream_lua_pcrefix.h"
#include "ngx_stream_lua_time.h"
#include "ngx_stream_lua_log.h"
#include "ngx_stream_lua_regex.h"
#include "ngx_stream_lua_cache.h"
//#include "ngx_stream_lua_headers.h"
//#include "ngx_stream_lua_variable.h"
#include "ngx_stream_lua_string.h"
#include "ngx_stream_lua_misc.h"
#include "ngx_stream_lua_consts.h"
#include "ngx_stream_lua_output.h"


static void ngx_stream_lua_filter_by_lua_env(lua_State *L,
    ngx_stream_session_t *s, ngx_chain_t *in, ngx_uint_t from_upstream);
static ngx_stream_filter_pt ngx_stream_next_filter;


/* key for the ngx_chain_t *in pointer in the Lua thread */
#define ngx_stream_lua_chain_key  "__ngx_cl"

// add by chrono
// key for from_upstream
#define ngx_stream_lua_from_upstream_key "__ngx_from_upstream"

/**
 * Set environment table for the given code closure.
 *
 * Before:
 *         | code closure | <- top
 *         |      ...     |
 *
 * After:
 *         | code closure | <- top
 *         |      ...     |
 * */
static void
ngx_stream_lua_filter_by_lua_env(lua_State *L, ngx_stream_session_t *s,
    ngx_chain_t *in, ngx_uint_t from_upstream)
{
    /*  set nginx request pointer to current lua thread's globals table */
    ngx_stream_lua_set_session(L, s);

    //lua_pushlightuserdata(L, in);
    //lua_setglobal(L, ngx_stream_lua_chain_key);

#if 1
    // set ngx_stream_lua_from_upstream_key => from_upstream
    lua_pushboolean(L, from_upstream);
    lua_setglobal(L, ngx_stream_lua_from_upstream_key);
#endif

    /**
     * we want to create empty environment for current script
     *
     * setmetatable({}, {__index = _G})
     *
     * if a function or symbol is not defined in our env, __index will lookup
     * in the global env.
     *
     * all variables created in the script-env will be thrown away at the end
     * of the script run.
     * */
    ngx_stream_lua_create_new_globals_table(L, 0 /* narr */, 1 /* nrec */);

    /*  {{{ make new env inheriting main thread's globals table */
    lua_createtable(L, 0, 1 /* nrec */);    /*  the metatable for the new
                                                env */
    ngx_stream_lua_get_globals_table(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);    /*  setmetatable({}, {__index = _G}) */
    /*  }}} */

    lua_setfenv(L, -2);    /*  set new running env for the code closure */
}


ngx_int_t
ngx_stream_lua_filter_by_chunk(lua_State *L, ngx_stream_session_t *s,
    ngx_chain_t *in, ngx_uint_t from_upstream)
{
    ngx_int_t        rc;
    u_char          *err_msg;
    size_t           len;
#if (NGX_PCRE)
    ngx_pool_t      *old_pool;
#endif

    dd("initialize nginx context in Lua VM, code chunk at stack top  sp = 1");
    ngx_stream_lua_filter_by_lua_env(L, s, in, from_upstream);

#if (NGX_PCRE)
    /* XXX: work-around to nginx regex subsystem */
    old_pool = ngx_stream_lua_pcre_malloc_init(s->connection->pool);
#endif

    lua_pushcfunction(L, ngx_stream_lua_traceback);
    lua_insert(L, 1);  /* put it under chunk and args */

    dd("protected call user code");
    rc = lua_pcall(L, 0, 1, 1);

    lua_remove(L, 1);  /* remove traceback function */

#if (NGX_PCRE)
    /* XXX: work-around to nginx regex subsystem */
    ngx_stream_lua_pcre_malloc_done(old_pool);
#endif

    if (rc != 0) {

        /*  error occurred */
        err_msg = (u_char *) lua_tolstring(L, -1, &len);

        if (err_msg == NULL) {
            err_msg = (u_char *) "unknown reason";
            len = sizeof("unknown reason") - 1;
        }

        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "failed to run filter_by_lua*: %*s", len, err_msg);

        lua_settop(L, 0);    /*  clear remaining elems on stack */

        return NGX_ERROR;
    }

    /* rc == 0 */

    rc = (ngx_int_t) lua_tointeger(L, -1);

    dd("got return value: %d", (int) rc);

    lua_settop(L, 0);

    if (rc == NGX_ERROR || rc >= NGX_STREAM_BAD_REQUEST) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_stream_lua_filter_inline(ngx_stream_session_t *s, ngx_chain_t *in, ngx_uint_t from_upstream)
{
    lua_State                   *L;
    ngx_int_t                    rc;
    ngx_stream_lua_srv_conf_t     *lscf;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    L = ngx_stream_lua_get_lua_vm(s, NULL);

    /*  load Lua inline script (w/ cache) sp = 1 */
    rc = ngx_stream_lua_cache_loadbuffer(s->connection->log, L,
                                       lscf->filter_src.data,
                                       lscf->filter_src.len,
                                       lscf->filter_src_key,
                                       "=filter_by_lua");
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    rc = ngx_stream_lua_filter_by_chunk(L, s, in, from_upstream);

    dd("stream filter by chunk returns %d", (int) rc);

    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_stream_lua_filter_file(ngx_stream_session_t *s, ngx_chain_t *in, ngx_uint_t from_upstream)
{
    lua_State                       *L;
    ngx_int_t                        rc;
    u_char                          *script_path;
    ngx_stream_lua_srv_conf_t         *lscf;
    ngx_str_t                        eval_src;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    /* Eval nginx variables in code path string first */
    //if (ngx_http_complex_value(r, &lscf->body_filter_src, &eval_src)
    //    != NGX_OK)
    //{
    //    return NGX_ERROR;
    //}
    eval_src = lscf->filter_src;

    script_path = ngx_stream_lua_rebase_path(s->connection->pool, eval_src.data,
                                           eval_src.len);

    if (script_path == NULL) {
        return NGX_ERROR;
    }

    L = ngx_stream_lua_get_lua_vm(s, NULL);

    /*  load Lua script file (w/ cache)        sp = 1 */
    rc = ngx_stream_lua_cache_loadfile(s->connection->log, L, script_path,
                                     lscf->filter_src_key);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    /*  make sure we have a valid code chunk */
    ngx_stream_lua_assert(lua_isfunction(L, -1));

    rc = ngx_stream_lua_filter_by_chunk(L, s, in, from_upstream);

    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_stream_lua_filter(ngx_stream_session_t *s, ngx_chain_t *in, ngx_uint_t from_upstream)
{
    ngx_stream_lua_srv_conf_t     *lscf;
    ngx_stream_lua_ctx_t          *ctx;
    ngx_int_t                    rc;
    uint16_t                     old_context;
    ngx_stream_lua_cleanup_t          *cln;
    //lua_State                   *L;
    //ngx_chain_t                 *out;

    //ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
    //               "lua body filter for user lua code, uri \"%V\"", &r->uri);

    if (in == NULL) {
        return ngx_stream_next_filter(s, in, from_upstream);
    }

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    if (lscf->filter_handler == NULL) {
        dd("no filter handler found");
        return ngx_stream_next_filter(s, in, from_upstream);
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);

    dd("ctx = %p", ctx);

    if (ctx == NULL) {
        ctx = ngx_stream_lua_create_ctx(s);
        if (ctx == NULL) {
            return NGX_ERROR;
        }
    }

#if 0
    if (ctx->seen_last_in_filter) {
        for (/* void */; in; in = in->next) {
            dd("mark the buf as consumed: %d", (int) ngx_buf_size(in->buf));
            in->buf->pos = in->buf->last;
            in->buf->file_pos = in->buf->file_last;
        }

        return NGX_OK;
    }
#endif

    if (ctx->cleanup == NULL) {
        cln = ngx_stream_lua_cleanup_add(s, 0);
        if (cln == NULL) {
            return NGX_ERROR;
        }

        cln->handler = ngx_stream_lua_session_cleanup_handler;
        cln->data = ctx;
        ctx->cleanup = cln;
    }

    old_context = ctx->context;
    ctx->context = NGX_STREAM_LUA_CONTEXT_FILTER;

    dd("calling stream filter handler");
    rc = lscf->filter_handler(s, in, from_upstream);

    dd("calling stream filter handler returned %d", (int) rc);

    ctx->context = old_context;

    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    // do not modify chain now
    return ngx_stream_next_filter(s, in, from_upstream);
#if 0
    L = ngx_stream_lua_get_lua_vm(s, ctx);

    lua_getglobal(L, ngx_stream_lua_chain_key);
    out = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (in == out) {
        return ngx_stream_next_filter(r, in, from_upstream);
    }

    if (out == NULL) {
        /* do not forward NULL to the next filters because the input is
         * not NULL */
        return NGX_OK;
    }

    /* in != out */
    rc = ngx_stream_next_filter(s, out, from_upstream);
    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

#if nginx_version >= 1001004
    ngx_chain_update_chains(s->connection->pool,
#else
    ngx_chain_update_chains(
#endif
                            &ctx->free_bufs, &ctx->busy_bufs, &out,
                            (ngx_buf_tag_t) &ngx_stream_lua_module);

    return rc;
#endif
}


ngx_int_t
ngx_stream_lua_filter_init(void)
{
    dd("calling stream filter init");
    ngx_stream_next_filter = ngx_stream_top_filter;
    ngx_stream_top_filter = ngx_stream_lua_filter;

    return NGX_OK;
}


int
ngx_stream_lua_filter_param_get(lua_State *L)
{
    int idx;
    unsigned from_upstream;

    // check index => arg[x]
    idx = luaL_checkint(L, 2);

    dd("index: %d", idx);

    if (idx != 1) {
        lua_pushnil(L);
        return 1;
    }

    lua_getglobal(L, ngx_stream_lua_from_upstream_key);

    // now only arg[1] = from_upstream
    from_upstream = lua_toboolean(L, -1);
    lua_pushboolean(L, from_upstream);

    return 1;
#if 0
    u_char              *data, *p;
    size_t               size;
    ngx_chain_t         *cl;
    ngx_buf_t           *b;
    int                  idx;
    ngx_chain_t         *in;

    idx = luaL_checkint(L, 2);

    dd("index: %d", idx);

    if (idx != 1 && idx != 2) {
        lua_pushnil(L);
        return 1;
    }

    lua_getglobal(L, ngx_stream_lua_chain_key);
    in = lua_touserdata(L, -1);

    if (idx == 2) {
        /* asking for the eof argument */

        for (cl = in; cl; cl = cl->next) {
            if (cl->buf->last_buf || cl->buf->last_in_chain) {
                lua_pushboolean(L, 1);
                return 1;
            }
        }

        lua_pushboolean(L, 0);
        return 1;
    }

    /* idx == 1 */

    size = 0;

    if (in == NULL) {
        /* being a cleared chain on the Lua land */
        lua_pushliteral(L, "");
        return 1;
    }

    if (in->next == NULL) {

        dd("seen only single buffer");

        b = in->buf;
        lua_pushlstring(L, (char *) b->pos, b->last - b->pos);
        return 1;
    }

    dd("seen multiple buffers");

    for (cl = in; cl; cl = cl->next) {
        b = cl->buf;

        size += b->last - b->pos;

        if (b->last_buf || b->last_in_chain) {
            break;
        }
    }

    data = (u_char *) lua_newuserdata(L, size);

    for (p = data, cl = in; cl; cl = cl->next) {
        b = cl->buf;
        p = ngx_copy(p, b->pos, b->last - b->pos);

        if (b->last_buf || b->last_in_chain) {
            break;
        }
    }

    lua_pushlstring(L, (char *) data, size);
    return 1;
#endif
}


int
ngx_stream_lua_filter_param_set(lua_State *L, ngx_stream_session_t *s,
    ngx_stream_lua_ctx_t *ctx)
{
#if 0
    int                      type;
    int                      idx;
    int                      found;
    u_char                  *data;
    size_t                   size;
    unsigned                 last;
    unsigned                 flush = 0;
    ngx_buf_t               *b;
    ngx_chain_t             *cl;
    ngx_chain_t             *in;

    idx = luaL_checkint(L, 2);

    dd("index: %d", idx);

    if (idx != 1 && idx != 2) {
        return luaL_error(L, "bad index: %d", idx);
    }

    if (idx == 2) {
        /* overwriting the eof flag */
        last = lua_toboolean(L, 3);

        lua_getglobal(L, ngx_stream_lua_chain_key);
        in = lua_touserdata(L, -1);
        lua_pop(L, 1);

        if (last) {
            ctx->seen_last_in_filter = 1;

            /* the "in" chain cannot be NULL and we set the "last_buf" or
             * "last_in_chain" flag in the last buf of "in" */

            for (cl = in; cl; cl = cl->next) {
                if (cl->next == NULL) {
                    if (r == r->main) {
                        cl->buf->last_buf = 1;

                    } else {
                        cl->buf->last_in_chain = 1;
                    }

                    break;
                }
            }

        } else {
            /* last == 0 */

            found = 0;

            for (cl = in; cl; cl = cl->next) {
                b = cl->buf;

                if (b->last_buf) {
                    b->last_buf = 0;
                    found = 1;
                }

                if (b->last_in_chain) {
                    b->last_in_chain = 0;
                    found = 1;
                }

                if (found && b->last == b->pos && !ngx_buf_in_memory(b)) {
                    /* make it a special sync buf to make
                     * ngx_http_write_filter_module happy. */
                    b->sync = 1;
                }
            }

            ctx->seen_last_in_filter = 0;
        }

        return 0;
    }

    /* idx == 1, overwriting the chunk data */

    type = lua_type(L, 3);

    switch (type) {
    case LUA_TSTRING:
    case LUA_TNUMBER:
        data = (u_char *) lua_tolstring(L, 3, &size);
        break;

    case LUA_TNIL:
        /* discard the buffers */

        lua_getglobal(L, ngx_stream_lua_chain_key); /* key val */
        in = lua_touserdata(L, -1);
        lua_pop(L, 1);

        last = 0;

        for (cl = in; cl; cl = cl->next) {
            b = cl->buf;

            if (b->flush) {
                flush = 1;
            }

            if (b->last_in_chain || b->last_buf) {
                last = 1;
            }

            dd("mark the buf as consumed: %d", (int) ngx_buf_size(b));
            b->pos = b->last;
        }

        /* cl == NULL */

        goto done;

    case LUA_TTABLE:
        size = ngx_stream_lua_calc_strlen_in_table(L, 3 /* index */, 3 /* arg */,
                                                 1 /* strict */);
        data = NULL;
        break;

    default:
        return luaL_error(L, "bad chunk data type: %s",
                          lua_typename(L, type));
    }

    lua_getglobal(L, ngx_stream_lua_chain_key);
    in = lua_touserdata(L, -1);
    lua_pop(L, 1);

    last = 0;

    for (cl = in; cl; cl = cl->next) {
        b = cl->buf;

        if (b->flush) {
            flush = 1;
        }

        if (b->last_buf || b->last_in_chain) {
            last = 1;
        }

        dd("mark the buf as consumed: %d", (int) ngx_buf_size(cl->buf));
        cl->buf->pos = cl->buf->last;
    }

    /* cl == NULL */

    if (size == 0) {
        goto done;
    }

    cl = ngx_stream_lua_chain_get_free_buf(r->connection->log, r->pool,
                                         &ctx->free_bufs, size);
    if (cl == NULL) {
        return luaL_error(L, "no memory");
    }

    if (type == LUA_TTABLE) {
        cl->buf->last = ngx_stream_lua_copy_str_in_table(L, 3, cl->buf->last);

    } else {
        cl->buf->last = ngx_copy(cl->buf->pos, data, size);
    }

done:

    if (last || flush) {
        if (cl == NULL) {
            cl = ngx_stream_lua_chain_get_free_buf(r->connection->log,
                                                 r->pool,
                                                 &ctx->free_bufs, 0);
            if (cl == NULL) {
                return luaL_error(L, "no memory");
            }
        }

        if (last) {
            ctx->seen_last_in_filter = 1;

            if (r == r->main) {
                cl->buf->last_buf = 1;

            } else {
                cl->buf->last_in_chain = 1;
            }
        }

        if (flush) {
            cl->buf->flush = 1;
        }
    }

    lua_pushlightuserdata(L, cl);
    lua_setglobal(L, ngx_stream_lua_chain_key);
#endif
    return 0;
}

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
