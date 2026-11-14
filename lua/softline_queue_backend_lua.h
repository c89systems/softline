/* Private Lua facade helpers; included after the handle/check/status helpers.
 */
static int softline_lua_history_status(lua_State *, softline_lua_handle_t *,
                                       int);
static const char *backend_string(lua_State *L, int table, const char *key,
                                  int optional) {
  size_t length;
  const char *value;
  table = lua_absindex(L, table);
  lua_getfield(L, table, key);
  if (optional && lua_isnil(L, -1))
    return NULL;
  if (lua_type(L, -1) != LUA_TSTRING)
    luaL_error(L, "%s must be a string", key);
  value = lua_tolstring(L, -1, &length);
  if (strlen(value) != length)
    luaL_error(L, "%s cannot contain NUL", key);
  /* Keep strings rooted until the native API has copied the borrowed view. */
  return value;
}

static lua_Integer backend_integer(lua_State *L, int table, const char *key,
                                   lua_Integer fallback) {
  lua_Integer value;
  lua_getfield(L, table, key);
  value = lua_isnil(L, -1) ? fallback : luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  if (value < 0 || (sizeof(unsigned long) < sizeof(lua_Integer) &&
                    value > (lua_Integer)ULONG_MAX))
    luaL_error(L, "%s must be a nonnegative native integer", key);
  return value;
}

static int backend_enum(lua_State *L, int table, const char *key, int fallback,
                        int maximum) {
  lua_Integer value = backend_integer(L, table, key, fallback);
  if (value > maximum)
    luaL_error(L, "invalid backend %s", key);
  return (int)value;
}

static int backend_boolean(lua_State *L, int table, const char *key) {
  int value;
  lua_getfield(L, table, key);
  if (!lua_isnil(L, -1) && !lua_isboolean(L, -1))
    luaL_error(L, "%s must be boolean", key);
  value = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return value;
}

static void backend_put_string(lua_State *L, const char *key,
                               const char *value) {
  if (value) {
    lua_pushstring(L, value);
    lua_setfield(L, -2, key);
  }
}
static void backend_put_integer(lua_State *L, const char *key,
                                lua_Integer value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, key);
}
static void backend_put_boolean(lua_State *L, const char *key, int value) {
  lua_pushboolean(L, value);
  lua_setfield(L, -2, key);
}
static void backend_push_request(lua_State *L,
                                 const sl_queue_request_t *request) {
  lua_createtable(L, 0, 10);
  backend_put_integer(L, "request_id", (lua_Integer)request->request_id);
  backend_put_integer(L, "operation", request->operation);
  backend_put_integer(L, "mode", request->mode);
  backend_put_integer(L, "target_request_id",
                      (lua_Integer)request->target_request_id);
  backend_put_boolean(L, "enabled", request->enabled);
  backend_put_string(L, "item_id", request->item_id);
  backend_put_string(L, "item_revision", request->item_revision);
  backend_put_string(L, "queue_revision", request->queue_revision);
  backend_put_string(L, "edit_token", request->edit_token);
  backend_put_string(L, "text", request->text);
}

