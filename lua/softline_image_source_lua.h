/* Owner-thread custom sources. Uservalues retain the source and borrowed
 * event strings without registry roots, including across DATA backpressure. */
static const char *source_pin(lua_State *L, int table, int pins,
                              const char *name, size_t *length) {
  const char *text = NULL;
  size_t n = 0;
  lua_getfield(L, table, name);
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TSTRING);
    text = lua_tolstring(L, -1, &n);
    if (!length && strlen(text) != n)
      luaL_error(L, "source %s cannot contain NUL", name);
  }
  lua_setfield(L, pins, name);
  if (length)
    *length = n;
  return text;
}
static int source_frame(lua_State *L) {
  softline_lua_handle_t *h = lua_touserdata(L, 1);
  const char *name = h->source_call == 1   ? "next"
                     : h->source_call == 2 ? "prepare"
                                           : "close";
  int result, pins;
  lua_getiuservalue(L, 1, 5);
  lua_getfield(L, 2, name);
  lua_getfield(L, 2, "context");
  lua_call(L, 1, h->source_call == 1 ? 2 : h->source_call == 2 ? 1 : 0);
  if (h->source_call == 3)
    return 0;
  if (h->source_call == 1) {
    if (lua_isnil(L, -2) || (lua_isboolean(L, -2) && !lua_toboolean(L, -2))) {
      int status = 0;
      if (!lua_isnil(L, -1)) {
        const char *reason = luaL_checkstring(L, -1);
        (void)snprintf(h->source_error, sizeof(h->source_error), "%s", reason);
        status = -1;
      }
      lua_pushinteger(L, status);
      return 1;
    }
    result = lua_absindex(L, -2);
    luaL_checktype(L, result, LUA_TTABLE);
    h->source_event->operation = (sl_image_paste_operation_t)backend_enum(
        L, result, "operation", 0, SL_IMAGE_PASTE_END);
    h->source_event->offset = (size_t)backend_integer(L, result, "offset", 0);
    lua_createtable(L, 0, 3);
    pins = lua_gettop(L);
    h->source_event->media_type =
        source_pin(L, result, pins, "media_type", NULL);
    h->source_event->filename = source_pin(L, result, pins, "filename", NULL);
    h->source_event->data = (const unsigned char *)source_pin(
        L, result, pins, "data", &h->source_event->length);
    lua_setiuservalue(L, 1, 6);
    lua_pushinteger(L, 1);
    return 1;
  } else {
    lua_Integer timeout;
    size_t i, count;
    result = lua_gettop(L);
    luaL_checktype(L, result, LUA_TTABLE);
    lua_getfield(L, result, "timeout_ms");
    timeout = luaL_optinteger(L, -1, -1);
    lua_pop(L, 1);
    if (timeout < -1 || timeout > INT_MAX)
      luaL_error(L, "invalid source timeout_ms");
    h->source_wait->timeout_ms = (int)timeout;
    lua_getfield(L, result, "fds");
    if (!lua_isnil(L, -1)) {
      luaL_checktype(L, -1, LUA_TTABLE);
      count = lua_rawlen(L, -1);
      if (count > SL_IMAGE_PASTE_MAX_FDS)
        luaL_error(L, "too many source descriptors");
      for (i = 0; i < count; ++i) {
        lua_Integer fd, events;
        lua_rawgeti(L, -1, (lua_Integer)i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        fd = backend_integer(L, lua_gettop(L), "fd", 0);
        events = backend_integer(L, lua_gettop(L), "events", 0);
        if (fd > INT_MAX || (lua_Unsigned)events > UINT_MAX)
          luaL_error(L, "invalid source descriptor");
        h->source_wait->fds[i].fd = (int)fd;
        h->source_wait->fds[i].events = (unsigned int)events;
        lua_pop(L, 1);
      }
      h->source_wait->count = count;
    }
    lua_pushinteger(L, SL_OK);
    return 1;
  }
}
static int source_call(softline_lua_handle_t *h, int operation) {
  lua_State *L = h->L;
  int base = lua_gettop(L), status = operation == 1 ? -1 : SL_ERROR;
  h->source_call = operation;
  h->image_callback_active = 1;
  if (lua_checkstack(L, 8)) {
    lua_pushcfunction(L, source_frame);
    lua_pushvalue(L, 1);
    if (lua_pcall(L, 1, operation == 3 ? 0 : 1, 0) == LUA_OK) {
      status = operation == 3 ? SL_OK : (int)lua_tointeger(L, -1);
    } else {
      const char *error = lua_type(L, -1) == LUA_TSTRING
                              ? lua_tostring(L, -1)
                              : "image source callback failed";
      (void)snprintf(h->source_error, sizeof(h->source_error), "%s", error);
      (void)snprintf(h->image_error, sizeof(h->image_error), "%s", error);
      h->image_error_pending = 1;
    }
  }
  lua_settop(L, base);
  h->image_callback_active = 0;
  return status;
}
static int source_next(void *context, sl_image_paste_event_t *event) {
  softline_lua_handle_t *h = context;
  int result;
  h->source_event = event;
  result = source_call(h, 1);
  h->source_event = NULL;
  return result;
}
static int source_prepare(void *context, sl_image_paste_wait_t *wait) {
  softline_lua_handle_t *h = context;
  int result;
  h->source_wait = wait;
  result = source_call(h, 2);
  h->source_wait = NULL;
  return result;
}
static const char *source_error(void *context) {
  softline_lua_handle_t *h = context;
  return *h->source_error ? h->source_error : "image source failed";
}
static void source_close(void *context) {
  softline_lua_handle_t *h = context;
  lua_State *L = h->L;
  (void)source_call(h, 3);
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 5);
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 6);
}
static int softline_lua_image_start_source(lua_State *L) {
  softline_lua_handle_t *h = softline_lua_check(L, 1);
  static const char *const methods[] = {"next", "prepare", "close"};
  sl_image_paste_source_t source;
  sl_image_paste_id_t id = 0;
  size_t i;
  int status;
  luaL_checktype(L, 2, LUA_TTABLE);
  lua_getiuservalue(L, 1, 5);
  if (!lua_isnil(L, -1))
    return softline_lua_status(L, SL_ERROR_INVALID);
  lua_pop(L, 1);
  for (i = 0; i < 3; ++i) {
    lua_getfield(L, 2, methods[i]);
    luaL_checktype(L, -1, LUA_TFUNCTION);
    lua_pop(L, 1);
  }
  memset(&source, 0, sizeof(source));
  source.context = h;
  source.next = source_next;
  source.prepare = source_prepare;
  source.close = source_close;
  source.error = source_error;
  h->source_error[0] = 0;
  lua_pushvalue(L, 2);
  lua_setiuservalue(L, 1, 5);
  status = sl_image_paste_start_source(h->sl, &source, &id);
  if (status != SL_OK) {
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 5);
    return softline_lua_status(L, status);
  }
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}
