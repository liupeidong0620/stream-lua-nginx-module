
/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_stream_lua_variable.h"
#include "ngx_stream_lua_util.h"


static int ngx_stream_lua_var_get(lua_State *L);
static int ngx_stream_lua_var_set(lua_State *L);

// disable inner var define
#if nginx_version < 1011002

static int ngx_stream_lua_variable_pid(lua_State *L);
static int ngx_stream_lua_variable_remote_addr(lua_State *L,
    ngx_stream_session_t *s);
static int ngx_stream_lua_variable_binary_remote_addr(lua_State *L,
    ngx_stream_session_t *s);
static int ngx_stream_lua_variable_remote_port(lua_State *L,
    ngx_stream_session_t *s);
static int ngx_stream_lua_variable_server_addr(lua_State *L,
    ngx_stream_session_t *s);
static int ngx_stream_lua_variable_server_port(lua_State *L,
    ngx_stream_session_t *s);
static int ngx_stream_lua_variable_connection(lua_State *L,
    ngx_stream_session_t *s);
static int ngx_stream_lua_variable_nginx_version(lua_State *L);

#endif


void
ngx_stream_lua_inject_variable_api(lua_State *L)
{
    /* {{{ register reference maps */
    lua_newtable(L);    /* ngx.var */

    lua_createtable(L, 0, 2 /* nrec */); /* metatable for .var */
    lua_pushcfunction(L, ngx_stream_lua_var_get);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ngx_stream_lua_var_set);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);

    lua_setfield(L, -2, "var");
}

#if nginx_version > 1011002

/**
 * Get nginx internal variables content
 *
 * @retval Always return a string or nil on Lua stack. Return nil when failed
 * to get content, and actual content string when found the specified variable.
 * @seealso ngx_stream_lua_var_set
 * */
static int
ngx_stream_lua_var_get(lua_State *L)
{
    ngx_stream_session_t        *s;
    u_char                      *p, *lowcase;
    size_t                       len;
    ngx_uint_t                   hash;
    ngx_str_t                    name;
    ngx_stream_variable_value_t   *vv;

#if (NGX_PCRE)
    u_char                      *val;
    ngx_uint_t                   n;
    LUA_NUMBER                   index;
    int                         *cap;
#endif

    s = ngx_stream_lua_get_session(L);
    if (s == NULL) {
        return luaL_error(L, "no session found");
    }

#if (NGX_PCRE)
    if (lua_type(L, -1) == LUA_TNUMBER) {
        /* it is a regex capturing variable */

        index = lua_tonumber(L, -1);

        if (index <= 0) {
            lua_pushnil(L);
            return 1;
        }

        n = (ngx_uint_t) index * 2;

        dd("n = %d, ncaptures = %d", (int) n, (int) s->ncaptures);

        if (s->captures == NULL
            || s->captures_data == NULL
            || n >= s->ncaptures)
        {
            lua_pushnil(L);
            return 1;
        }

        /* n >= 0 && n < s->ncaptures */

        cap = s->captures;

        p = s->captures_data;

        val = &p[cap[n]];

        lua_pushlstring(L, (const char *) val, (size_t) (cap[n + 1] - cap[n]));

        return 1;
    }
#endif

    if (lua_type(L, -1) != LUA_TSTRING) {
        return luaL_error(L, "bad variable name");
    }

    p = (u_char *) lua_tolstring(L, -1, &len);

    lowcase = lua_newuserdata(L, len);

    hash = ngx_hash_strlow(lowcase, p, len);

    name.len = len;
    name.data = lowcase;

    vv = ngx_stream_get_variable(s, &name, hash);

    if (vv == NULL || vv->not_found) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *) vv->data, (size_t) vv->len);
    return 1;
}

/**
 * Set nginx internal variable content
 *
 * @retval Always return a boolean on Lua stack. Return true when variable
 * content was modified successfully, false otherwise.
 * @seealso ngx_stream_lua_var_get
 * */
