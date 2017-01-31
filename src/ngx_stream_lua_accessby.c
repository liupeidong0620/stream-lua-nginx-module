// add by chrono

/*
 * Copyright (C) Xiaozhe Wang (chaoslawful)
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include <nginx.h>
#include "ngx_stream_lua_accessby.h"
#include "ngx_stream_lua_util.h"
#include "ngx_stream_lua_exception.h"
#include "ngx_stream_lua_cache.h"

// add by chrono
#define NGX_STREAM_SPECIAL_RESPONSE 300

static ngx_int_t ngx_stream_lua_access_by_chunk(lua_State *L,
    ngx_stream_session_t *s);


ngx_int_t
ngx_stream_lua_access_handler(ngx_stream_session_t *s)
{
    ngx_int_t                   rc;
    ngx_stream_lua_ctx_t         *ctx;
    ngx_stream_lua_srv_conf_t    *lscf;
    ngx_stream_lua_main_conf_t   *lmcf;
    ngx_stream_phase_handler_t    tmp, *ph, *cur_ph, *last_ph;
    ngx_stream_core_main_conf_t  *cmcf;

    //ngx_log_debug2(NGX_LOG_DEBUG_STREAM, r->connection->log, 0,
    //               "lua access handler, uri:\"%V\" c:%ud", &r->uri,
    //               r->main->count);

    lmcf = ngx_stream_get_module_main_conf(s, ngx_stream_lua_module);

    if (!lmcf->postponed_to_access_phase_end) {

        lmcf->postponed_to_access_phase_end = 1;

        cmcf = ngx_stream_get_module_main_conf(r, ngx_stream_core_module);

        ph = cmcf->phase_engine.handlers;
        cur_ph = &ph[s->phase_handler];

        /* we should skip the post_access phase handler here too */
        last_ph = &ph[cur_ph->next - 1];

        dd("ph cur: %d, ph next: %d", (int) r->phase_handler,
           (int) (cur_ph->next - 2));

#if 0
        if (cur_ph == last_ph) {
            dd("XXX our handler is already the last access phase handler");
        }
#endif

        if (cur_ph < last_ph) {
            dd("swaping the contents of cur_ph and last_ph...");

            tmp = *cur_ph;

            memmove(cur_ph, cur_ph + 1,
                    (last_ph - cur_ph) * sizeof (ngx_stream_phase_handler_t));

            *last_ph = tmp;

            s->phase_handler--; /* redo the current ph */

            return NGX_DECLINED;
        }
    }

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    if (lscf->access_handler == NULL) {
        dd("no access handler found");
        return NGX_DECLINED;
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);

    dd("ctx = %p", ctx);

    if (ctx == NULL) {
        ctx = ngx_stream_lua_create_ctx(s);
        if (ctx == NULL) {
            return NGX_STREAM_INTERNAL_SERVER_ERROR;
        }
    }

    dd("entered? %d", (int) ctx->entered_access_phase);

    if (ctx->entered_access_phase) {
        dd("calling wev handler");
        rc = ctx->resume_handler(s);
        dd("wev handler returns %d", (int) rc);

        if (rc == NGX_ERROR || rc == NGX_DONE || rc > NGX_OK) {
            return rc;
        }

        if (rc == NGX_OK) {
            //if (r->header_sent) {
            //    dd("header already sent");

            //    /* response header was already generated in access_by_lua*,
            //     * so it is no longer safe to proceed to later phases
            //     * which may generate responses again */

            //    if (!ctx->eof) {
            //        dd("eof not yet sent");

            //        rc = ngx_stream_lua_send_chain_link(r, ctx, NULL
            //                                         /* indicate last_buf */);
            //        if (rc == NGX_ERROR || rc > NGX_OK) {
            //            return rc;
            //        }
            //    }

            //    return NGX_STREAM_OK;
            //}

            return NGX_OK;
        }

        return NGX_DECLINED;
    }

    if (ctx->waiting_more_body) {
        dd("WAITING MORE BODY");
        return NGX_DONE;
    }

#if 0
    if (lscf->force_read_body && !ctx->read_body_done) {
        r->request_body_in_single_buf = 1;
        r->request_body_in_persistent_file = 1;
        r->request_body_in_clean_file = 1;

        rc = ngx_stream_read_client_request_body(r,
                                       ngx_stream_lua_generic_phase_post_read);

        if (rc == NGX_ERROR || rc >= NGX_STREAM_SPECIAL_RESPONSE) {
#if (nginx_version < 1002006) ||                                             \
        (nginx_version >= 1003000 && nginx_version < 1003009)
            r->main->count--;
#endif

            return rc;
        }

        if (rc == NGX_AGAIN) {
            ctx->waiting_more_body = 1;
            return NGX_DONE;
        }
    }
#endif

    dd("calling access handler");
    return lscf->access_handler(r);
}


ngx_int_t
ngx_stream_lua_access_handler_inline(ngx_stream_session_t *s)
{
    ngx_int_t                  rc;
    lua_State                 *L;
    ngx_stream_lua_srv_conf_t   *lscf;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    L = ngx_stream_lua_get_lua_vm(s, NULL);

    /*  load Lua inline script (w/ cache) sp = 1 */
    rc = ngx_stream_lua_cache_loadbuffer(s->connection->log, L,
                                       lscf->access_src.data,
                                       lscf->access_src.len,
                                       lscf->access_src_key,
                                       (const char *) lscf->access_chunkname);

    if (rc != NGX_OK) {
        return NGX_STREAM_INTERNAL_SERVER_ERROR;
    }

    return ngx_stream_lua_access_by_chunk(L, s);
}


