#include "softline/softline.h"
#include <limits.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SOFTLINE_LUA_HANDLE "softline.handle"
#define SOFTLINE_LUA_MAX_KEY_BINDINGS 64
#define SOFTLINE_LUA_MAX_WATCHES 32
#define SOFTLINE_LUA_IDLE_ERROR_LEN 256

typedef struct softline_lua_handle {
  sl_t *sl;
  lua_State *L;
  struct {
    sl_key_t key;
    int ref;
  } key_bindings[SOFTLINE_LUA_MAX_KEY_BINDINGS];
  struct {
    sl_watch_id_t id;
    int ref;
    int file_ref;
  } watches[SOFTLINE_LUA_MAX_WATCHES];
  int history_callback_active;
  lua_State *history_caller;
  char history_callback_error[SOFTLINE_LUA_IDLE_ERROR_LEN];
  int idle_callback_ref;
  int policy_callback_ref;
  int completion_callback_active;
  sl_queue_backend_t backend_callbacks;
  int backend_callback_active;
  int terminal_callback_active;
  int policy_callback_active;
  int idle_callback_active;
  int idle_callback_failed;
  char idle_callback_error[SOFTLINE_LUA_IDLE_ERROR_LEN];
  int watch_callback_active;
  int watch_callback_failed;
  char watch_callback_error[SOFTLINE_LUA_IDLE_ERROR_LEN];
} softline_lua_handle_t;

typedef struct softline_lua_stream {
  lua_State *L;
  int ref;
  int kind;
  int index;
  int base_top;
} softline_lua_stream_t;

static softline_lua_handle_t *softline_lua_check(lua_State *L, int index) {
  softline_lua_handle_t *handle;
  handle =
      (softline_lua_handle_t *)luaL_checkudata(L, index, SOFTLINE_LUA_HANDLE);
  luaL_argcheck(L, handle != NULL && handle->sl != NULL, index,
                "closed softline handle");
  luaL_argcheck(L, !handle->backend_callback_active, index,
                "cannot reenter softline from a backend callback");
  luaL_argcheck(L, !handle->terminal_callback_active, index,
                "cannot reenter softline from a terminal callback");
  return handle;
}

static int softline_lua_status(lua_State *L, int status) {
  if (status == SL_OK) {
    lua_pushboolean(L, 1);
    return 1;
  }
  lua_pushnil(L);
  lua_pushinteger(L, status);
  return 2;
}

#include "softline_queue_backend_lua.h"
#include "softline_terminal_lua.h"

static int softline_lua_find_key_ref(softline_lua_handle_t *handle,
                                     sl_key_t key) {
  int i;
  for (i = 0; i < SOFTLINE_LUA_MAX_KEY_BINDINGS; i++) {
    if (handle->key_bindings[i].ref != LUA_NOREF &&
        handle->key_bindings[i].key == key)
      return i;
  }
  return -1;
}

static int softline_lua_find_empty_key_ref(softline_lua_handle_t *handle) {
  int i;
  for (i = 0; i < SOFTLINE_LUA_MAX_KEY_BINDINGS; i++) {
    if (handle->key_bindings[i].ref == LUA_NOREF)
      return i;
  }
  return -1;
}

static int softline_lua_find_watch_ref(softline_lua_handle_t *handle,
                                       sl_watch_id_t id) {
  int i;
  for (i = 0; i < SOFTLINE_LUA_MAX_WATCHES; i++) {
    if (handle->watches[i].ref != LUA_NOREF && handle->watches[i].id == id)
      return i;
  }
  return -1;
}

static int softline_lua_find_empty_watch_ref(softline_lua_handle_t *handle) {
  int i;
  for (i = 0; i < SOFTLINE_LUA_MAX_WATCHES; i++) {
    if (handle->watches[i].ref == LUA_NOREF)
      return i;
  }
  return -1;
}

static void softline_lua_release_watch_ref(lua_State *L,
                                           softline_lua_handle_t *handle,
                                           int slot) {
  if (!L || !handle || slot < 0 || slot >= SOFTLINE_LUA_MAX_WATCHES)
    return;
  if (handle->watches[slot].ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, handle->watches[slot].ref);
    handle->watches[slot].ref = LUA_NOREF;
  }
  if (handle->watches[slot].file_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, handle->watches[slot].file_ref);
    handle->watches[slot].file_ref = LUA_NOREF;
  }
  handle->watches[slot].id = 0;
}

static int softline_lua_key_callback(sl_t *sl, sl_key_t key, void *userdata,
                                     sl_key_action_t *action) {
  softline_lua_handle_t *handle;
  lua_State *L;
  int slot;
  int result;
  (void)sl;
  handle = (softline_lua_handle_t *)userdata;
  if (!handle || !handle->L || !action)
    return SL_ERROR_INVALID;
  L = handle->L;
  slot = softline_lua_find_key_ref(handle, key);
  if (slot < 0)
    return SL_ERROR_INVALID;
  lua_rawgeti(L, LUA_REGISTRYINDEX, handle->key_bindings[slot].ref);
  lua_pushinteger(L, key);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    lua_pop(L, 1);
    return SL_ERROR;
  }
  if (lua_isnil(L, -1)) {
    *action = SL_KEY_ACTION_HANDLED;
    lua_pop(L, 1);
    return SL_OK;
  }
  if (!lua_isinteger(L, -1)) {
    lua_pop(L, 1);
    return SL_ERROR_INVALID;
  }
  result = (int)lua_tointeger(L, -1);
  lua_pop(L, 1);
  switch (result) {
  case SL_KEY_ACTION_PASS:
  case SL_KEY_ACTION_HANDLED:
  case SL_KEY_ACTION_SUBMIT:
  case SL_KEY_ACTION_CANCEL:
  case SL_KEY_ACTION_INTERRUPT:
  case SL_KEY_ACTION_RESET:
  case SL_KEY_ACTION_SUSPEND:
    *action = (sl_key_action_t)result;
    return SL_OK;
  default:
    return SL_ERROR_INVALID;
  }
}

static void softline_lua_clear_idle_error(softline_lua_handle_t *handle) {
  if (!handle)
    return;
  handle->idle_callback_failed = 0;
  handle->idle_callback_error[0] = '\0';
}

static void softline_lua_clear_watch_error(softline_lua_handle_t *handle) {
  if (!handle)
    return;
  handle->watch_callback_failed = 0;
  handle->watch_callback_error[0] = '\0';
}

static void softline_lua_set_watch_error(softline_lua_handle_t *handle,
                                         const char *message) {
  if (!handle)
    return;
  (void)snprintf(handle->watch_callback_error,
                 sizeof(handle->watch_callback_error),
                 "watch callback failed: %s",
                 message && message[0] != '\0' ? message : "Lua error");
  handle->watch_callback_failed = 1;
}

static void softline_lua_set_idle_error(softline_lua_handle_t *handle,
                                        const char *message) {
  if (!handle)
    return;
  (void)snprintf(handle->idle_callback_error,
                 sizeof(handle->idle_callback_error),
                 "idle callback failed: %s",
                 message && message[0] != '\0' ? message : "Lua error");
  handle->idle_callback_failed = 1;
}

static void softline_lua_idle_callback(sl_t *sl, void *userdata) {
  softline_lua_handle_t *handle;
  lua_State *L;
  handle = (softline_lua_handle_t *)userdata;
  if (!handle || !handle->L || handle->idle_callback_ref == LUA_NOREF)
    return;
  L = handle->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, handle->idle_callback_ref);
  handle->idle_callback_active = 1;
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    softline_lua_set_idle_error(handle, lua_tostring(L, -1));
    lua_pop(L, 1);
    (void)sl_cancel(sl);
  }
  handle->idle_callback_active = 0;
}

static int softline_lua_watch_callback(sl_t *sl, const sl_watch_event_t *event,
                                       void *userdata) {
  softline_lua_handle_t *handle;
  lua_State *L;
  int slot;
  (void)sl;
  handle = (softline_lua_handle_t *)userdata;
  if (!handle || !handle->L || !event)
    return SL_ERROR_INVALID;
  slot = softline_lua_find_watch_ref(handle, event->id);
  if (slot < 0)
    return SL_ERROR_INVALID;
  L = handle->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, handle->watches[slot].ref);
  lua_pushinteger(L, (lua_Integer)event->id);
  lua_pushinteger(L, event->fd);
  lua_pushinteger(L, (lua_Integer)event->events);
  handle->watch_callback_active = 1;
  if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
    softline_lua_set_watch_error(handle, lua_tostring(L, -1));
    lua_pop(L, 1);
    handle->watch_callback_active = 0;
    return SL_ERROR;
  }
  handle->watch_callback_active = 0;
  return SL_OK;
}

/** Accepted configuration names mirror sl_config_t. Reject unknown fields so
 * removed layouts and misspelled options cannot silently use defaults. */
static const char *const softline_lua_config_fields[] = {
    "input_fd",
    "output_fd",
    "screen_width",
    "live_scroll_region",
    "disable_image_paste",
    "image_paste_path_template",
    "clear_prompt_on_exit",
    "prompt_queue",
    "prompt_queue_max_entries",
    "prompt_queue_preview_entries",
    "prompt_theme",
    "statusline",
    "statusline_start_element",
    "status_spinner",
    "status_busy",
    "status_idle_marker",
    "history_max_len",
    "line_max_len",
    "queue_backend",
    "queue_backend_context",
    "osc8_mode"};

static void softline_lua_config(lua_State *L, int index, sl_config_t *config,
                                int *rooted_template, int *backend,
                                int *backend_context) {
  const char *idle_marker;
  size_t idle_marker_len;
  if (lua_isnoneornil(L, index))
    return;
  luaL_checktype(L, index, LUA_TTABLE);
  index = lua_absindex(L, index);
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    size_t field, name_len;
    const char *name;
    if (lua_type(L, -2) != LUA_TSTRING)
      luaL_error(L, "configuration field names must be strings");
    name = lua_tolstring(L, -2, &name_len);
    for (field = 0; field < sizeof(softline_lua_config_fields) /
                                sizeof(softline_lua_config_fields[0]);
         field++) {
      if (name_len == strlen(softline_lua_config_fields[field]) &&
          memcmp(name, softline_lua_config_fields[field], name_len) == 0)
        break;
    }
    if (field == sizeof(softline_lua_config_fields) /
                     sizeof(softline_lua_config_fields[0]))
      luaL_error(L, "unknown configuration field '%s'", name);
    lua_pop(L, 1);
  }

  lua_getfield(L, index, "input_fd");
  if (!lua_isnil(L, -1))
    config->input_fd = (int)luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, index, "output_fd");
  if (!lua_isnil(L, -1))
    config->output_fd = (int)luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, index, "screen_width");
  if (!lua_isnil(L, -1))
    config->screen_width = (int)luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, index, "live_scroll_region");
  if (!lua_isnil(L, -1))
    config->live_scroll_region = lua_toboolean(L, -1);
  lua_pop(L, 1);
  /* Linux image paste uses private bundled c-ares: literal IPv4/IPv6,
   * /etc/hosts, then /etc/resolv.conf DNS within the paste deadline. Static
   * and shared builds need no libc NSS modules or extra resolver library. */
  lua_getfield(L, index, "disable_image_paste");
  if (!lua_isnil(L, -1))
    config->disable_image_paste = lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "image_paste_path_template");
  if (!lua_isnil(L, -1)) {
    size_t template_len;
    config->image_paste_path_template = luaL_checklstring(L, -1, &template_len);
    if (strlen(config->image_paste_path_template) != template_len)
      luaL_error(L, "image_paste_path_template cannot contain NUL bytes");
    /* Keep the Lua string on the stack until sl_create_with_config copies it.
     * Later configuration lookups may invoke __index and collect garbage. */
    *rooted_template = 1;
  } else
    lua_pop(L, 1);
  lua_getfield(L, index, "clear_prompt_on_exit");
  if (!lua_isnil(L, -1))
    config->clear_prompt_on_exit = lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "prompt_queue");
  if (!lua_isnil(L, -1))
    config->prompt_queue = lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "prompt_queue_max_entries");
  if (!lua_isnil(L, -1))
    config->prompt_queue_max_entries = (int)luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "prompt_queue_preview_entries");
  if (!lua_isnil(L, -1))
    config->prompt_queue_preview_entries = (int)luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "prompt_theme");
  if (!lua_isnil(L, -1))
    config->prompt_theme = (sl_prompt_theme_t)luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "osc8_mode");
  if (!lua_isnil(L, -1)) {
    lua_Integer mode = luaL_checkinteger(L, -1);
    luaL_argcheck(L, mode >= SL_OSC8_OFF && mode <= SL_OSC8_AUTO, index,
                  "osc8_mode must be OSC8_OFF, OSC8_ON or OSC8_AUTO");
    config->osc8_mode = (sl_osc8_mode_t)mode;
  }
  lua_pop(L, 1);
  lua_getfield(L, index, "statusline");
  if (!lua_isnil(L, -1))
    config->statusline = lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "statusline_start_element");
  if (!lua_isnil(L, -1))
    config->statusline_start_element = (size_t)luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "status_spinner");
  if (!lua_isnil(L, -1))
    config->status_spinner = lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "status_busy");
  if (!lua_isnil(L, -1))
    config->status_busy = lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "status_idle_marker");
  if (!lua_isnil(L, -1)) {
    idle_marker = luaL_checklstring(L, -1, &idle_marker_len);
    if (idle_marker_len != 1)
      luaL_error(L, "status_idle_marker must be one byte");
    config->status_idle_marker = idle_marker[0];
  }
  lua_pop(L, 1);

  lua_getfield(L, index, "history_max_len");
  if (!lua_isnil(L, -1))
    config->history_max_len = (int)luaL_checkinteger(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, index, "line_max_len");
  if (!lua_isnil(L, -1))
    config->line_max_len = (size_t)luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "queue_backend");
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TTABLE);
    *backend = lua_gettop(L);
  } else
    lua_pop(L, 1);
  lua_getfield(L, index, "queue_backend_context");
  if (!lua_isnil(L, -1))
    *backend_context = lua_gettop(L);
  else
    lua_pop(L, 1);
}

