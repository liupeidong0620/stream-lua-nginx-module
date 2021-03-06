// add by chrono

/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_stream_lua_directive.h"
#include "ngx_stream_lua_logby.h"
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
#include "ngx_stream_lua_shdict.h"
#include "ngx_stream_lua_util.h"
#include "ngx_stream_lua_exception.h"


static ngx_int_t ngx_stream_lua_log_by_chunk(lua_State *L, ngx_stream_session_t *s);


static void
ngx_stream_lua_log_by_lua_env(lua_State *L, ngx_stream_session_t *s)
{
    /*  set nginx request pointer to current lua thread's globals table */
    ngx_stream_lua_set_session(L, s);

    /**
     * we want to create empty environment for current script
     *
     * newt = {}
     * newt["_G"] = newt
     * setmetatable(newt, {__index = _G})
     *
     * if a function or symbol is not defined in our env, __index will lookup
     * in the global env.
     *
     * all variables created in the script-env will be thrown away at the end
     * of the script run.
     * */
    ngx_stream_lua_create_new_globals_table(L, 0 /* narr */, 1 /* nrec */);

    /*  {{{ make new env inheriting main thread's globals table */
    lua_createtable(L, 0, 1);    /*  the metatable for the new env */
    ngx_stream_lua_get_globals_table(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);    /*  setmetatable({}, {__index = _G}) */
    /*  }}} */

    lua_setfenv(L, -2);    /*  set new running env for the code closure */
}


ngx_int_t
ngx_stream_lua_log_handler(ngx_stream_session_t *s)
{
    ngx_stream_lua_srv_conf_t     *lscf;
    ngx_stream_lua_ctx_t          *ctx;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    if (lscf->log_handler == NULL) {
        dd("no log handler found");
        return NGX_DECLINED;
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);

    dd("ctx = %p", ctx);

    if (ctx == NULL) {
        ctx = ngx_stream_lua_create_ctx(s);
        if (ctx == NULL) {
            return NGX_ERROR;
        }
    }

    ctx->context = NGX_STREAM_LUA_CONTEXT_LOG;

    dd("calling log handler");
    return lscf->log_handler(s, ctx);
}


ngx_int_t
ngx_stream_lua_log_handler_inline(ngx_stream_session_t *s)
{
    lua_State                   *L;
    ngx_int_t                    rc;
    ngx_stream_lua_srv_conf_t     *lscf;

    dd("log by lua inline");

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    L = ngx_stream_lua_get_lua_vm(s, NULL);

    /*  load Lua inline script (w/ cache) sp = 1 */
    rc = ngx_stream_lua_cache_loadbuffer(s->connection->log, L,
                                       lscf->log_src.data,
                                       lscf->log_src.len,
                                       lscf->log_src_key,
                                       (const char *) lscf->log_chunkname);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    return ngx_stream_lua_log_by_chunk(L, s);
}


ngx_int_t
ngx_stream_lua_log_handler_file(ngx_stream_session_t *s)
{
    lua_State                       *L;
    ngx_int_t                        rc;
    u_char                          *script_path;
    ngx_stream_lua_srv_conf_t         *lscf;
    ngx_str_t                        eval_src;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    //if (ngx_stream_complex_value(s, &lscf->log_src, &eval_src) != NGX_OK) {
    //    return NGX_ERROR;
    //}
    eval_src = lscf->log_src;

    script_path = ngx_stream_lua_rebase_path(s->connection->pool, eval_src.data,
                                           eval_src.len);

    if (script_path == NULL) {
        return NGX_ERROR;
    }

    L = ngx_stream_lua_get_lua_vm(s, NULL);

    /*  load Lua script file (w/ cache)        sp = 1 */
    rc = ngx_stream_lua_cache_loadfile(s->connection->log, L, script_path,
                                     lscf->log_src_key);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    return ngx_stream_lua_log_by_chunk(L, s);
}


ngx_int_t
ngx_stream_lua_log_by_chunk(lua_State *L, ngx_stream_session_t *s)
{
    ngx_int_t        rc;
    u_char          *err_msg;
    size_t           len;
#if (NGX_PCRE)
    ngx_pool_t      *old_pool;
#endif

    /*  set Lua VM panic handler */
    lua_atpanic(L, ngx_stream_lua_atpanic);

    NGX_LUA_EXCEPTION_TRY {

        /* initialize nginx context in Lua VM, code chunk at stack top sp = 1 */
        ngx_stream_lua_log_by_lua_env(L, s);

#if (NGX_PCRE)
        /* XXX: work-around to nginx regex subsystem */
        old_pool = ngx_stream_lua_pcre_malloc_init(s->connection->pool);
#endif

        lua_pushcfunction(L, ngx_stream_lua_traceback);
        lua_insert(L, 1);  /* put it under chunk and args */

        /*  protected call user code */
        rc = lua_pcall(L, 0, 1, 1);

        lua_remove(L, 1);  /* remove traceback function */

#if (NGX_PCRE)
        /* XXX: work-around to nginx regex subsystem */
        ngx_stream_lua_pcre_malloc_done(old_pool);
#endif

        if (rc != 0) {
            /*  error occurred when running loaded code */
            err_msg = (u_char *) lua_tolstring(L, -1, &len);

            if (err_msg == NULL) {
                err_msg = (u_char *) "unknown reason";
                len = sizeof("unknown reason") - 1;
            }

            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "failed to run log_by_lua*: %*s", len, err_msg);

            lua_settop(L, 0);    /*  clear remaining elems on stack */

            return NGX_ERROR;
        }

    } NGX_LUA_EXCEPTION_CATCH {

        dd("nginx execution restored");
        return NGX_ERROR;
    }

    /*  clear Lua stack */
    lua_settop(L, 0);

    return NGX_OK;
}

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