/* Protect argument allocation and callback execution in the same Lua frame. */
static int backend_request_frame(lua_State *L) {
  const sl_queue_request_t *request = lua_touserdata(L, 2);
  lua_getiuservalue(L, 3, 2);
  lua_getfield(L, -1, "request");
  backend_push_request(L, request);
  lua_getfield(L, -3, "context");
  lua_call(L, 2, 1);
  return 1;
}
static int softline_lua_backend_request(void *context,
                                        const sl_queue_request_t *request) {
  softline_lua_handle_t *handle = context;
  lua_State *L = handle->L;
  int base = lua_gettop(L), status;
  handle->backend_callback_active = 1;
  if (!lua_checkstack(L, 5)) {
    handle->backend_callback_active = 0;
    return SL_ERROR_NOMEM;
  }
  lua_pushcfunction(L, backend_request_frame);
  lua_pushlightuserdata(L, handle);
  lua_pushlightuserdata(L, (void *)request);
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 3, 1, 0);
  status = status == LUA_OK && lua_isboolean(L, -1) && lua_toboolean(L, -1)
               ? SL_OK
               : SL_ERROR;
  lua_settop(L, base);
  handle->backend_callback_active = 0;
  return status;
}
static int backend_detach_frame(lua_State *L) {
  lua_getiuservalue(L, 2, 2);
  lua_getfield(L, -1, "detach");
  if (lua_isnil(L, -1))
    return 0;
  lua_getfield(L, -2, "context");
  lua_call(L, 1, 0);
  return 0;
}
static void softline_lua_backend_detach(void *context) {
  softline_lua_handle_t *handle = context;
  lua_State *L = handle->L;
  int base = lua_gettop(L);
  handle->backend_callback_active = 1;
  if (!lua_checkstack(L, 4)) {
    handle->backend_callback_active = 0;
    return;
  }
  lua_pushcfunction(L, backend_detach_frame);
  lua_pushlightuserdata(L, handle);
  lua_pushvalue(L, 1);
  (void)lua_pcall(L, 2, 0, 0);
  lua_settop(L, base);
  handle->backend_callback_active = 0;
}
static void backend_config(lua_State *L, softline_lua_handle_t *handle,
                           sl_config_t *config, int table, int context) {
  lua_Integer capabilities, pending;
  int receiver = lua_gettop(L), callbacks;
  if (!table) {
    if (context)
      luaL_error(L, "queue_backend_context needs queue_backend");
    return;
  }
  lua_newtable(L);
  callbacks = lua_gettop(L);
  lua_getfield(L, table, "request");
  luaL_checktype(L, -1, LUA_TFUNCTION);
  lua_setfield(L, callbacks, "request");
  lua_getfield(L, table, "detach");
  if (!lua_isnil(L, -1))
    luaL_checktype(L, -1, LUA_TFUNCTION);
  lua_setfield(L, callbacks, "detach");
  if (context)
    lua_pushvalue(L, context);
  else
    lua_pushnil(L);
  lua_setfield(L, callbacks, "context");
  /* A receiver-owned table is collectible even when callbacks/context refer
   * back to the receiver. Registry roots would leak this ordinary cycle. */
  lua_setiuservalue(L, receiver, 2);
  capabilities = backend_integer(L, table, "capabilities", 0);
  pending = backend_integer(L, table, "max_pending_requests", 0);
  if (capabilities < 0 || capabilities > 8191 || pending < 0 ||
      pending > INT_MAX)
    luaL_error(L, "invalid backend capabilities/request limit");
  handle->backend_callbacks.request = softline_lua_backend_request;
  handle->backend_callbacks.detach = softline_lua_backend_detach;
  handle->backend_callbacks.capabilities = (unsigned long)capabilities;
  handle->backend_callbacks.max_pending_requests = (size_t)pending;
  config->queue_backend = &handle->backend_callbacks;
  config->queue_backend_context = handle;
}

/** Lua editor:queue_backend_request(request): return request_id or nil, status.
 * The bridge receives a copied Lua table; true means handoff, not acceptance.
 * Typed BEGIN_EDIT retires on acceptance without requiring native readline;
 * END_EDIT/ABORT_EDIT still require the owned hold in the ordered mirror. */
static int softline_lua_queue_backend_request(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_queue_request_t request;
  sl_queue_request_id_t id;
  int status;
  luaL_checktype(L, 2, LUA_TTABLE);
  if (!lua_checkstack(L, 16))
    return luaL_error(L, "backend request stack allocation failed");
  memset(&request, 0, sizeof(request));
  request.operation = (sl_queue_operation_t)backend_enum(
      L, 2, "operation", SL_QUEUE_OFFER, SL_QUEUE_RESOLVE);
  request.mode = (sl_prompt_queue_mode_t)backend_enum(
      L, 2, "mode", SL_PROMPT_QUEUE_MODE_QUEUED, SL_PROMPT_QUEUE_MODE_STEER);
  request.enabled = backend_boolean(L, 2, "enabled");
  request.target_request_id =
      (sl_queue_request_id_t)backend_integer(L, 2, "target_request_id", 0);
  request.item_id = backend_string(L, 2, "item_id", 1);
  request.item_revision = backend_string(L, 2, "item_revision", 1);
  request.queue_revision = backend_string(L, 2, "queue_revision", 1);
  request.edit_token = backend_string(L, 2, "edit_token", 1);
  request.text = backend_string(L, 2, "text", 1);
  status = sl_queue_backend_request(handle->sl, &request, &id);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}
/** Lua editor:queue_backend_result(result): apply a deferred acknowledgement on
 * the owner coroutine. Never calls the outbound bridge or recreates an item.
 * Native dispatch later reconciles a missing hold before opening the editor and
 * keeps a newer ordinary draft editable while the recovered hold waits. */