static int softline_lua_new(lua_State *L) {
  sl_config_t config;
  softline_lua_handle_t *handle;
  int i, rooted_template = 0, backend = 0, backend_context = 0;
  sl_config_init(&config);
  softline_lua_config(L, 1, &config, &rooted_template, &backend,
                      &backend_context);
  handle = (softline_lua_handle_t *)lua_newuserdatauv(L, sizeof(*handle), 3);
  memset(handle, 0, sizeof(*handle));
  handle->L = L;
  handle->history_callback_active = 0;
  handle->history_caller = NULL;
  handle->history_callback_error[0] = '\0';
  for (i = 0; i < SOFTLINE_LUA_MAX_KEY_BINDINGS; i++) {
    handle->key_bindings[i].key = SL_KEY_NONE;
    handle->key_bindings[i].ref = LUA_NOREF;
  }
  for (i = 0; i < SOFTLINE_LUA_MAX_WATCHES; i++) {
    handle->watches[i].id = 0;
    handle->watches[i].ref = LUA_NOREF;
    handle->watches[i].file_ref = LUA_NOREF;
  }
  handle->idle_callback_ref = LUA_NOREF;
  handle->policy_callback_ref = LUA_NOREF;
  handle->policy_callback_active = 0;
  handle->idle_callback_active = 0;
  handle->watch_callback_active = 0;
  softline_lua_clear_idle_error(handle);
  softline_lua_clear_watch_error(handle);
  luaL_getmetatable(L, SOFTLINE_LUA_HANDLE);
  lua_setmetatable(L, -2);
  backend_config(L, handle, &config, backend, backend_context);
  handle->sl = sl_create_with_config(&config);
  if (!handle->sl)
    return luaL_error(L, "failed to create softline handle");
  lua_pushvalue(L, -1);
  lua_replace(L, 1);
  lua_settop(L, 1);
  return 1;
}

static int softline_lua_gc(lua_State *L) {
  softline_lua_handle_t *handle;
  int i;
  handle = (softline_lua_handle_t *)luaL_checkudata(L, 1, SOFTLINE_LUA_HANDLE);
  if (handle->completion_callback_active || handle->backend_callback_active ||
      handle->policy_callback_active || handle->idle_callback_active ||
      handle->watch_callback_active || handle->history_callback_active ||
      handle->terminal_callback_active)
    return 0;
  for (i = 0; i < SOFTLINE_LUA_MAX_KEY_BINDINGS; i++) {
    if (handle->key_bindings[i].ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, handle->key_bindings[i].ref);
      handle->key_bindings[i].ref = LUA_NOREF;
    }
  }
  if (handle->idle_callback_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, handle->idle_callback_ref);
    handle->idle_callback_ref = LUA_NOREF;
  }
  if (handle->policy_callback_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, handle->policy_callback_ref);
    handle->policy_callback_ref = LUA_NOREF;
  }
  for (i = 0; i < SOFTLINE_LUA_MAX_WATCHES; i++) {
    softline_lua_release_watch_ref(L, handle, i);
  }
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 1);
  if (handle->sl) {
    handle->L = L;
    sl_destroy(handle->sl);
    handle->sl = NULL;
  }
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 2);
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 3);
  return 0;
}

static int softline_lua_close(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = (softline_lua_handle_t *)luaL_checkudata(L, 1, SOFTLINE_LUA_HANDLE);
  if (handle->completion_callback_active || handle->backend_callback_active ||
      handle->policy_callback_active || handle->idle_callback_active ||
      handle->watch_callback_active || handle->history_callback_active ||
      handle->terminal_callback_active)
    return luaL_error(L, "cannot close softline handle from its callback");
  return softline_lua_gc(L);
}

/** Lua editor:readline(prompt): return submitted input or nil, status.
 * Raw mode temporarily disables kernel tab expansion (TAB3/OXTABS) while
 * preserving OPOST/ONLCR; original terminal flags are restored on release. */
static int softline_lua_readline(lua_State *L) {
  softline_lua_handle_t *handle;
  lua_State *previous;
  const char *prompt;
  char *line;
  handle = softline_lua_check(L, 1);
  prompt = luaL_optstring(L, 2, NULL);
  softline_lua_clear_idle_error(handle);
  softline_lua_clear_watch_error(handle);
  handle->history_callback_error[0] = '\0';
  previous = handle->history_caller;
  handle->history_caller = L;
  line = sl_readline(handle->sl, prompt);
  handle->history_caller = previous;
  if (handle->idle_callback_failed || handle->watch_callback_failed) {
    if (line)
      sl_free_string(handle->sl, line);
    lua_pushnil(L);
    lua_pushinteger(L, SL_READLINE_ERROR);
    return 2;
  }
  if (!line) {
    lua_pushnil(L);
    lua_pushinteger(L, sl_last_readline_status(handle->sl));
    if (handle->history_callback_error[0]) {
      lua_pushstring(L, handle->history_callback_error);
      return 3;
    }
    return 2;
  }
  lua_pushstring(L, line);
  sl_free_string(handle->sl, line);
  return 1;
}

static int softline_lua_next_prompt(lua_State *L) {
  softline_lua_handle_t *handle;
  lua_State *previous;
  const char *prompt;
  char *line;
  sl_prompt_source_t source;
  handle = softline_lua_check(L, 1);
  prompt = luaL_optstring(L, 2, NULL);
  softline_lua_clear_idle_error(handle);
  softline_lua_clear_watch_error(handle);
  handle->history_callback_error[0] = '\0';
  source = SL_PROMPT_SOURCE_NONE;
  previous = handle->history_caller;
  handle->history_caller = L;
  line = sl_next_prompt(handle->sl, prompt, &source);
  handle->history_caller = previous;
  if (handle->idle_callback_failed || handle->watch_callback_failed) {
    if (line)
      sl_free_string(handle->sl, line);
    lua_pushnil(L);
    lua_pushinteger(L, SL_READLINE_ERROR);
    return 2;
  }
  if (!line) {
    lua_pushnil(L);
    lua_pushinteger(L, sl_last_readline_status(handle->sl));
    if (handle->history_callback_error[0]) {
      lua_pushstring(L, handle->history_callback_error);
      return 3;
    }
    return 2;
  }
  lua_pushstring(L, line);
  lua_pushinteger(L, source);
  lua_pushinteger(L, (lua_Integer)sl_last_prompt_id(handle->sl));
  sl_free_string(handle->sl, line);
  return 3;
}

static const char *softline_lua_history_string(lua_State *L, int index) {
  size_t len;
  const char *text = luaL_checklstring(L, index, &len);
  luaL_argcheck(L, strlen(text) == len, index,
                "history strings cannot contain NUL");
  return text;
}

typedef struct softline_lua_history_emit {
  sl_history_emit_t emit;
  void *context;
  int active;
} softline_lua_history_emit_t;

/* The emitter is rooted userdata, so a Lua backend retaining its closure gets
 * a controlled error after load rather than a dangling C stack pointer. */
static int softline_lua_history_emit(lua_State *L) {
  softline_lua_history_emit_t *state;
  const char *line;
  size_t len;
  state = (softline_lua_history_emit_t *)lua_touserdata(L, lua_upvalueindex(1));
  if (!state->active)
    return luaL_error(L, "history emitter is only valid during load");
  line = luaL_checklstring(L, 1, &len);
  if (strlen(line) != len)
    return softline_lua_status(L, state->emit(state->context, NULL));
  return softline_lua_status(L, state->emit(state->context, line));
}

static int softline_lua_history_call(softline_lua_handle_t *handle,
                                     const char *key, const char *prompt,
                                     sl_history_emit_t emit, void *context) {
  lua_State *L = handle->history_caller;
  softline_lua_history_emit_t *state;
  int top, status;
  if (!L)
    return SL_ERROR_INVALID;
  top = lua_gettop(L);
  state = NULL;
  if (emit) {
    state =
        (softline_lua_history_emit_t *)lua_newuserdatauv(L, sizeof(*state), 0);
    state->emit = emit;
    state->context = context;
    state->active = 1;
  }
  /* The synchronous caller keeps its receiver at index one. Use its current
   * coroutine, never the constructor's state (which may already be collected).
   * Hooks live in the receiver's uservalue, so captured handles are
   * collectable. Each entry point scopes/restores history_caller across nested
   * facade calls.
   */
  lua_getiuservalue(L, 1, 1);
  lua_getfield(L, -1, emit ? "load" : "append");
  lua_remove(L, -2);
  lua_pushstring(L, key);
  if (emit) {
    lua_pushvalue(L, top + 1);
    lua_pushcclosure(L, softline_lua_history_emit, 1);
  } else {
    lua_pushstring(L, prompt);
  }
  /* Automatic recording can invoke another hook within the same editor call.
   * Each invocation owns its diagnostic, including status-only failures. */
  handle->history_callback_error[0] = '\0';
  handle->history_callback_active = 1;
  status = SL_OK;
  if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
    const char *message = lua_tostring(L, -1);
    snprintf(handle->history_callback_error,
             sizeof(handle->history_callback_error), "%s",
             message ? message : "history callback failed");
    status = SL_ERROR;
  } else if (!lua_isboolean(L, -2) || !lua_toboolean(L, -2)) {
    status = lua_isinteger(L, -1) && lua_tointeger(L, -1) < 0
                 ? (int)lua_tointeger(L, -1)
                 : SL_ERROR;
  }
  handle->history_callback_active = 0;
  if (state) {
    state->active = 0;
    state->context = NULL;
  }
  lua_settop(L, top);
  return status;
}

static int softline_lua_history_load_hook(const char *key,
                                          sl_history_emit_t emit, void *context,
                                          void *userdata) {
  return softline_lua_history_call((softline_lua_handle_t *)userdata, key, NULL,
                                   emit, context);
}

static int softline_lua_history_append_hook(const char *key, const char *prompt,
                                            void *userdata) {
  return softline_lua_history_call((softline_lua_handle_t *)userdata, key,
                                   prompt, NULL, NULL);
}

