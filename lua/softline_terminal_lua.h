/* Keep allocation, callback invocation and result validation in a protected
 * Lua frame, so errors cannot unwind past terminal restoration. */
typedef struct lua_terminal {
  lua_State *L;
  softline_lua_handle_t *handle;
  int ref, status;
  sl_terminal_event_t *event;
  char error[256];
} lua_terminal_t;
static int terminal_integer(lua_State *L, const char *key, int fallback,
                            int minimum) {
  lua_Integer value;
  lua_getfield(L, -1, key);
  value = lua_isnil(L, -1) ? fallback : luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  if (value < minimum || value > INT_MAX)
    luaL_error(L, "invalid terminal %s", key);
  return (int)value;
}
static int terminal_frame(lua_State *L) {
  lua_terminal_t *t = lua_touserdata(L, 1);
  sl_terminal_event_t *e = t->event;
  lua_rawgeti(L, LUA_REGISTRYINDEX, t->ref);
  lua_createtable(L, 0, 8);
  backend_put_integer(L, "operation", e->operation);
  backend_put_integer(L, "events", e->events);
  backend_put_integer(L, "rows", e->rows);
  backend_put_integer(L, "columns", e->columns);
  backend_put_integer(L, "length", (lua_Integer)e->length);
  if (e->operation == SL_TERMINAL_WRITE) {
    lua_pushlstring(L, e->data, e->length);
    lua_setfield(L, -2, "data");
  }
  lua_call(L, 1, 1);
  luaL_checktype(L, -1, LUA_TTABLE);
  t->status = terminal_integer(L, "status", SL_OK, INT_MIN);
  if (t->status != SL_OK)
    return 0;
  if (e->operation == SL_TERMINAL_STEP) {
    e->fd = terminal_integer(L, "fd", -1, -1);
    e->timeout_ms = terminal_integer(L, "timeout_ms", -1, -1);
    e->interests = (unsigned int)terminal_integer(L, "interests", 0, 0);
    lua_getfield(L, -1, "ready");
    if (!lua_isboolean(L, -1))
      luaL_error(L, "terminal ready must be boolean");
    e->ready = lua_toboolean(L, -1);
  } else if (e->operation == SL_TERMINAL_READ) {
    size_t length;
    const char *bytes;
    lua_getfield(L, -1, "data");
    luaL_checktype(L, -1, LUA_TSTRING);
    bytes = lua_tolstring(L, -1, &length);
    if (length > e->length)
      luaL_error(L, "terminal read exceeds capacity");
    memcpy(e->data, bytes, length);
    e->transferred = length;
  } else if (e->operation == SL_TERMINAL_WRITE)
    e->transferred = (size_t)terminal_integer(L, "transferred", 0, 0);
  return 0;
}
static int terminal_callback(void *context, sl_terminal_event_t *event) {
  lua_terminal_t *t = context;
  lua_State *L = t->L;
  int base = lua_gettop(L), status;
  if (!lua_checkstack(L, 5))
    return SL_ERROR_NOMEM;
  t->event = event;
  t->status = SL_ERROR;
  t->handle->terminal_callback_active = 1;
  lua_pushcfunction(L, terminal_frame);
  lua_pushlightuserdata(L, t);
  status = lua_pcall(L, 1, 0, 0);
  if (status != LUA_OK && !*t->error) {
    const char *message =
        lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
    snprintf(t->error, sizeof(t->error), "%s",
             message ? message : "terminal callback failed");
  }
  lua_settop(L, base);
  t->handle->terminal_callback_active = 0;
  return status == LUA_OK ? t->status : SL_ERROR;
}
static int softline_lua_terminal_handoff(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  lua_terminal_t t;
  int status;
  luaL_checktype(L, 2, LUA_TFUNCTION);
  memset(&t, 0, sizeof(t));
  t.L = L;
  t.handle = handle;
  lua_pushvalue(L, 2);
  t.ref = luaL_ref(L, LUA_REGISTRYINDEX);
  status = sl_terminal_handoff(handle->sl, terminal_callback, &t);
  luaL_unref(L, LUA_REGISTRYINDEX, t.ref);
  if (*t.error)
    return luaL_error(L, "%s", t.error);
  if (status < 0)
    return softline_lua_status(L, status);
  lua_pushboolean(L, 1);
  lua_pushboolean(L, status == SL_TERMINAL_ESCAPED);
  return 2;
}
static int softline_lua_terminal_local(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  const char *shell = luaL_optstring(L, 2, NULL);
  size_t length;
  int exit_status = -1;
  int status;
  if (shell) {
    (void)lua_tolstring(L, 2, &length);
    luaL_argcheck(L, strlen(shell) == length, 2, "shell cannot contain NUL");
  }
  status = sl_terminal_local(handle->sl, shell, &exit_status);
  if (status < 0)
    return softline_lua_status(L, status);
  lua_pushboolean(L, 1);
  lua_pushinteger(L, exit_status);
  lua_pushboolean(L, status == SL_TERMINAL_ESCAPED);
  return 3;
}