static int softline_lua_queue_backend_result(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_queue_result_t result;
  lua_State *previous;
  int status;
  luaL_checktype(L, 2, LUA_TTABLE);
  if (!lua_checkstack(L, 12))
    return luaL_error(L, "backend result stack allocation failed");
  memset(&result, 0, sizeof(result));
  result.request_id =
      (sl_queue_request_id_t)backend_integer(L, 2, "request_id", 0);
  result.status = (sl_queue_result_status_t)backend_enum(
      L, 2, "status", SL_QUEUE_ACCEPTED, SL_QUEUE_UNKNOWN);
  result.item_id = backend_string(L, 2, "item_id", 1);
  result.item_revision = backend_string(L, 2, "item_revision", 1);
  result.edit_token = backend_string(L, 2, "edit_token", 1);
  result.reason = backend_string(L, 2, "reason", 1);
  if (!handle->history_callback_active)
    handle->history_callback_error[0] = '\0';
  previous = handle->history_caller;
  handle->history_caller = L;
  status = sl_queue_backend_result(handle->sl, &result);
  handle->history_caller = previous;
  return softline_lua_history_status(L, handle, status);
}
static void backend_read_entry(lua_State *L, int table,
                               sl_queue_entry_t *entry) {
  memset(entry, 0, sizeof(*entry));
  entry->mode = (sl_prompt_queue_mode_t)backend_enum(
      L, table, "mode", SL_PROMPT_QUEUE_MODE_QUEUED,
      SL_PROMPT_QUEUE_MODE_STEER);
  entry->held = backend_boolean(L, table, "held");
  entry->edit_held = backend_boolean(L, table, "edit_held");
  entry->ready = backend_boolean(L, table, "ready");
  entry->stopped = backend_boolean(L, table, "stopped");
  entry->action = backend_boolean(L, table, "action");
  entry->id = backend_string(L, table, "id", 0);
  entry->revision = backend_string(L, table, "revision", 0);
  entry->text = backend_string(L, table, "text", 0);
  entry->edit_token = backend_string(L, table, "edit_token", 1);
}
static void backend_push_entry(lua_State *L, const sl_queue_entry_t *entry) {
  lua_createtable(L, 0, 10);
  backend_put_string(L, "id", entry->id);
  backend_put_string(L, "revision", entry->revision);
  backend_put_string(L, "text", entry->text);
  backend_put_string(L, "edit_token", entry->edit_token);
  backend_put_integer(L, "mode", entry->mode);
  backend_put_boolean(L, "held", entry->held);
  backend_put_boolean(L, "edit_held", entry->edit_held);
  backend_put_boolean(L, "ready", entry->ready);
  backend_put_boolean(L, "stopped", entry->stopped);
  backend_put_boolean(L, "action", entry->action);
}
/** Lua editor:queue_backend_update(change): copy one ordered snapshot/delta.
 * Lua entry/index arguments are one-based. Disconnect preserves accepted view
 * and draft while marking outstanding mutation outcomes unknown. Reconnect
 * supplies a snapshot and resolves original IDs; it never reoffers work. */
static int softline_lua_queue_backend_update(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_queue_change_t change;
  sl_queue_entry_t *entries;
  size_t count, index;
  int list;
  luaL_checktype(L, 2, LUA_TTABLE);
  memset(&change, 0, sizeof(change));
  change.kind = (sl_queue_change_kind_t)backend_enum(
      L, 2, "kind", SL_QUEUE_SNAPSHOT, SL_QUEUE_DISCONNECTED);
  change.index = (size_t)backend_integer(L, 2, "index", 1) - 1;
  change.request_id =
      (sl_queue_request_id_t)backend_integer(L, 2, "request_id", 0);
  change.total_count = (size_t)backend_integer(L, 2, "total_count", 0);
  change.stopped = backend_boolean(L, 2, "stopped");
  change.auto_approval = backend_boolean(L, 2, "auto_approval");
  change.previous_revision = backend_string(L, 2, "previous_revision", 1);
  change.revision = backend_string(L, 2, "revision", 1);
  change.item_id = backend_string(L, 2, "item_id", 1);
  lua_getfield(L, 2, "entries");
  list = lua_gettop(L);
  if (!lua_isnil(L, list))
    luaL_checktype(L, list, LUA_TTABLE);
  count = lua_isnil(L, list) ? 0 : lua_rawlen(L, list);
  if (count > sl_prompt_queue_capacity(handle->sl) ||
      count > (size_t)(INT_MAX - 16) / 6)
    return softline_lua_status(L, SL_ERROR_FULL);
  if (!lua_checkstack(L, (int)(count * 6 + 16)))
    return luaL_error(L, "backend update stack allocation failed");
  entries = lua_newuserdatauv(L, (count + 1) * sizeof(*entries), 0);
  for (index = 0; index < count; ++index) {
    int table;
    lua_rawgeti(L, list, (lua_Integer)index + 1);
    table = lua_gettop(L);
    luaL_checktype(L, table, LUA_TTABLE);
    backend_read_entry(L, table, entries + index);
  }
  change.entries = entries;
  change.count = count;
  return softline_lua_status(L, sl_queue_backend_update(handle->sl, &change));
}
typedef struct {
  int kind;
  sl_queue_backend_info_t info;
  sl_queue_entry_t entry;
  sl_queue_pending_info_t pending;
} backend_view_t;