static int softline_lua_history_status(lua_State *L,
                                       softline_lua_handle_t *handle,
                                       int status) {
  int results = softline_lua_status(L, status);
  if (status != SL_OK && handle->history_callback_error[0]) {
    lua_pushstring(L, handle->history_callback_error);
    /* Lua owns the returned message. It must not also become the diagnostic
     * for an enclosing editor's later cancellation, EOF or unrelated error. */
    handle->history_callback_error[0] = '\0';
    results++;
  }
  return results;
}

/** Lua history_set_backend(key, {load=function(key, emit), append=function(key,
 * prompt)}): callbacks return true or nil, negative status. Load may be absent.
 * Load is synchronous and transactional; append runs before history_add
 * returns in the calling coroutine. Callback exceptions become nil, status,
 * message, never escape C frames. Diagnostics belong to that hook invocation;
 * later status-only failures do not repeat previously handled exceptions. */
static int softline_lua_history_set_backend(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  const char *key;
  lua_State *previous;
  int has_load, status;
  key = softline_lua_history_string(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  lua_getfield(L, 3, "load");
  has_load = !lua_isnil(L, -1);
  if (has_load)
    luaL_checktype(L, -1, LUA_TFUNCTION);
  lua_pop(L, 1);
  lua_getfield(L, 3, "append");
  luaL_checktype(L, -1, LUA_TFUNCTION);
  lua_pop(L, 1);
  if (handle->history_callback_active)
    return softline_lua_status(L, SL_ERROR_INVALID);
  handle->history_callback_error[0] = '\0';
  lua_getiuservalue(L, 1,
                    1); /* Preserve the old hooks until attachment succeeds. */
  lua_newtable(L);
  lua_getfield(L, 3, "load");
  lua_setfield(L, -2, "load");
  lua_getfield(L, 3, "append");
  lua_setfield(L, -2, "append");
  lua_setiuservalue(L, 1, 1);
  previous = handle->history_caller;
  handle->history_caller = L;
  status = sl_history_set_backend(
      handle->sl, key, has_load ? softline_lua_history_load_hook : NULL,
      softline_lua_history_append_hook, handle);
  handle->history_caller = previous;
  if (status == SL_OK)
    lua_pop(L, 1);
  else
    lua_setiuservalue(L, 1, 1);
  return softline_lua_history_status(L, handle, status);
}

/** Lua history_open(key, directory=nil): XDG state keyed append store; no
 * auto-add unless explicitly enabled. Missing history is empty; torn tails
 * are discarded, private files are locked, and errors return nil, status. */
static int softline_lua_history_open(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  int status = sl_history_open(
      handle->sl, softline_lua_history_string(L, 2),
      lua_isnoneornil(L, 3) ? NULL : softline_lua_history_string(L, 3));
  if (status == SL_OK) {
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 1);
  }
  return softline_lua_status(L, status);
}

/** Lua history_close(): detach without an exit-time save; retained history
 * and editor auto-add policy survive. */
static int softline_lua_history_close(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  int status = sl_history_close(handle->sl);
  if (status == SL_OK) {
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 1);
  }
  return softline_lua_status(L, status);
}

/** Lua history_compact(): reread native shared storage under the key lock,
 * retain the newest capped entries, and atomically replace its file. */
static int softline_lua_history_compact(lua_State *L) {
  return softline_lua_status(L,
                             sl_history_compact(softline_lua_check(L, 1)->sl));
}

/** Lua history_set_auto_add(enabled): opt in to immediate recording of accepted
 * editor/queued drafts; delivery, promotion and Ctrl-R selection do not append.
 */
static int softline_lua_history_set_auto_add(lua_State *L) {
  return softline_lua_status(
      L, sl_history_set_auto_add(softline_lua_check(L, 1)->sl,
                                 lua_toboolean(L, 2)));
}

/** Lua history_add(prompt): immediate append on acceptance; empty/consecutive
 * duplicates are skipped and a failed append leaves recall unchanged. */
static int softline_lua_history_add(lua_State *L) {
  softline_lua_handle_t *handle;
  lua_State *previous;
  const char *prompt;
  int status;
  handle = softline_lua_check(L, 1);
  prompt = softline_lua_history_string(L, 2);
  if (!handle->history_callback_active)
    handle->history_callback_error[0] = '\0';
  previous = handle->history_caller;
  handle->history_caller = L;
  status = sl_history_add(handle->sl, prompt);
  handle->history_caller = previous;
  return softline_lua_history_status(L, handle, status);
}

/** Lua history_set_max_len(cap): bound recall; zero disables recording. */
static int softline_lua_history_set_max_len(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_history_set_max_len(handle->sl, (int)luaL_checkinteger(L, 2)));
}

/** Lua history_save(filename): overwrite an escaped snapshot, independently
 * of native locking. Do not overwrite an attached shared native store. */
static int softline_lua_history_save(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_history_save(handle->sl, luaL_checkstring(L, 2)));
}

/** Lua history_load(filename): import an escaped snapshot without append hooks.
 * LF, CR, Tab and backslashes round-trip; Unicode stays literal UTF-8. */
static int softline_lua_history_load(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_history_load(handle->sl, luaL_checkstring(L, 2)));
}

/** Lua editor:set_screen_width(width): set ordinary readline wrapping width.
 * With no active readline, this hint leaves native output geometry alone. */
static int softline_lua_set_screen_width(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_set_screen_width(handle->sl, (int)luaL_checkinteger(L, 2)));
}

/** Lua editor:set_live_scroll_region(enabled): opt ordinary readline into a
 * bottom scroll region. Ownership is checked at startup and between producer
 * callbacks; feed calls never request or wait for cursor replies. */
static int softline_lua_set_live_scroll_region(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_set_live_scroll_region(handle->sl, lua_toboolean(L, 2)));
}

/** Lua editor:set_image_paste_path_template(template): set the image path
 * template, or nil for the XDG cache default. `*` is replaced by a 20-character
 * xid, then `.png` or `.jpeg` is appended. Supports ~/ and HOME/XDG_CACHE_HOME
 * template tokens in uppercase or lowercase. Parent directories are created
 * recursively when an image is pasted. */
static int softline_lua_set_image_paste_path_template(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  const char *path_template = NULL;
  size_t length;
  if (!lua_isnoneornil(L, 2)) {
    path_template = luaL_checklstring(L, 2, &length);
    if (strlen(path_template) != length)
      return luaL_error(L,
                        "image paste path template cannot contain NUL bytes");
  }
  return softline_lua_status(
      L, sl_set_image_paste_path_template(handle->sl, path_template));
}

/** Lua editor:set_osc8_mode(mode): update policy between output streams. */
static int softline_lua_set_osc8_mode(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  lua_Integer mode = luaL_checkinteger(L, 2);
  if (mode < SL_OSC8_OFF || mode > SL_OSC8_AUTO)
    return softline_lua_status(L, SL_ERROR_INVALID);
  return softline_lua_status(
      L, sl_set_osc8_mode(handle->sl, (sl_osc8_mode_t)mode));
}

/** Lua editor:get_osc8_info(): cached mode, detected and enabled fields. */
static int softline_lua_get_osc8_info(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_osc8_info_t info;
  int result = sl_get_osc8_info(handle->sl, &info);
  if (result != SL_OK)
    return softline_lua_status(L, result);
  lua_newtable(L);
  lua_pushinteger(L, info.mode);
  lua_setfield(L, -2, "mode");
  lua_pushboolean(L, info.detected);
  lua_setfield(L, -2, "detected");
  lua_pushboolean(L, info.enabled);
  lua_setfield(L, -2, "enabled");
  return 1;
}

static int softline_lua_complete(sl_t *sl, char trigger,
                                 sl_completion_emit_t emit, void *context,
                                 void *userdata) {
  softline_lua_handle_t *handle = userdata;
  lua_State *L = handle->history_caller ? handle->history_caller : handle->L;
  int top = lua_gettop(L), status = SL_OK;
  size_t i, count;
  (void)sl;
  lua_getiuservalue(L, 1, 3);
  lua_pushlstring(L, &trigger, 1);
  lua_rawget(L, -2);
  lua_pushlstring(L, &trigger, 1);
  handle->completion_callback_active = 1;
  if (lua_pcall(L, 1, 1, 0) != LUA_OK)
    status = SL_ERROR;
  else if (!lua_istable(L, -1))
    status = SL_ERROR_INVALID;
  else {
    count = lua_rawlen(L, -1);
    for (i = 0; i < count; ++i) {
      const char *text;
      size_t length;
      lua_rawgeti(L, -1, (lua_Integer)i + 1);
      text =
          lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &length) : NULL;
      if (!text || strlen(text) != length)
        status = SL_ERROR_INVALID;
      else
        status = emit(text, context);
      lua_pop(L, 1);
      if (status != SL_OK)
        break;
    }
  }
  handle->completion_callback_active = 0;
  lua_settop(L, top);
  return status;
}

/** Lua editor:set_completion_providers(registrations): trigger/scope/strip
 * settings and a complete(trigger) callback returning full candidate strings.
 */
static int softline_lua_set_completion_providers(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_completion_provider_t *items;
  size_t count = 0, i;
  int callbacks, status;
  if (handle->completion_callback_active)
    return softline_lua_status(L, SL_ERROR_NOT_READY);
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TTABLE);
    count = lua_rawlen(L, 2);
  }
  luaL_argcheck(L, count <= (size_t)-1 / sizeof(*items), 2,
                "too many completion providers");
  items = lua_newuserdatauv(L, count * sizeof(*items), 0);
  memset(items, 0, count * sizeof(*items));
  lua_newtable(L);
  callbacks = lua_gettop(L);
  for (i = 0; i < count; ++i) {
    const char *trigger;
    size_t length;
    lua_rawgeti(L, 2, (lua_Integer)i + 1);
    luaL_checktype(L, -1, LUA_TTABLE);
    lua_getfield(L, -1, "trigger");
    trigger = luaL_checklstring(L, -1, &length);
    luaL_argcheck(L, length == 1, 2, "trigger must be one ASCII character");
    items[i].trigger = trigger[0];
    lua_pop(L, 1);
    lua_getfield(L, -1, "scope");
    if (!lua_isnil(L, -1)) {
      lua_Integer scope = luaL_checkinteger(L, -1);
      luaL_argcheck(L,
                    scope == SL_COMPLETION_START || scope == SL_COMPLETION_WORD,
                    2, "invalid completion scope");
      items[i].scope = (sl_completion_scope_t)scope;
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "strip_trigger");
    if (!lua_isnil(L, -1))
      luaL_checktype(L, -1, LUA_TBOOLEAN);
    items[i].strip_trigger = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_pushlstring(L, &items[i].trigger, 1);
    lua_getfield(L, -2, "complete");
    luaL_checktype(L, -1, LUA_TFUNCTION);
    lua_rawset(L, callbacks);
    items[i].callback = softline_lua_complete;
    items[i].userdata = handle;
    lua_pop(L, 1);
  }
  status = sl_set_completion_providers(handle->sl, items, count);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushvalue(L, callbacks);
  lua_setiuservalue(L, 1, 3);
  return softline_lua_status(L, SL_OK);
}

static int softline_lua_set_prompt_queue(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_set_prompt_queue(handle->sl, lua_toboolean(L, 2),
                             (int)luaL_checkinteger(L, 3),
                             (int)luaL_checkinteger(L, 4)));
}

static int softline_lua_queue_index(lua_State *L, int argument, size_t *index) {
  lua_Integer value;
  value = luaL_checkinteger(L, argument);
  if (value < 1 || (lua_Unsigned)(value - 1) > (lua_Unsigned)SIZE_MAX)
    return 0;
  *index = (size_t)(value - 1);
  return 1;
}

static int softline_lua_queue_count(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  lua_pushinteger(L, (lua_Integer)sl_prompt_queue_count(handle->sl));
  return 1;
}

static int softline_lua_queue_capacity(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  lua_pushinteger(L, (lua_Integer)sl_prompt_queue_capacity(handle->sl));
  return 1;
}

static int softline_lua_queue_peek(lua_State *L) {
  softline_lua_handle_t *handle;
  char *text;
  size_t index;
  int status;
  handle = softline_lua_check(L, 1);
  if (!softline_lua_queue_index(L, 2, &index))
    return softline_lua_status(L, SL_ERROR_INVALID);
  text = NULL;
  status = sl_prompt_queue_peek(handle->sl, index, &text);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushstring(L, text);
  sl_free_string(handle->sl, text);
  return 1;
}

