// add by chrono

/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef _NGX_STREAM_LUA_FILTERBY_H_INCLUDED_
#define _NGX_STREAM_LUA_FILTERBY_H_INCLUDED_


#include "ngx_stream_lua_common.h"

// nouse, for capture
//extern ngx_stream_filter_pt ngx_stream_lua_next_filter;


ngx_int_t ngx_stream_lua_filter_init(void);
ngx_int_t ngx_stream_lua_filter_by_chunk(lua_State *L,
    ngx_stream_session_t *s, ngx_chain_t *in, ngx_uint_t from_upstream);
ngx_int_t ngx_stream_lua_filter_inline(ngx_stream_session_t *s,
    ngx_chain_t *in, ngx_uint_t from_upstream);
ngx_int_t ngx_stream_lua_filter_file(ngx_stream_session_t *s,
    ngx_chain_t *in, ngx_uint_t from_upstream);
int ngx_stream_lua_filter_param_get(lua_State *L);
int ngx_stream_lua_filter_param_set(lua_State *L, ngx_stream_session_t *s,
    ngx_http_lua_ctx_t *ctx);


#endif /* _NGX_STREAM_LUA_FILTERBY_H_INCLUDED_ */

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