static int backend_view_frame(lua_State *L) {
  backend_view_t *view = lua_touserdata(L, 1);
  if (view->kind == 0) {
    lua_createtable(L, 0, 6);
    backend_put_boolean(L, "enabled", view->info.enabled);
    backend_put_boolean(L, "synchronized", view->info.synchronized);
    backend_put_string(L, "revision", view->info.revision);
    backend_put_integer(L, "visible_count",
                        (lua_Integer)view->info.visible_count);
    backend_put_integer(L, "total_count", (lua_Integer)view->info.total_count);
    backend_put_integer(L, "pending_count",
                        (lua_Integer)view->info.pending_count);
  } else if (view->kind == 1)
    backend_push_entry(L, &view->entry);
  else {
    backend_push_request(L, &view->pending.request);
    backend_put_boolean(L, "pending", view->pending.pending);
    backend_put_integer(L, "status", view->pending.status);
    backend_put_string(L, "reason", view->pending.reason);
  }
  return 1;
}

/* GC finalizers may mutate another live editor while Lua allocates a table.
 * Own the entire borrowed view before entering any allocating Lua operation. */
static int backend_view(lua_State *L, backend_view_t *view) {
  const char **fields[6];
  char *storage, *cursor;
  size_t count = 0, size = 0, index, length;
  int status;
  if (view->kind == 0)
    fields[count++] = &view->info.revision;
  else if (view->kind == 1) {
    fields[count++] = &view->entry.id;
    fields[count++] = &view->entry.revision;
    fields[count++] = &view->entry.text;
    fields[count++] = &view->entry.edit_token;
  } else {
    fields[count++] = &view->pending.request.item_id;
    fields[count++] = &view->pending.request.item_revision;
    fields[count++] = &view->pending.request.queue_revision;
    fields[count++] = &view->pending.request.edit_token;
    fields[count++] = &view->pending.request.text;
    fields[count++] = &view->pending.reason;
  }
  for (index = 0; index < count; ++index)
    if (*fields[index]) {
      length = strlen(*fields[index]) + 1;
      if (size > (size_t)-1 - length)
        return softline_lua_status(L, SL_ERROR_NOMEM);
      size += length;
    }
  storage = malloc(size ? size : 1);
  if (!storage)
    return softline_lua_status(L, SL_ERROR_NOMEM);
  cursor = storage;
  for (index = 0; index < count; ++index)
    if (*fields[index]) {
      length = strlen(*fields[index]) + 1;
      memcpy(cursor, *fields[index], length);
      *fields[index] = cursor;
      cursor += length;
    }
  if (!lua_checkstack(L, 3)) {
    free(storage);
    return luaL_error(L, "cannot allocate backend view stack");
  }
  lua_pushcfunction(L, backend_view_frame);
  lua_pushlightuserdata(L, view);
  status = lua_pcall(L, 1, 1, 0);
  free(storage);
  return status == LUA_OK ? 1 : lua_error(L);
}
static int softline_lua_queue_backend_info(lua_State *L) {
  backend_view_t view;
  int status = sl_queue_backend_info(softline_lua_check(L, 1)->sl, &view.info);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  view.kind = 0;
  return backend_view(L, &view);
}
static int softline_lua_queue_backend_entry(lua_State *L) {
  backend_view_t view;
  int status =
      sl_queue_backend_entry(softline_lua_check(L, 1)->sl,
                             (size_t)luaL_checkinteger(L, 2) - 1, &view.entry);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  view.kind = 1;
  return backend_view(L, &view);
}
static int softline_lua_queue_backend_pending(lua_State *L) {
  backend_view_t view;
  int status = sl_queue_backend_pending(softline_lua_check(L, 1)->sl,
                                        (size_t)luaL_checkinteger(L, 2) - 1,
                                        &view.pending);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  view.kind = 2;
  return backend_view(L, &view);
}
static int softline_lua_queue_backend_recover(lua_State *L) {
  return softline_lua_status(
      L,
      sl_queue_backend_recover(softline_lua_check(L, 1)->sl,
                               (sl_queue_request_id_t)luaL_checkinteger(L, 2)));
}