static int softline_lua_queue_insert(lua_State *L) {
  softline_lua_handle_t *handle;
  size_t index;
  sl_prompt_id_t id;
  int status, held = 0;
  handle = softline_lua_check(L, 1);
  if (!softline_lua_queue_index(L, 2, &index))
    return softline_lua_status(L, SL_ERROR_INVALID);
  if (!lua_isnoneornil(L, 4)) {
    luaL_checktype(L, 4, LUA_TBOOLEAN);
    held = lua_toboolean(L, 4);
  }
  status = sl_prompt_queue_insert(handle->sl, index, luaL_checkstring(L, 3),
                                  held, &id);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}

static int softline_lua_queue_append(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_prompt_id_t id;
  int status, held = 0;
  handle = softline_lua_check(L, 1);
  if (!lua_isnoneornil(L, 3)) {
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    held = lua_toboolean(L, 3);
  }
  status =
      sl_prompt_queue_append(handle->sl, luaL_checkstring(L, 2), held, &id);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}

static int softline_lua_inject(lua_State *L, int steer) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_prompt_id_t id;
  int status =
      steer ? sl_inject_steer_prompt(handle->sl, luaL_checkstring(L, 2), &id)
            : sl_inject_queued_prompt(handle->sl, luaL_checkstring(L, 2), &id);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}

static int softline_lua_inject_queued_prompt(lua_State *L) {
  return softline_lua_inject(L, 0);
}

static int softline_lua_inject_steer_prompt(lua_State *L) {
  return softline_lua_inject(L, 1);
}

static int softline_lua_set_auto_approval(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  luaL_checktype(L, 2, LUA_TBOOLEAN);
  return softline_lua_status(
      L, sl_set_auto_approval(handle->sl, lua_toboolean(L, 2)));
}

static int softline_lua_get_auto_approval(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  int enabled;
  int status = sl_get_auto_approval(handle->sl, &enabled);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushboolean(L, enabled);
  return 1;
}

static void
softline_lua_queue_push_info(lua_State *L,
                             const sl_prompt_queue_entry_info_t *info) {
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, (lua_Integer)info->id);
  lua_setfield(L, -2, "id");
  lua_pushinteger(L, info->mode);
  lua_setfield(L, -2, "mode");
  lua_pushboolean(L, info->held);
  lua_setfield(L, -2, "held");
  lua_pushboolean(L, info->stopped);
  lua_setfield(L, -2, "stopped");
  lua_pushboolean(L, info->eligible);
  lua_setfield(L, -2, "eligible");
  lua_newtable(L);
  lua_pushboolean(L, info->policy.enqueue);
  lua_setfield(L, -2, "enqueue");
  lua_pushboolean(L, info->policy.independent);
  lua_setfield(L, -2, "independent");
  lua_pushboolean(L, info->policy.executable);
  lua_setfield(L, -2, "executable");
  lua_setfield(L, -2, "policy");
}

static int softline_lua_policy_field(lua_State *L, const char *field,
                                     int *value) {
  int valid;
  lua_pushstring(L, field);
  lua_rawget(L, -2);
  valid = lua_isnil(L, -1) || lua_isboolean(L, -1);
  if (lua_isboolean(L, -1))
    *value = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return valid;
}

static int softline_lua_policy_callback(sl_t *sl, const char *text,
                                        sl_prompt_queue_mode_t mode,
                                        sl_prompt_queue_policy_t *policy,
                                        void *userdata) {
  softline_lua_handle_t *handle = userdata;
  lua_State *L = handle->history_caller ? handle->history_caller : handle->L;
  int valid;
  sl_prompt_queue_entry_info_t info;
  (void)sl;
  memset(&info, 0, sizeof(info));
  info.policy = *policy;
  lua_rawgeti(L, LUA_REGISTRYINDEX, handle->policy_callback_ref);
  lua_pushstring(L, text);
  lua_pushinteger(L, mode);
  softline_lua_queue_push_info(L, &info);
  lua_getfield(L, -1, "policy");
  lua_remove(L, -2);
  handle->policy_callback_active = 1;
  valid = lua_pcall(L, 3, 1, 0) == LUA_OK;
  handle->policy_callback_active = 0;
  if (valid && !lua_isnil(L, -1)) {
    valid = lua_istable(L, -1) &&
            softline_lua_policy_field(L, "enqueue", &policy->enqueue) &&
            softline_lua_policy_field(L, "independent", &policy->independent) &&
            softline_lua_policy_field(L, "executable", &policy->executable);
  }
  lua_pop(L, 1);
  return valid ? SL_OK : SL_ERROR_INVALID;
}

static int softline_lua_set_queue_policy(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  int ref = LUA_NOREF, status;
  if (handle->policy_callback_active)
    return softline_lua_status(L, SL_ERROR_INVALID);
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  status = sl_set_prompt_queue_policy(
      handle->sl, ref == LUA_NOREF ? NULL : softline_lua_policy_callback,
      handle);
  if (status != SL_OK) {
    if (ref != LUA_NOREF)
      luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return softline_lua_status(L, status);
  }
  if (handle->policy_callback_ref != LUA_NOREF)
    luaL_unref(L, LUA_REGISTRYINDEX, handle->policy_callback_ref);
  handle->policy_callback_ref = ref;
  return softline_lua_status(L, status);
}

static int softline_lua_queue_identity(lua_State *L, sl_prompt_id_t *id) {
  lua_Integer value = luaL_checkinteger(L, 2);
  if (value <= 0 || (lua_Unsigned)value > (lua_Unsigned)LONG_MAX)
    return 0;
  *id = (sl_prompt_id_t)value;
  return 1;
}

static int softline_lua_queue_set_executable(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_prompt_id_t id;
  if (!softline_lua_queue_identity(L, &id))
    return softline_lua_status(L, SL_ERROR_INVALID);
  luaL_checktype(L, 3, LUA_TBOOLEAN);
  return softline_lua_status(
      L, sl_prompt_queue_set_executable(handle->sl, id, lua_toboolean(L, 3)));
}

static int softline_lua_queue_stopped(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  int stopped, status = sl_get_prompt_queue_stopped(handle->sl, &stopped);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushboolean(L, stopped);
  return 1;
}

static int softline_lua_queue_resume(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  lua_State *previous = handle->L;
  int status;
  handle->L = L;
  status = sl_prompt_queue_resume(handle->sl);
  handle->L = previous;
  return softline_lua_status(L, status);
}

static int softline_lua_queue_info(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_prompt_queue_entry_info_t info;
  size_t index;
  int status;
  if (!softline_lua_queue_index(L, 2, &index))
    return softline_lua_status(L, SL_ERROR_INVALID);
  status = sl_prompt_queue_info(handle->sl, index, &info);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  softline_lua_queue_push_info(L, &info);
  return 1;
}

static int softline_lua_queue_draft_info(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_prompt_queue_entry_info_t info;
  int status = sl_prompt_queue_draft_info(handle->sl, &info);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  if (!info.id)
    lua_pushnil(L);
  else
    softline_lua_queue_push_info(L, &info);
  return 1;
}

static int softline_lua_queue_approve(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  lua_State *previous = handle->L;
  int status;
  sl_prompt_id_t id;
  if (!softline_lua_queue_identity(L, &id))
    return softline_lua_status(L, SL_ERROR_INVALID);
  handle->L = L;
  status = sl_prompt_queue_approve(handle->sl, id);
  handle->L = previous;
  return softline_lua_status(L, status);
}

static int softline_lua_queue_reject(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  lua_State *previous = handle->L;
  int status;
  sl_prompt_id_t id;
  if (!softline_lua_queue_identity(L, &id))
    return softline_lua_status(L, SL_ERROR_INVALID);
  handle->L = L;
  status = sl_prompt_queue_reject(handle->sl, id);
  handle->L = previous;
  return softline_lua_status(L, status);
}

static int softline_lua_last_prompt_id(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  lua_pushinteger(L, (lua_Integer)sl_last_prompt_id(handle->sl));
  return 1;
}

static int softline_lua_queue_replace(lua_State *L) {
  softline_lua_handle_t *handle;
  size_t index;
  handle = softline_lua_check(L, 1);
  if (!softline_lua_queue_index(L, 2, &index))
    return softline_lua_status(L, SL_ERROR_INVALID);
  return softline_lua_status(
      L, sl_prompt_queue_replace(handle->sl, index, luaL_checkstring(L, 3)));
}

static int softline_lua_queue_take(lua_State *L) {
  softline_lua_handle_t *handle;
  char *text;
  size_t index;
  int status;
  handle = softline_lua_check(L, 1);
  if (!softline_lua_queue_index(L, 2, &index))
    return softline_lua_status(L, SL_ERROR_INVALID);
  text = NULL;
  status = sl_prompt_queue_take(handle->sl, index, &text);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushstring(L, text);
  lua_pushinteger(L, (lua_Integer)sl_last_prompt_id(handle->sl));
  sl_free_string(handle->sl, text);
  return 2;
}

static int softline_lua_queue_mode(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_prompt_queue_mode_t mode;
  size_t index;
  int status;
  handle = softline_lua_check(L, 1);
  if (!softline_lua_queue_index(L, 2, &index))
    return softline_lua_status(L, SL_ERROR_INVALID);
  status = sl_prompt_queue_get_mode(handle->sl, index, &mode);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushinteger(L, (lua_Integer)mode);
  return 1;
}

static int softline_lua_queue_set_mode(lua_State *L) {
  softline_lua_handle_t *handle;
  size_t index;
  handle = softline_lua_check(L, 1);
  if (!softline_lua_queue_index(L, 2, &index))
    return softline_lua_status(L, SL_ERROR_INVALID);
  return softline_lua_status(
      L,
      sl_prompt_queue_set_mode(
          handle->sl, index, (sl_prompt_queue_mode_t)luaL_checkinteger(L, 3)));
}

static int softline_lua_queue_clear(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(L, sl_prompt_queue_clear(handle->sl));
}

static int softline_lua_queue_draft(lua_State *L) {
  softline_lua_handle_t *handle;
  lua_State *previous, *previous_backend;
  int status;
  handle = softline_lua_check(L, 1);
  if (!handle->history_callback_active)
    handle->history_callback_error[0] = '\0';
  previous = handle->history_caller;
  handle->history_caller = L;
  previous_backend = handle->L;
  handle->L = L;
  status = sl_prompt_queue_enqueue_draft(handle->sl);
  handle->L = previous_backend;
  handle->history_caller = previous;
  return softline_lua_history_status(L, handle, status);
}

static int softline_lua_set_queue_delivery(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_prompt_queue_delivery_t delivery;
  const char *name;
  handle = softline_lua_check(L, 1);
  name = luaL_checkstring(L, 2);
  if (strcmp(name, "auto") == 0)
    delivery = SL_PROMPT_QUEUE_DELIVERY_AUTO;
  else if (strcmp(name, "manual") == 0)
    delivery = SL_PROMPT_QUEUE_DELIVERY_MANUAL;
  else
    return softline_lua_status(L, SL_ERROR_INVALID);
  return softline_lua_status(
      L, sl_set_prompt_queue_delivery(handle->sl, delivery));
}

static int softline_lua_queue_delivery(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_prompt_queue_delivery_t delivery;
  int status;
  handle = softline_lua_check(L, 1);
  status = sl_get_prompt_queue_delivery(handle->sl, &delivery);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushstring(L, delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL ? "manual"
                                                                : "auto");
  return 1;
}

static int softline_lua_set_queue_profile(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_prompt_queue_profile_t profile;
  const char *name;
  handle = softline_lua_check(L, 1);
  name = luaL_checkstring(L, 2);
  if (strcmp(name, "default") == 0)
    profile = SL_PROMPT_QUEUE_PROFILE_DEFAULT;
  else if (strcmp(name, "queued_turns") == 0)
    profile = SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS;
  else
    return softline_lua_status(L, SL_ERROR_INVALID);
  return softline_lua_status(L,
                             sl_set_prompt_queue_profile(handle->sl, profile));
}