static int
ngx_stream_lua_var_set(lua_State *L)
{
    ngx_stream_variable_t         *v;
    ngx_stream_variable_value_t   *vv;
    ngx_stream_core_main_conf_t   *cmcf;
    u_char                      *p, *lowcase, *val;
    size_t                       len;
    ngx_str_t                    name;
    ngx_uint_t                   hash;
    ngx_stream_session_t          *s;
    int                          value_type;
    const char                  *msg;

    s = ngx_stream_lua_get_session(L);
    if (s == NULL) {
        return luaL_error(L, "no session object found");
    }

    /* we skip the first argument that is the table */

    /* we read the variable name */

    if (lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "bad variable name");
    }

    p = (u_char *) lua_tolstring(L, 2, &len);

    lowcase = lua_newuserdata(L, len + 1);

    hash = ngx_hash_strlow(lowcase, p, len);
    lowcase[len] = '\0';

    name.len = len;
    name.data = lowcase;

    /* we read the variable new value */

    value_type = lua_type(L, 3);
    switch (value_type) {
    case LUA_TNUMBER:
    case LUA_TSTRING:
        p = (u_char *) luaL_checklstring(L, 3, &len);

        val = ngx_palloc(s->connection->pool, len);
        if (val == NULL) {
            return luaL_error(L, "memory allocation error");
        }

        ngx_memcpy(val, p, len);

        break;

    case LUA_TNIL:
        /* undef the variable */

        val = NULL;
        len = 0;

        break;

    default:
        msg = lua_pushfstring(L, "string, number, or nil expected, "
                              "but got %s", lua_typename(L, value_type));
        return luaL_argerror(L, 1, msg);
    }

    /* we fetch the variable itself */

    cmcf = ngx_stream_get_module_main_conf(s, ngx_stream_core_module);

    v = ngx_hash_find(&cmcf->variables_hash, hash, name.data, name.len);

    if (v) {
        if (!(v->flags & NGX_STREAM_VAR_CHANGEABLE)) {
            return luaL_error(L, "variable \"%s\" not changeable", lowcase);
        }

        if (v->set_handler) {

            dd("set variables with set_handler");

            vv = ngx_palloc(s->connection->pool, sizeof(ngx_stream_variable_value_t));
            if (vv == NULL) {
                return luaL_error(L, "no memory");
            }

            if (value_type == LUA_TNIL) {
                vv->valid = 0;
                vv->not_found = 1;
                vv->no_cacheable = 0;
                vv->data = NULL;
                vv->len = 0;

            } else {
                vv->valid = 1;
                vv->not_found = 0;
                vv->no_cacheable = 0;

                vv->data = val;
                vv->len = len;
            }

            v->set_handler(s, vv, v->data);

            return 0;
        }

        if (v->flags & NGX_STREAM_VAR_INDEXED) {
            vv = &s->variables[v->index];

            dd("set indexed variable");

            if (value_type == LUA_TNIL) {
                vv->valid = 0;
                vv->not_found = 1;
                vv->no_cacheable = 0;

                vv->data = NULL;
                vv->len = 0;

            } else {
                vv->valid = 1;
                vv->not_found = 0;
                vv->no_cacheable = 0;

                vv->data = val;
                vv->len = len;
            }

            return 0;
        }

        return luaL_error(L, "variable \"%s\" cannot be assigned a value",
                          lowcase);
    }

    /* variable not found */

    return luaL_error(L, "variable \"%s\" not found for writing; "
                      "maybe it is a built-in variable that is not changeable "
                      "or you forgot to use \"set $%s '';\" "
                      "in the config file to define it first",
                      lowcase, lowcase);
}

#else   // nginx_version < 1011002

/* Get pseudo NGINX variables content
 *
 * @retval Always return a string or nil on Lua stack. Return nil when failed
 * to get content, and actual content string when found the specified variable.
 */
static int
ngx_stream_lua_var_get(lua_State *L)
{
    ngx_stream_session_t        *s;
    ngx_stream_lua_ctx_t        *ctx;
    u_char                      *p;
    size_t                       len;

    s = ngx_stream_lua_get_session(L);
    if (s == NULL) {
        return luaL_error(L, "no session found");
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no session ctx found");
    }

    if (lua_type(L, -1) != LUA_TSTRING) {
        return luaL_error(L, "bad variable name");
    }

    p = (u_char *) lua_tolstring(L, -1, &len);

    switch (len) {

    case sizeof("pid") - 1:
        if (ngx_strncmp(p, "pid", sizeof("pid") - 1) == 0) {
            return ngx_stream_lua_variable_pid(L);
        }
        break;

    case sizeof("connection") - 1:
        if (ngx_strncmp(p, "connection", sizeof("connection") - 1) == 0) {
            return ngx_stream_lua_variable_connection(L, s);
        }
        break;

    case sizeof("remote_addr") - 1:
        if (ngx_strncmp(p, "remote_addr", sizeof("remote_addr") - 1) == 0) {
            return ngx_stream_lua_variable_remote_addr(L, s);
        }

        if (ngx_strncmp(p, "remote_port", sizeof("remote_port") - 1) == 0) {
            return ngx_stream_lua_variable_remote_port(L, s);
        }

        if (ngx_strncmp(p, "server_addr", sizeof("server_addr") - 1) == 0) {
            return ngx_stream_lua_variable_server_addr(L, s);
        }

        if (ngx_strncmp(p, "server_port", sizeof("server_port") - 1) == 0) {
            return ngx_stream_lua_variable_server_port(L, s);
        }
        break;

    case sizeof("nginx_version") - 1:
        if (ngx_strncmp(p, "nginx_version", sizeof("nginx_version") - 1) == 0) {
            return ngx_stream_lua_variable_nginx_version(L);
        }
        break;

    case sizeof("binary_remote_addr") - 1:
        if (ngx_strncmp(p, "binary_remote_addr",
                       sizeof("binary_remote_addr") - 1) == 0)
        {
            return ngx_stream_lua_variable_binary_remote_addr(L, s);
        }
        break;

    default:
        break;
    }

    lua_pushnil(L);
    return 1;
}