ngx_int_t
ngx_stream_lua_access_handler_file(ngx_stream_session_t *s)
{
    u_char                    *script_path;
    ngx_int_t                  rc;
    ngx_str_t                  eval_src;
    lua_State                 *L;
    ngx_stream_lua_srv_conf_t   *lscf;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    /* Eval nginx variables in code path string first */
    //if (ngx_stream_complex_value(s, &lscf->access_src, &eval_src) != NGX_OK) {
    //    return NGX_ERROR;
    //}
    eval_src = lscf->access_src;

    script_path = ngx_stream_lua_rebase_path(s->connection->pool, eval_src.data,
                                           eval_src.len);

    if (script_path == NULL) {
        return NGX_ERROR;
    }

    L = ngx_stream_lua_get_lua_vm(s, NULL);

    /*  load Lua script file (w/ cache)        sp = 1 */
    rc = ngx_stream_lua_cache_loadfile(s->connection->log, L, script_path,
                                     lscf->access_src_key);
    if (rc != NGX_OK) {
        if (rc < NGX_STREAM_SPECIAL_RESPONSE) {
            return NGX_STREAM_INTERNAL_SERVER_ERROR;
        }

        return rc;
    }

    /*  make sure we have a valid code chunk */
    ngx_stream_lua_assert(lua_isfunction(L, -1));

    return ngx_stream_lua_access_by_chunk(L, s);
}


static ngx_int_t
ngx_stream_lua_access_by_chunk(lua_State *L, ngx_stream_session_t *s)
{
    int                  co_ref;
    ngx_int_t            rc;
    lua_State           *co;
    ngx_event_t         *rev;
    ngx_connection_t    *c;
    ngx_stream_lua_ctx_t  *ctx;
    ngx_stream_cleanup_t  *cln;

    ngx_stream_lua_srv_conf_t     *lscf;

    /*  {{{ new coroutine to handle request */
    co = ngx_stream_lua_new_thread(s, L, &co_ref);

    if (co == NULL) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "lua: failed to create new coroutine "
                      "to handle request");

        return NGX_STREAM_INTERNAL_SERVER_ERROR;
    }

    /*  move code closure to new coroutine */
    lua_xmove(L, co, 1);

    /*  set closure's env table to new coroutine's globals table */
    ngx_stream_lua_get_globals_table(co);
    lua_setfenv(co, -2);

    /*  save nginx request in coroutine globals table */
    ngx_stream_lua_set_session(co, s);

    /*  {{{ initialize request context */
    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);

    dd("ctx = %p", ctx);

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_stream_lua_reset_ctx(s, L, ctx);

    ctx->entered_access_phase = 1;

    ctx->cur_co_ctx = &ctx->entry_co_ctx;
    ctx->cur_co_ctx->co = co;
    ctx->cur_co_ctx->co_ref = co_ref;
#ifdef NGX_LUA_USE_ASSERT
    ctx->cur_co_ctx->co_top = 1;
#endif

    /*  }}} */

    /*  {{{ register request cleanup hooks */
    if (ctx->cleanup == NULL) {
        cln = ngx_stream_cleanup_add(s, 0);
        if (cln == NULL) {
            return NGX_STREAM_INTERNAL_SERVER_ERROR;
        }

        cln->handler = ngx_stream_lua_session_cleanup_handler;
        cln->data = ctx;
        ctx->cleanup = cln;
    }
    /*  }}} */

    ctx->context = NGX_STREAM_LUA_CONTEXT_ACCESS;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    if (lscf->check_client_abort) {
        ctx->read_event_handler = ngx_stream_lua_rd_check_broken_connection;

        rev = s->connection->read;

        if (!rev->active) {
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) != NGX_OK) {
                return NGX_ERROR;
            }
        }

    } else {
        ctx->read_event_handler = ngx_stream_lua_block_reading;
    }

    rc = ngx_stream_lua_run_thread(L, s, ctx, 0);

    dd("returned %d", (int) rc);

    if (rc == NGX_ERROR || rc > NGX_OK) {
        return rc;
    }

    c = s->connection;

    if (rc == NGX_AGAIN) {
        rc = ngx_stream_lua_run_posted_threads(c, L, s, ctx);

        if (rc == NGX_ERROR || rc == NGX_DONE || rc > NGX_OK) {
            return rc;
        }

        if (rc != NGX_OK) {
            return NGX_DECLINED;
        }

    } else if (rc == NGX_DONE) {
        ngx_stream_lua_finalize_session(s, NGX_DONE);

        rc = ngx_stream_lua_run_posted_threads(c, L, s, ctx);

        if (rc == NGX_ERROR || rc == NGX_DONE || rc > NGX_OK) {
            return rc;
        }

        if (rc != NGX_OK) {
            return NGX_DECLINED;
        }
    }

#if 1
    if (rc == NGX_OK) {
        return NGX_OK;
    }
#endif

    return NGX_DECLINED;
}

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
