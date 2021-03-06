// add by chrono

/*
 * Copyright (C) Xiaozhe Wang (chaoslawful)
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef _NGX_STREAM_LUA_ACCESSBY_H_INCLUDED_
#define _NGX_STREAM_LUA_ACCESSBY_H_INCLUDED_


#include "ngx_stream_lua_common.h"


ngx_int_t ngx_stream_lua_access_handler(ngx_stream_session_t *s);
ngx_int_t ngx_stream_lua_access_handler_inline(ngx_stream_session_t *s);
ngx_int_t ngx_stream_lua_access_handler_file(ngx_stream_session_t *s);


#endif /* _NGX_STREAM_LUA_ACCESSBY_H_INCLUDED_ */

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
