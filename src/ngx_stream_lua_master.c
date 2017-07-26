// add by chrono

/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"

#include "ngx_stream_lua_master.h"

static int
ngx_stream_lua_ngx_master_pid(lua_State *L);

void
ngx_stream_lua_inject_master_api(lua_State *L)
{
    lua_createtable(L, 0 /* narr */, 4 /* nrec */);    /* ngx.master. */

    lua_pushcfunction(L, ngx_stream_lua_ngx_master_pid);
    lua_setfield(L, -2, "pid");

    lua_setfield(L, -2, "master");
}

static int
ngx_stream_lua_ngx_master_pid(lua_State *L)
{
// only enabled after we patch ngx_cycle.c
#ifdef NGX_STREAM_HAS_MASTER_PID
    extern ngx_pid_t ngx_master_pid;
    lua_pushinteger(L, (lua_Integer) ngx_master_pid);
#else
    lua_pushnil(L);
#endif

    return 1;
}