static int softline_lua_queue_profile(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_prompt_queue_profile_t profile;
  int status;
  handle = softline_lua_check(L, 1);
  status = sl_get_prompt_queue_profile(handle->sl, &profile);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_pushstring(L, profile == SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS
                        ? "queued_turns"
                        : "default");
  return 1;
}

static int softline_lua_queue_key_field(lua_State *L, const char *name,
                                        sl_key_t *key) {
  lua_getfield(L, 2, name);
  if (lua_isnil(L, -1))
    *key = SL_KEY_NONE;
  else if (lua_isinteger(L, -1))
    *key = (sl_key_t)lua_tointeger(L, -1);
  else {
    lua_pop(L, 1);
    return 0;
  }
  lua_pop(L, 1);
  return 1;
}

static int softline_lua_set_queue_keys(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_prompt_queue_keys_t keys;
  luaL_checktype(L, 2, LUA_TTABLE);
  handle = softline_lua_check(L, 1);
  if (!softline_lua_queue_key_field(L, "enqueue_draft", &keys.enqueue_draft) ||
      !softline_lua_queue_key_field(L, "edit_newest", &keys.edit_newest) ||
      !softline_lua_queue_key_field(L, "submit_or_promote_newest",
                                    &keys.submit_or_promote_newest))
    return luaL_argerror(L, 2, "queue keys must be integers or nil");
  return softline_lua_status(L, sl_set_prompt_queue_keys(handle->sl, &keys));
}

static int softline_lua_queue_keys(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_prompt_queue_keys_t keys;
  int status;
  handle = softline_lua_check(L, 1);
  status = sl_get_prompt_queue_keys(handle->sl, &keys);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  lua_newtable(L);
  lua_pushinteger(L, keys.enqueue_draft);
  lua_setfield(L, -2, "enqueue_draft");
  lua_pushinteger(L, keys.edit_newest);
  lua_setfield(L, -2, "edit_newest");
  lua_pushinteger(L, keys.submit_or_promote_newest);
  lua_setfield(L, -2, "submit_or_promote_newest");
  return 1;
}

static int softline_lua_set_prompt_theme(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_set_prompt_theme(handle->sl,
                             (sl_prompt_theme_t)luaL_checkinteger(L, 2)));
}

/** Lua editor:set_quoted_prompt_prefix(prefix): set the prefix repeated on
 * every wrapped quote row. nil restores the default "> ". */
static int softline_lua_set_quoted_prompt_prefix(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  const char *prefix = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
  return softline_lua_status(L,
                             sl_set_quoted_prompt_prefix(handle->sl, prefix));
}

static void softline_lua_quote_color(lua_State *L, int index, const char *field,
                                     sl_quote_color_t *out) {
  unsigned char *values[3];
  int i;
  lua_getfield(L, index, field);
  luaL_checktype(L, -1, LUA_TTABLE);
  values[0] = &out->red;
  values[1] = &out->green;
  values[2] = &out->blue;
  for (i = 0; i < 3; i++) {
    lua_Integer value;
    lua_rawgeti(L, -1, i + 1);
    value = luaL_checkinteger(L, -1);
    if (value < 0 || value > 255)
      luaL_error(L, "quoted prompt %s colour channels must be 0..255", field);
    *values[i] = (unsigned char)value;
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

/** Lua editor:set_quoted_prompt_style(style): set independent RGB colours for
 * the dim prefix and italic text. nil restores the selected theme. */
static int softline_lua_set_quoted_prompt_style(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  sl_quote_style_t style;
  if (lua_isnoneornil(L, 2))
    return softline_lua_status(L, sl_set_quoted_prompt_style(handle->sl, NULL));
  luaL_checktype(L, 2, LUA_TTABLE);
  softline_lua_quote_color(L, 2, "prefix", &style.prefix);
  softline_lua_quote_color(L, 2, "text", &style.text);
  return softline_lua_status(L, sl_set_quoted_prompt_style(handle->sl, &style));
}

static int softline_lua_set_statusline(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_set_statusline(handle->sl, lua_toboolean(L, 2),
                           (size_t)luaL_checkinteger(L, 3)));
}

/** Lua editor:set_status_message(text): update the wrapped status message
 * above the status line, including during an active output stream. nil or an
 * empty string clears it. */
static int softline_lua_set_status_message(lua_State *L) {
  softline_lua_handle_t *handle;
  const char *message;
  handle = softline_lua_check(L, 1);
  message = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
  return softline_lua_status(L, sl_set_status_message(handle->sl, message));
}

/** Lua editor:set_status_message_prefix(prefix): change the first-row prefix;
 * continuation rows are indented to its display width. nil restores "! ". */
static int softline_lua_set_status_message_prefix(lua_State *L) {
  softline_lua_handle_t *handle;
  const char *prefix;
  handle = softline_lua_check(L, 1);
  prefix = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
  return softline_lua_status(L,
                             sl_set_status_message_prefix(handle->sl, prefix));
}

/** Lua editor:set_status_message_colors(prefix_color, text_color): select
 * separate theme palette roles for the prefix and italic message. */
static int softline_lua_set_status_message_colors(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(L, sl_set_status_message_colors(
                                    handle->sl,
                                    (sl_theme_color_t)luaL_checkinteger(L, 2),
                                    (sl_theme_color_t)luaL_checkinteger(L, 3)));
}

static int softline_lua_set_status_elements(lua_State *L) {
  const char *elements[SL_STATUS_MAX_ELEMENTS];
  softline_lua_handle_t *handle;
  size_t count;
  size_t i;
  size_t limit;
  luaL_checktype(L, 2, LUA_TTABLE);
  handle = softline_lua_check(L, 1);
  count = lua_rawlen(L, 2);
  limit = count > SL_STATUS_MAX_ELEMENTS ? SL_STATUS_MAX_ELEMENTS - 1 : count;
  memset(elements, 0, sizeof(elements));
  for (i = 0; i < limit; i++) {
    lua_rawgeti(L, 2, (lua_Integer)i + 1);
    if (!lua_isnil(L, -1))
      elements[i] = luaL_checkstring(L, -1);
    lua_pop(L, 1);
  }
  return softline_lua_status(
      L, sl_set_status_elements(handle->sl, elements, count));
}

static int softline_lua_set_status_element(lua_State *L) {
  softline_lua_handle_t *handle;
  const char *element;
  handle = softline_lua_check(L, 1);
  element = lua_isnil(L, 3) ? NULL : luaL_checkstring(L, 3);
  return softline_lua_status(
      L, sl_set_status_element(handle->sl, (size_t)luaL_checkinteger(L, 2),
                               element));
}

static int softline_lua_set_status_busy(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_set_status_busy(handle->sl, lua_toboolean(L, 2)));
}

static int softline_lua_set_status_spinner(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_set_status_spinner(handle->sl, lua_toboolean(L, 2)));
}

static int softline_lua_set_status_idle_marker(lua_State *L) {
  softline_lua_handle_t *handle;
  const char *marker;
  size_t marker_len;
  handle = softline_lua_check(L, 1);
  if (lua_isnil(L, 2))
    return softline_lua_status(L, sl_set_status_idle_marker(handle->sl, '\0'));
  marker = luaL_checklstring(L, 2, &marker_len);
  if (marker_len != 1)
    return luaL_argerror(L, 2, "must be one byte or nil");
  return softline_lua_status(L,
                             sl_set_status_idle_marker(handle->sl, marker[0]));
}

static int softline_lua_insert(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(L, sl_insert(handle->sl, luaL_checkstring(L, 2)));
}

static int softline_lua_set_buffer(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(L,
                             sl_set_buffer(handle->sl, luaL_checkstring(L, 2)));
}

/** Lua history_search_active(): boolean editor search mode, also for failed
 * matches. False after selection, cancellation or editor exit. Read-only on
 * the editor-owner thread. Key callbacks run before built-in handling; return
 * KEY_ACTION_PASS during search to allow editable selection before guarding
 * submissions. */
static int softline_lua_history_search_active(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  lua_pushboolean(L, sl_history_search_active(handle->sl));
  return 1;
}

static int softline_lua_buffer(lua_State *L) {
  softline_lua_handle_t *handle;
  const char *buffer;
  handle = softline_lua_check(L, 1);
  buffer = sl_buffer(handle->sl);
  lua_pushstring(L, buffer ? buffer : "");
  return 1;
}

static int softline_lua_cursor(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  lua_pushinteger(L, (lua_Integer)sl_cursor(handle->sl));
  return 1;
}

static int softline_lua_set_cursor(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_set_cursor(handle->sl, (size_t)luaL_checkinteger(L, 2)));
}

static int softline_lua_submit(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(L, sl_submit(handle->sl));
}

static int softline_lua_cancel(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(L, sl_cancel(handle->sl));
}
static int softline_lua_suspend(lua_State *L) {
  return softline_lua_status(L, sl_suspend(softline_lua_check(L, 1)->sl));
}

/** Lua editor:bind_key(key, callback) overrides built-in handling. An Enter
 * callback returning KEY_ACTION_SUBMIT submits a reverse-search match
 * immediately; the default Enter accepts it into the editable prompt. */
static int softline_lua_bind_key(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_key_t key;
  int slot;
  int status;
  handle = softline_lua_check(L, 1);
  key = (sl_key_t)luaL_checkinteger(L, 2);
  slot = softline_lua_find_key_ref(handle, key);
  if (lua_isnoneornil(L, 3)) {
    if (slot >= 0) {
      luaL_unref(L, LUA_REGISTRYINDEX, handle->key_bindings[slot].ref);
      handle->key_bindings[slot].ref = LUA_NOREF;
    }
    return softline_lua_status(L, sl_bind_key(handle->sl, key, NULL, NULL));
  }
  luaL_checktype(L, 3, LUA_TFUNCTION);
  if (slot < 0) {
    slot = softline_lua_find_empty_key_ref(handle);
    if (slot < 0)
      return softline_lua_status(L, SL_ERROR_NOMEM);
  } else {
    luaL_unref(L, LUA_REGISTRYINDEX, handle->key_bindings[slot].ref);
  }
  lua_pushvalue(L, 3);
  handle->key_bindings[slot].key = key;
  handle->key_bindings[slot].ref = luaL_ref(L, LUA_REGISTRYINDEX);
  status = sl_bind_key(handle->sl, key, softline_lua_key_callback, handle);
  if (status != SL_OK) {
    luaL_unref(L, LUA_REGISTRYINDEX, handle->key_bindings[slot].ref);
    handle->key_bindings[slot].ref = LUA_NOREF;
  }
  return softline_lua_status(L, status);
}

static int softline_lua_set_idle_callback(lua_State *L) {
  softline_lua_handle_t *handle;
  int ref;
  int status;
  handle = softline_lua_check(L, 1);
  if (lua_isnoneornil(L, 2)) {
    status = sl_set_idle_callback(handle->sl, NULL, NULL);
    if (status == SL_OK && handle->idle_callback_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, handle->idle_callback_ref);
      handle->idle_callback_ref = LUA_NOREF;
    }
    return softline_lua_status(L, status);
  }
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  ref = luaL_ref(L, LUA_REGISTRYINDEX);
  status = sl_set_idle_callback(handle->sl, softline_lua_idle_callback, handle);
  if (status != SL_OK) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return softline_lua_status(L, status);
  }
  if (handle->idle_callback_ref != LUA_NOREF)
    luaL_unref(L, LUA_REGISTRYINDEX, handle->idle_callback_ref);
  handle->idle_callback_ref = ref;
  return softline_lua_status(L, status);
}