static int
ngx_stream_lua_variable_pid(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer) ngx_pid);
    lua_tostring(L, -1);
    return 1;
}


static int
ngx_stream_lua_variable_remote_addr(lua_State *L, ngx_stream_session_t *s)
{
    lua_pushlstring(L, (const char *) s->connection->addr_text.data,
                    (size_t) s->connection->addr_text.len);
    return 1;
}


static int
ngx_stream_lua_variable_binary_remote_addr(lua_State *L,
    ngx_stream_session_t *s)
{
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    switch (s->connection->sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:
        sin6 = (struct sockaddr_in6 *) s->connection->sockaddr;

        lua_pushlstring(L, (const char *) sin6->sin6_addr.s6_addr,
                        sizeof(struct in6_addr));
        return 1;
#endif

    default: /* AF_INET */
        sin = (struct sockaddr_in *) s->connection->sockaddr;

        lua_pushlstring(L, (const char *) &sin->sin_addr, sizeof(in_addr_t));
        return 1;
    }
}


static int
ngx_stream_lua_variable_remote_port(lua_State *L, ngx_stream_session_t *s)
{
    ngx_uint_t            port;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    switch (s->connection->sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:
        sin6 = (struct sockaddr_in6 *) s->connection->sockaddr;
        port = ntohs(sin6->sin6_port);
        break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
    case AF_UNIX:
        port = 0;
        break;
#endif

    default: /* AF_INET */
        sin = (struct sockaddr_in *) s->connection->sockaddr;
        port = ntohs(sin->sin_port);
        break;
    }

    if (port > 0 && port < 65536) {
        lua_pushnumber(L, port);
        lua_tostring(L, -1);
        return 1;
    }

    lua_pushnil(L);
    return 1;
}


static int
ngx_stream_lua_variable_server_addr(lua_State *L, ngx_stream_session_t *s)
{
    ngx_str_t  str;
    u_char     addr[NGX_SOCKADDR_STRLEN];

    str.len = NGX_SOCKADDR_STRLEN;
    str.data = addr;

    if (ngx_connection_local_sockaddr(s->connection, &str, 0) != NGX_OK) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *) str.data, (size_t) str.len);
    return 1;
}


static int
ngx_stream_lua_variable_server_port(lua_State *L, ngx_stream_session_t *s)
{
    ngx_uint_t            port;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    if (ngx_connection_local_sockaddr(s->connection, NULL, 0) != NGX_OK) {
        lua_pushnil(L);
        return 1;
    }

    switch (s->connection->local_sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:
        sin6 = (struct sockaddr_in6 *) s->connection->local_sockaddr;
        port = ntohs(sin6->sin6_port);
        break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
    case AF_UNIX:
        port = 0;
        break;
#endif

    default: /* AF_INET */
        sin = (struct sockaddr_in *) s->connection->local_sockaddr;
        port = ntohs(sin->sin_port);
        break;
    }

    if (port > 0 && port < 65536) {
        lua_pushnumber(L, port);
        lua_tostring(L, -1);
        return 1;
    }

    lua_pushnil(L);
    return 1;
}


static int
ngx_stream_lua_variable_connection(lua_State *L,
    ngx_stream_session_t *s)
{
    lua_pushnumber(L, (lua_Integer) s->connection->number);
    lua_tostring(L, -1);

    return 1;
}


static int
ngx_stream_lua_variable_nginx_version(lua_State *L)
{
    lua_pushlstring(L, (const char *) NGINX_VERSION, sizeof(NGINX_VERSION) - 1);
    return 1;
}

/**
 * Can not set pseudo NGINX variables content
 * */
static int
ngx_stream_lua_var_set(lua_State *L)
{
    return luaL_error(L, "can not set variable");
}

#endif