static int softline_lua_watch_add(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_watch_id_t id;
  luaL_Stream *stream;
  int fd;
  int file_argument;
  int slot;
  int status;
  unsigned int events;
  handle = softline_lua_check(L, 1);
  file_argument = 0;
  if (lua_isinteger(L, 2))
    fd = (int)lua_tointeger(L, 2);
  else {
    stream = (luaL_Stream *)luaL_testudata(L, 2, LUA_FILEHANDLE);
    if (!stream || !stream->f)
      return luaL_argerror(L, 2, "watch fd must be an integer or open file");
    fd = fileno(stream->f);
    if (fd < 0)
      return luaL_argerror(L, 2, "file has no descriptor");
    file_argument = 1;
  }
  events = (unsigned int)luaL_checkinteger(L, 3);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  slot = softline_lua_find_empty_watch_ref(handle);
  if (slot < 0)
    return softline_lua_status(L, SL_ERROR_FULL);
  lua_pushvalue(L, 4);
  handle->watches[slot].ref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (file_argument) {
    lua_pushvalue(L, 2);
    handle->watches[slot].file_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  id = 0;
  status = sl_watch_add(handle->sl, fd, events, softline_lua_watch_callback,
                        handle, &id);
  if (status != SL_OK) {
    softline_lua_release_watch_ref(L, handle, slot);
    return softline_lua_status(L, status);
  }
  handle->watches[slot].id = id;
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}

static int softline_lua_watch_modify(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(
      L, sl_watch_modify(handle->sl, (sl_watch_id_t)luaL_checkinteger(L, 2),
                         (unsigned int)luaL_checkinteger(L, 3)));
}

static int softline_lua_watch_remove(lua_State *L) {
  softline_lua_handle_t *handle;
  sl_watch_id_t id;
  int slot;
  int status;
  handle = softline_lua_check(L, 1);
  id = (sl_watch_id_t)luaL_checkinteger(L, 2);
  status = sl_watch_remove(handle->sl, id);
  if (status == SL_OK) {
    slot = softline_lua_find_watch_ref(handle, id);
    if (slot >= 0)
      softline_lua_release_watch_ref(L, handle, slot);
  }
  return softline_lua_status(L, status);
}

static int softline_lua_watch_clear(lua_State *L) {
  softline_lua_handle_t *handle;
  int i;
  int status;
  handle = softline_lua_check(L, 1);
  status = sl_watch_clear(handle->sl);
  if (status != SL_OK)
    return softline_lua_status(L, status);
  for (i = 0; i < SOFTLINE_LUA_MAX_WATCHES; i++) {
    softline_lua_release_watch_ref(L, handle, i);
  }
  return softline_lua_status(L, SL_OK);
}

static int softline_lua_last_readline_status(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  if (handle->idle_callback_failed || handle->watch_callback_failed) {
    lua_pushinteger(L, SL_READLINE_ERROR);
    return 1;
  }
  lua_pushinteger(L, sl_last_readline_status(handle->sl));
  return 1;
}

static int softline_lua_last_error(lua_State *L) {
  softline_lua_handle_t *handle;
  const char *error;
  handle = softline_lua_check(L, 1);
  if (handle->idle_callback_failed || handle->watch_callback_failed) {
    lua_pushstring(L, handle->watch_callback_failed
                          ? handle->watch_callback_error
                          : handle->idle_callback_error);
    return 1;
  }
  error = sl_last_error(handle->sl);
  if (!error) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, error);
  return 1;
}

static int softline_lua_next_chunk(sl_t *sl, void *userdata, const char **chunk,
                                   size_t *len) {
  softline_lua_stream_t *stream;
  lua_State *L;
  (void)sl;
  stream = (softline_lua_stream_t *)userdata;
  L = stream->L;
  lua_settop(L, stream->base_top);
  *chunk = NULL;
  *len = 0;
  if (stream->kind == LUA_TSTRING) {
    if (stream->index > 0)
      return SL_OK;
    lua_rawgeti(L, LUA_REGISTRYINDEX, stream->ref);
    *chunk = lua_tolstring(L, -1, len);
    stream->index++;
    return SL_OK;
  }
  if (stream->kind == LUA_TTABLE) {
    stream->index++;
    lua_rawgeti(L, LUA_REGISTRYINDEX, stream->ref);
    lua_rawgeti(L, -1, stream->index);
    if (lua_isnil(L, -1))
      return SL_OK;
    if (!lua_isstring(L, -1))
      return SL_ERROR_INVALID;
    *chunk = lua_tolstring(L, -1, len);
    return SL_OK;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, stream->ref);
  lua_pushinteger(L, stream->index + 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK)
    return SL_ERROR;
  stream->index++;
  if (lua_isnil(L, -1))
    return SL_OK;
  if (!lua_isstring(L, -1))
    return SL_ERROR_INVALID;
  *chunk = lua_tolstring(L, -1, len);
  return SL_OK;
}

/* Delegate finite chunks to the core's native resize reconciliation, including
 * physical resize while a Lua source function is running. */
static int softline_lua_print_above(lua_State *L) {
  softline_lua_handle_t *handle;
  softline_lua_stream_t stream;
  int status;
  handle = softline_lua_check(L, 1);
  luaL_checkany(L, 2);
  stream.L = L;
  stream.kind = lua_type(L, 2);
  stream.index = 0;
  stream.base_top = lua_gettop(L);
  if (stream.kind != LUA_TSTRING && stream.kind != LUA_TTABLE &&
      stream.kind != LUA_TFUNCTION)
    return luaL_error(L, "print_above expects string, table, or function");
  lua_pushvalue(L, 2);
  stream.ref = luaL_ref(L, LUA_REGISTRYINDEX);
  status = sl_print_above(handle->sl, softline_lua_next_chunk, &stream);
  luaL_unref(L, LUA_REGISTRYINDEX, stream.ref);
  if (status == SL_ERROR && lua_isstring(L, -1)) {
    lua_pushnil(L);
    lua_pushinteger(L, status);
    lua_pushvalue(L, -3);
    return 3;
  }
  return softline_lua_status(L, status);
}

/** Lua editor:output_stream_begin(): open one renderer-agnostic session.
 * Reopening a closed native session probes the cursor again. A nonzero column
 * restarts quote separation for an unfinished externally written line;
 * otherwise known spacing remains. An editor retains its continuation state.
 * The transcript starts at the original cursor; the prompt starts at the
 * bottom and follows native resize. Its actual position sets the output margin.
 * A clipped unfinished line resumes at the first visible output row without
 * replay. Rapid tmux resizes can outrun PTY size notifications while output
 * is emitted, so exact output in that interval is not guaranteed.
 * The Lua/editor owner thread controls writes; an external producer hands
 * chunks through a watched descriptor. Non-TTY handles forward validated bytes
 * only.
 */
static int softline_lua_output_stream_begin(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(L, sl_output_stream_begin(handle->sl));
}

/** Lua editor:output_stream_reconcile(): observe resize outside feed writes. */
static int softline_lua_output_stream_reconcile(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  return softline_lua_status(L, sl_output_stream_reconcile(handle->sl));
}

/** Lua editor:output_stream_write(bytes): forward a byte string immediately.
 * No newline or document boundary is inferred; an empty string is a no-op.
 * Printable UTF-8, LF/CR/Tab and ANSI SGR are accepted. Partial ANSI/UTF-8 is
 * bounded and retained across writes; complete bytes are emitted before return.
 * Native LF follows OPOST/ONLCR. While Softline owns raw mode, kernel tab
 * expansion (TAB3/OXTABS) is disabled so tabs reach the terminal unchanged;
 * original flags are restored when raw mode is released.
 * Native writes after release disable expansion for the write and restore it
 * before returning, without flushing pending input.
 * Feed calls add no intentional delay and never request or wait for terminal
 * cursor replies, including on resize. Cursor advances use local scalar cell
 * widths shared with prompt layout; no producer transcript, glyph spans or
 * grapheme tails are buffered. Terminal Unicode versions and cluster shaping
 * can differ from the Unicode 16.0 accounting;
 * physical resize can race a VT write and misplace even ASCII output. This
 * is an accepted concurrency limit, with no feed wait or transcript replay;
 * a retained frame resized without an input-owned observation, including after
 * readline returns, uses a delta estimate. Earlier hard-ended ASCII reflow or
 * unknown terminal history can misplace continuation even after resize settles.
 * The active input loop observes resize before dispatching producer watches.
 * Synchronous producers call output_stream_reconcile() between feeds after
 * resize settles; this does not change observation-free feed behavior.
 * wrapping remains producer/terminal-owned. Width resize keeps the tracked
 * column of an opaque line and clamps it
 * only if offscreen. Continuation after Unicode/tab reflow can land in a gap or
 * overwrite text, including CR tails. With an active prompt, a later printable
 * chunk after a known ASCII right-edge write uses a hard row advance; width
 * growth may leave it split and a divided Unicode cluster may lose its
 * attachment. Bottom-margin reflow can also leave a continuation split after
 * growth. A last-column report cannot distinguish Unicode pending wrap from a
 * cursor before the final cell. These intentional opaque-feed limits are
 * accepted without reconstructing or replaying producer text.
 */
static int softline_lua_output_stream_write(lua_State *L) {
  softline_lua_handle_t *handle;
  const char *bytes;
  size_t length;
  handle = softline_lua_check(L, 1);
  bytes = luaL_checklstring(L, 2, &length);
  return softline_lua_status(L,
                             sl_output_stream_write(handle->sl, bytes, length));
}

/** Lua editor:output_stream_write_quoted_prompt(text): write a literal,
 * wrapped quote between renderer segments, with one empty row on each side. */
static int softline_lua_output_stream_write_quoted_prompt(lua_State *L) {
  softline_lua_handle_t *handle = softline_lua_check(L, 1);
  const char *text = luaL_checkstring(L, 2);
  return softline_lua_status(
      L, sl_output_stream_write_quoted_prompt(handle->sl, text));
}

/** Lua editor:output_stream_end(): end the producer session without finishing
 * an external renderer document. An active editor retains its prompt and
 * scroll region. Otherwise native chat closes and releases terminal input.
 * By default, clear editable input rows, retain queue/status rows and return
 * at column zero on the current input row without a newline or scroll.
 * clear_prompt_on_exit=true clears the whole prompt area and returns below
 * output. Between editor calls, outside callbacks, a foreground child may
 * run before output_stream_begin() probes a fresh cursor and builds a new
 * prompt, retaining handle history and settings. The application owns child
 * execution. Incomplete ANSI/UTF-8 returns nil, ERROR_INVALID and leaves the
 * session open for the missing bytes. */
static int softline_lua_output_stream_end(lua_State *L) {
  softline_lua_handle_t *handle;
  handle = softline_lua_check(L, 1);
  return softline_lua_status(L, sl_output_stream_end(handle->sl));
}

static const luaL_Reg softline_lua_methods[] = {
    {"queue_backend_request", softline_lua_queue_backend_request},
    {"queue_backend_result", softline_lua_queue_backend_result},
    {"queue_backend_update", softline_lua_queue_backend_update},
    {"queue_backend_info", softline_lua_queue_backend_info},
    {"queue_backend_entry", softline_lua_queue_backend_entry},
    {"queue_backend_pending", softline_lua_queue_backend_pending},
    {"queue_backend_recover", softline_lua_queue_backend_recover},
    {"readline", softline_lua_readline},
    {"next_prompt", softline_lua_next_prompt},
    {"history_add", softline_lua_history_add},
    {"history_set_max_len", softline_lua_history_set_max_len},
    {"history_save", softline_lua_history_save},
    {"history_load", softline_lua_history_load},
    {"history_set_backend", softline_lua_history_set_backend},
    {"history_open", softline_lua_history_open},
    {"history_close", softline_lua_history_close},
    {"history_compact", softline_lua_history_compact},
    {"history_set_auto_add", softline_lua_history_set_auto_add},
    {"set_screen_width", softline_lua_set_screen_width},
    {"set_completion_providers", softline_lua_set_completion_providers},
    {"set_osc8_mode", softline_lua_set_osc8_mode},
    {"get_osc8_info", softline_lua_get_osc8_info},
    {"set_live_scroll_region", softline_lua_set_live_scroll_region},
    {"set_image_paste_path_template",
     softline_lua_set_image_paste_path_template},
    {"set_prompt_queue", softline_lua_set_prompt_queue},
    {"queue_count", softline_lua_queue_count},
    {"queue_capacity", softline_lua_queue_capacity},
    {"queue_peek", softline_lua_queue_peek},
    {"queue_insert", softline_lua_queue_insert},
    {"queue_append", softline_lua_queue_append},
    {"inject_queued_prompt", softline_lua_inject_queued_prompt},
    {"inject_steer_prompt", softline_lua_inject_steer_prompt},
    {"set_auto_approval", softline_lua_set_auto_approval},
    {"get_auto_approval", softline_lua_get_auto_approval},
    {"queue_info", softline_lua_queue_info},
    {"queue_draft_info", softline_lua_queue_draft_info},
    {"set_queue_policy", softline_lua_set_queue_policy},
    {"queue_set_executable", softline_lua_queue_set_executable},
    {"queue_stopped", softline_lua_queue_stopped},
    {"queue_resume", softline_lua_queue_resume},
    {"queue_approve", softline_lua_queue_approve},
    {"queue_reject", softline_lua_queue_reject},
    {"last_prompt_id", softline_lua_last_prompt_id},
    {"queue_replace", softline_lua_queue_replace},
    {"queue_take", softline_lua_queue_take},
    {"queue_mode", softline_lua_queue_mode},
    {"queue_set_mode", softline_lua_queue_set_mode},
    {"queue_clear", softline_lua_queue_clear},
    {"queue_draft", softline_lua_queue_draft},
    {"set_queue_delivery", softline_lua_set_queue_delivery},
    {"queue_delivery", softline_lua_queue_delivery},
    {"set_queue_profile", softline_lua_set_queue_profile},
    {"queue_profile", softline_lua_queue_profile},
    {"set_queue_keys", softline_lua_set_queue_keys},
    {"queue_keys", softline_lua_queue_keys},
    {"set_prompt_theme", softline_lua_set_prompt_theme},
    {"set_quoted_prompt_prefix", softline_lua_set_quoted_prompt_prefix},
    {"set_quoted_prompt_style", softline_lua_set_quoted_prompt_style},
    {"set_statusline", softline_lua_set_statusline},
    {"set_status_message", softline_lua_set_status_message},
    {"set_status_message_prefix", softline_lua_set_status_message_prefix},
    {"set_status_message_colors", softline_lua_set_status_message_colors},
    {"set_status_elements", softline_lua_set_status_elements},
    {"set_status_element", softline_lua_set_status_element},
    {"set_status_busy", softline_lua_set_status_busy},
    {"set_status_spinner", softline_lua_set_status_spinner},
    {"set_status_idle_marker", softline_lua_set_status_idle_marker},
    {"insert", softline_lua_insert},
    {"set_buffer", softline_lua_set_buffer},
    {"buffer", softline_lua_buffer},
    {"history_search_active", softline_lua_history_search_active},
    {"cursor", softline_lua_cursor},
    {"set_cursor", softline_lua_set_cursor},
    {"submit", softline_lua_submit},
    {"cancel", softline_lua_cancel},
    {"suspend", softline_lua_suspend},
    {"bind_key", softline_lua_bind_key},
    {"set_idle_callback", softline_lua_set_idle_callback},
    {"watch_add", softline_lua_watch_add},
    {"watch_modify", softline_lua_watch_modify},
    {"watch_remove", softline_lua_watch_remove},
    {"watch_clear", softline_lua_watch_clear},
    {"print_above", softline_lua_print_above},
    {"output_stream_begin", softline_lua_output_stream_begin},
    {"output_stream_reconcile", softline_lua_output_stream_reconcile},
    {"output_stream_write", softline_lua_output_stream_write},
    {"output_stream_write_quoted_prompt",
     softline_lua_output_stream_write_quoted_prompt},
    {"output_stream_end", softline_lua_output_stream_end},
    {"last_readline_status", softline_lua_last_readline_status},
    {"last_error", softline_lua_last_error},
    {"close", softline_lua_close},
    {NULL, NULL}};

static const luaL_Reg softline_lua_functions[] = {
    {"new", softline_lua_new},
    {"terminal_handoff", softline_lua_terminal_handoff},
    {"terminal_local", softline_lua_terminal_local},
    {NULL, NULL}};

int luaopen_softline(lua_State *L) {
  luaL_newmetatable(L, SOFTLINE_LUA_HANDLE);
  lua_pushcfunction(L, softline_lua_gc);
  lua_setfield(L, -2, "__gc");
  lua_newtable(L);
  luaL_setfuncs(L, softline_lua_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  lua_newtable(L);
  luaL_setfuncs(L, softline_lua_functions, 0);
  lua_pushinteger(L, SL_TERMINAL_OPEN);
  lua_setfield(L, -2, "TERMINAL_OPEN");
  lua_pushinteger(L, SL_TERMINAL_STEP);
  lua_setfield(L, -2, "TERMINAL_STEP");
  lua_pushinteger(L, SL_TERMINAL_READ);
  lua_setfield(L, -2, "TERMINAL_READ");
  lua_pushinteger(L, SL_TERMINAL_WRITE);
  lua_setfield(L, -2, "TERMINAL_WRITE");
  lua_pushinteger(L, SL_TERMINAL_RESIZE);
  lua_setfield(L, -2, "TERMINAL_RESIZE");
  lua_pushinteger(L, SL_TERMINAL_CLOSE);
  lua_setfield(L, -2, "TERMINAL_CLOSE");
  lua_pushinteger(L, SL_READLINE_NONE);
  lua_setfield(L, -2, "READLINE_NONE");
  lua_pushinteger(L, SL_READLINE_SUBMITTED);
  lua_setfield(L, -2, "READLINE_SUBMITTED");
  lua_pushinteger(L, SL_READLINE_EOF);
  lua_setfield(L, -2, "READLINE_EOF");
  lua_pushinteger(L, SL_READLINE_CANCELLED);
  lua_setfield(L, -2, "READLINE_CANCELLED");
  lua_pushinteger(L, SL_READLINE_INTERRUPTED);
  lua_setfield(L, -2, "READLINE_INTERRUPTED");
  lua_pushinteger(L, SL_READLINE_ERROR);
  lua_setfield(L, -2, "READLINE_ERROR");
  lua_pushinteger(L, SL_READLINE_SUSPENDED);
  lua_setfield(L, -2, "READLINE_SUSPENDED");
  lua_pushinteger(L, SL_OK);
  lua_setfield(L, -2, "OK");
  lua_pushinteger(L, SL_ERROR);
  lua_setfield(L, -2, "ERROR");
  lua_pushinteger(L, SL_ERROR_INVALID);
  lua_setfield(L, -2, "ERROR_INVALID");
  lua_pushinteger(L, SL_ERROR_NOMEM);
  lua_setfield(L, -2, "ERROR_NOMEM");
  lua_pushinteger(L, SL_ERROR_IO);
  lua_setfield(L, -2, "ERROR_IO");
  lua_pushinteger(L, SL_ERROR_FULL);
  lua_setfield(L, -2, "ERROR_FULL");
  lua_pushinteger(L, SL_ERROR_UNSUPPORTED);
  lua_setfield(L, -2, "ERROR_UNSUPPORTED");
  lua_pushinteger(L, SL_ERROR_CONFLICT);
  lua_setfield(L, -2, "ERROR_CONFLICT");
  lua_pushinteger(L, SL_ERROR_NOT_READY);
  lua_setfield(L, -2, "ERROR_NOT_READY");
  lua_pushinteger(L, SL_QUEUE_OFFER);
  lua_setfield(L, -2, "QUEUE_OFFER");
  lua_pushinteger(L, SL_QUEUE_BEGIN_EDIT);
  lua_setfield(L, -2, "QUEUE_BEGIN_EDIT");
  lua_pushinteger(L, SL_QUEUE_END_EDIT);
  lua_setfield(L, -2, "QUEUE_END_EDIT");
  lua_pushinteger(L, SL_QUEUE_ABORT_EDIT);
  lua_setfield(L, -2, "QUEUE_ABORT_EDIT");
  lua_pushinteger(L, SL_QUEUE_APPROVE);
  lua_setfield(L, -2, "QUEUE_APPROVE");
  lua_pushinteger(L, SL_QUEUE_WITHDRAW);
  lua_setfield(L, -2, "QUEUE_WITHDRAW");
  lua_pushinteger(L, SL_QUEUE_SET_INTENT);
  lua_setfield(L, -2, "QUEUE_SET_INTENT");
  lua_pushinteger(L, SL_QUEUE_STOP);
  lua_setfield(L, -2, "QUEUE_STOP");
  lua_pushinteger(L, SL_QUEUE_RESUME);
  lua_setfield(L, -2, "QUEUE_RESUME");
  lua_pushinteger(L, SL_QUEUE_CLEAR);
  lua_setfield(L, -2, "QUEUE_CLEAR");
  lua_pushinteger(L, SL_QUEUE_AUTO_APPROVAL);
  lua_setfield(L, -2, "QUEUE_AUTO_APPROVAL");
  lua_pushinteger(L, SL_QUEUE_SYNCHRONIZE);
  lua_setfield(L, -2, "QUEUE_SYNCHRONIZE");
  lua_pushinteger(L, SL_QUEUE_RESOLVE);
  lua_setfield(L, -2, "QUEUE_RESOLVE");
  lua_pushinteger(L, SL_QUEUE_ACCEPTED);
  lua_setfield(L, -2, "QUEUE_ACCEPTED");
  lua_pushinteger(L, SL_QUEUE_REJECTED);
  lua_setfield(L, -2, "QUEUE_REJECTED");
  lua_pushinteger(L, SL_QUEUE_UNKNOWN);
  lua_setfield(L, -2, "QUEUE_UNKNOWN");
  lua_pushinteger(L, SL_QUEUE_SNAPSHOT);
  lua_setfield(L, -2, "QUEUE_SNAPSHOT");
  lua_pushinteger(L, SL_QUEUE_UPSERT);
  lua_setfield(L, -2, "QUEUE_UPSERT");
  lua_pushinteger(L, SL_QUEUE_REMOVE);
  lua_setfield(L, -2, "QUEUE_REMOVE");
  lua_pushinteger(L, SL_QUEUE_STATE);
  lua_setfield(L, -2, "QUEUE_STATE");
  lua_pushinteger(L, SL_QUEUE_DISCONNECTED);
  lua_setfield(L, -2, "QUEUE_DISCONNECTED");
  lua_pushinteger(L, SL_WATCH_READ);
  lua_setfield(L, -2, "WATCH_READ");
  lua_pushinteger(L, SL_WATCH_WRITE);
  lua_setfield(L, -2, "WATCH_WRITE");
  lua_pushinteger(L, SL_WATCH_ERROR);
  lua_setfield(L, -2, "WATCH_ERROR");
  lua_pushinteger(L, SL_WATCH_HANGUP);
  lua_setfield(L, -2, "WATCH_HANGUP");
  lua_pushinteger(L, SL_PROMPT_SOURCE_NONE);
  lua_setfield(L, -2, "PROMPT_SOURCE_NONE");
  lua_pushinteger(L, SL_PROMPT_SOURCE_DIRECT);
  lua_setfield(L, -2, "PROMPT_SOURCE_DIRECT");
  lua_pushinteger(L, SL_PROMPT_SOURCE_QUEUED);
  lua_setfield(L, -2, "PROMPT_SOURCE_QUEUED");
  lua_pushinteger(L, SL_PROMPT_SOURCE_PROMOTED);
  lua_setfield(L, -2, "PROMPT_SOURCE_PROMOTED");
  lua_pushinteger(L, SL_PROMPT_QUEUE_MODE_QUEUED);
  lua_setfield(L, -2, "QUEUE_MODE_QUEUED");
  lua_pushinteger(L, SL_PROMPT_QUEUE_MODE_STEER);
  lua_setfield(L, -2, "QUEUE_MODE_STEER");
  lua_pushinteger(L, SL_PROMPT_THEME_PLAIN);
  lua_setfield(L, -2, "PROMPT_THEME_PLAIN");
  lua_pushinteger(L, SL_PROMPT_THEME_ACCENT);
  lua_setfield(L, -2, "PROMPT_THEME_ACCENT");
  lua_pushinteger(L, SL_PROMPT_THEME_DRACULA);
  lua_setfield(L, -2, "PROMPT_THEME_DRACULA");
  lua_pushinteger(L, SL_PROMPT_THEME_GRUVBOX);
  lua_setfield(L, -2, "PROMPT_THEME_GRUVBOX");
  lua_pushinteger(L, SL_PROMPT_THEME_MONOCHROME);
  lua_setfield(L, -2, "PROMPT_THEME_MONOCHROME");
  lua_pushinteger(L, SL_PROMPT_THEME_MONOGREEN);
  lua_setfield(L, -2, "PROMPT_THEME_MONOGREEN");
  lua_pushinteger(L, SL_PROMPT_THEME_OUTRUN);
  lua_setfield(L, -2, "PROMPT_THEME_OUTRUN");
  lua_pushinteger(L, SL_PROMPT_THEME_RICED);
  lua_setfield(L, -2, "PROMPT_THEME_RICED");
  lua_pushinteger(L, SL_PROMPT_THEME_SYNTHWAVE);
  lua_setfield(L, -2, "PROMPT_THEME_SYNTHWAVE");
  lua_pushinteger(L, SL_PROMPT_THEME_DEFAULT);
  lua_setfield(L, -2, "PROMPT_THEME_DEFAULT");
  lua_pushinteger(L, SL_THEME_COLOR_MUTED);
  lua_setfield(L, -2, "THEME_COLOR_MUTED");
  lua_pushinteger(L, SL_THEME_COLOR_SECONDARY);
  lua_setfield(L, -2, "THEME_COLOR_SECONDARY");
  lua_pushinteger(L, SL_THEME_COLOR_PROMPT);
  lua_setfield(L, -2, "THEME_COLOR_PROMPT");
  lua_pushinteger(L, SL_THEME_COLOR_QUEUE);
  lua_setfield(L, -2, "THEME_COLOR_QUEUE");
  lua_pushinteger(L, SL_THEME_COLOR_INPUT);
  lua_setfield(L, -2, "THEME_COLOR_INPUT");
  lua_pushinteger(L, SL_THEME_COLOR_ELEMENT_0);
  lua_setfield(L, -2, "THEME_COLOR_ELEMENT_0");
  lua_pushinteger(L, SL_THEME_COLOR_ELEMENT_1);
  lua_setfield(L, -2, "THEME_COLOR_ELEMENT_1");
  lua_pushinteger(L, SL_THEME_COLOR_ELEMENT_2);
  lua_setfield(L, -2, "THEME_COLOR_ELEMENT_2");
  lua_pushinteger(L, SL_THEME_COLOR_ELEMENT_3);
  lua_setfield(L, -2, "THEME_COLOR_ELEMENT_3");
  lua_pushinteger(L, SL_THEME_COLOR_ELEMENT_4);
  lua_setfield(L, -2, "THEME_COLOR_ELEMENT_4");
  lua_pushinteger(L, SL_THEME_COLOR_ELEMENT_5);
  lua_setfield(L, -2, "THEME_COLOR_ELEMENT_5");
  lua_pushinteger(L, SL_THEME_COLOR_ELEMENT_6);
  lua_setfield(L, -2, "THEME_COLOR_ELEMENT_6");
  lua_pushinteger(L, SL_THEME_COLOR_ELEMENT_7);
  lua_setfield(L, -2, "THEME_COLOR_ELEMENT_7");
  lua_pushinteger(L, SL_STATUS_MAX_ELEMENTS);
  lua_setfield(L, -2, "STATUS_MAX_ELEMENTS");
  lua_pushinteger(L, SL_KEY_CTRL_C);
  lua_setfield(L, -2, "KEY_CTRL_C");
  lua_pushinteger(L, SL_KEY_NONE);
  lua_setfield(L, -2, "KEY_NONE");
  lua_pushinteger(L, SL_KEY_CTRL_A);
  lua_setfield(L, -2, "KEY_CTRL_A");
  lua_pushinteger(L, SL_KEY_CTRL_B);
  lua_setfield(L, -2, "KEY_CTRL_B");
  lua_pushinteger(L, SL_KEY_CTRL_D);
  lua_setfield(L, -2, "KEY_CTRL_D");
  lua_pushinteger(L, SL_KEY_CTRL_E);
  lua_setfield(L, -2, "KEY_CTRL_E");
  lua_pushinteger(L, SL_KEY_CTRL_F);
  lua_setfield(L, -2, "KEY_CTRL_F");
  lua_pushinteger(L, SL_KEY_CTRL_J);
  lua_setfield(L, -2, "KEY_CTRL_J");
  lua_pushinteger(L, SL_KEY_CTRL_K);
  lua_setfield(L, -2, "KEY_CTRL_K");
  lua_pushinteger(L, SL_KEY_CTRL_L);
  lua_setfield(L, -2, "KEY_CTRL_L");
  lua_pushinteger(L, SL_KEY_CTRL_R);
  lua_setfield(L, -2, "KEY_CTRL_R");
  lua_pushinteger(L, SL_KEY_CTRL_T);
  lua_setfield(L, -2, "KEY_CTRL_T");
  lua_pushinteger(L, SL_KEY_CTRL_U);
  lua_setfield(L, -2, "KEY_CTRL_U");
  lua_pushinteger(L, SL_KEY_CTRL_V);
  lua_setfield(L, -2, "KEY_CTRL_V");
  lua_pushinteger(L, SL_KEY_CTRL_W);
  lua_setfield(L, -2, "KEY_CTRL_W");
  lua_pushinteger(L, SL_KEY_CTRL_X);
  lua_setfield(L, -2, "KEY_CTRL_X");
  lua_pushinteger(L, SL_KEY_CTRL_Y);
  lua_setfield(L, -2, "KEY_CTRL_Y");
  lua_pushinteger(L, SL_KEY_CTRL_UNDERSCORE);
  lua_setfield(L, -2, "KEY_CTRL_UNDERSCORE");
  lua_pushinteger(L, SL_KEY_ESCAPE);
  lua_setfield(L, -2, "KEY_ESCAPE");
  lua_pushinteger(L, SL_KEY_BACKSPACE);
  lua_setfield(L, -2, "KEY_BACKSPACE");
  lua_pushinteger(L, SL_KEY_TAB);
  lua_setfield(L, -2, "KEY_TAB");
  lua_pushinteger(L, SL_OSC8_OFF);
  lua_setfield(L, -2, "OSC8_OFF");
  lua_pushinteger(L, SL_OSC8_ON);
  lua_setfield(L, -2, "OSC8_ON");
  lua_pushinteger(L, SL_OSC8_AUTO);
  lua_setfield(L, -2, "OSC8_AUTO");
  lua_pushinteger(L, SL_OSC8_MAX_BYTES);
  lua_setfield(L, -2, "OSC8_MAX_BYTES");
  lua_pushinteger(L, SL_COMPLETION_START);
  lua_setfield(L, -2, "COMPLETION_START");
  lua_pushinteger(L, SL_COMPLETION_WORD);
  lua_setfield(L, -2, "COMPLETION_WORD");
  lua_pushinteger(L, SL_KEY_ENTER);
  lua_setfield(L, -2, "KEY_ENTER");
  lua_pushinteger(L, SL_KEY_CTRL_N);
  lua_setfield(L, -2, "KEY_CTRL_N");
  lua_pushinteger(L, SL_KEY_CTRL_P);
  lua_setfield(L, -2, "KEY_CTRL_P");
  lua_pushinteger(L, SL_KEY_UP);
  lua_setfield(L, -2, "KEY_UP");
  lua_pushinteger(L, SL_KEY_DOWN);
  lua_setfield(L, -2, "KEY_DOWN");
  lua_pushinteger(L, SL_KEY_LEFT);
  lua_setfield(L, -2, "KEY_LEFT");
  lua_pushinteger(L, SL_KEY_RIGHT);
  lua_setfield(L, -2, "KEY_RIGHT");
  lua_pushinteger(L, SL_KEY_HOME);
  lua_setfield(L, -2, "KEY_HOME");
  lua_pushinteger(L, SL_KEY_END);
  lua_setfield(L, -2, "KEY_END");
  lua_pushinteger(L, SL_KEY_DELETE);
  lua_setfield(L, -2, "KEY_DELETE");
  lua_pushinteger(L, SL_KEY_ALT_B);
  lua_setfield(L, -2, "KEY_ALT_B");
  lua_pushinteger(L, SL_KEY_ALT_F);
  lua_setfield(L, -2, "KEY_ALT_F");
  lua_pushinteger(L, SL_KEY_ALT_D);
  lua_setfield(L, -2, "KEY_ALT_D");
  lua_pushinteger(L, SL_KEY_ALT_BACKSPACE);
  lua_setfield(L, -2, "KEY_ALT_BACKSPACE");
  lua_pushinteger(L, SL_KEY_ALT_Y);
  lua_setfield(L, -2, "KEY_ALT_Y");
  lua_pushinteger(L, SL_KEY_UNKNOWN);
  lua_setfield(L, -2, "KEY_UNKNOWN");
  lua_pushinteger(L, SL_KEY_PASTE_BEGIN);
  lua_setfield(L, -2, "KEY_PASTE_BEGIN");
  lua_pushinteger(L, SL_KEY_PASTE_END);
  lua_setfield(L, -2, "KEY_PASTE_END");
  lua_pushinteger(L, SL_KEY_F1);
  lua_setfield(L, -2, "KEY_F1");
  lua_pushinteger(L, SL_KEY_F2);
  lua_setfield(L, -2, "KEY_F2");
  lua_pushinteger(L, SL_KEY_F3);
  lua_setfield(L, -2, "KEY_F3");
  lua_pushinteger(L, SL_KEY_F4);
  lua_setfield(L, -2, "KEY_F4");
  lua_pushinteger(L, SL_KEY_F5);
  lua_setfield(L, -2, "KEY_F5");
  lua_pushinteger(L, SL_KEY_F6);
  lua_setfield(L, -2, "KEY_F6");
  lua_pushinteger(L, SL_KEY_F7);
  lua_setfield(L, -2, "KEY_F7");
  lua_pushinteger(L, SL_KEY_F8);
  lua_setfield(L, -2, "KEY_F8");
  lua_pushinteger(L, SL_KEY_F9);
  lua_setfield(L, -2, "KEY_F9");
  lua_pushinteger(L, SL_KEY_F10);
  lua_setfield(L, -2, "KEY_F10");
  lua_pushinteger(L, SL_KEY_CTRL_ENTER);
  lua_setfield(L, -2, "KEY_CTRL_ENTER");
  lua_pushinteger(L, SL_KEY_ALT_ENTER);
  lua_setfield(L, -2, "KEY_ALT_ENTER");
  lua_pushinteger(L, SL_KEY_ALT_E);
  lua_setfield(L, -2, "KEY_ALT_E");
  lua_pushinteger(L, SL_KEY_ALT_A);
  lua_setfield(L, -2, "KEY_ALT_A");
  lua_pushinteger(L, SL_KEY_PAGE_UP);
  lua_setfield(L, -2, "KEY_PAGE_UP");
  lua_pushinteger(L, SL_KEY_PAGE_DOWN);
  lua_setfield(L, -2, "KEY_PAGE_DOWN");
  lua_pushinteger(L, SL_KEY_ALT_BASE);
  lua_setfield(L, -2, "KEY_ALT_BASE");
  lua_pushinteger(L, SL_KEY_ALT_M);
  lua_setfield(L, -2, "KEY_ALT_M");
  lua_pushinteger(L, SL_KEY_ACTION_PASS);
  lua_setfield(L, -2, "KEY_ACTION_PASS");
  lua_pushinteger(L, SL_KEY_ACTION_HANDLED);
  lua_setfield(L, -2, "KEY_ACTION_HANDLED");
  lua_pushinteger(L, SL_KEY_ACTION_SUBMIT);
  lua_setfield(L, -2, "KEY_ACTION_SUBMIT");
  lua_pushinteger(L, SL_KEY_ACTION_CANCEL);
  lua_setfield(L, -2, "KEY_ACTION_CANCEL");
  lua_pushinteger(L, SL_KEY_ACTION_INTERRUPT);
  lua_setfield(L, -2, "KEY_ACTION_INTERRUPT");
  lua_pushinteger(L, SL_KEY_ACTION_RESET);
  lua_setfield(L, -2, "KEY_ACTION_RESET");
  lua_pushinteger(L, SL_KEY_ACTION_SUSPEND);
  lua_setfield(L, -2, "KEY_ACTION_SUSPEND");
  return 1;
}
