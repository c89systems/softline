#include "softline/softline.h"
#include "softline/www.h"
#include "softline_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generated from repository-owned assets, including pinned third-party files.
 */
#include "softline_www_assets.h"

typedef struct {
  sl_www_config_t config;
  char *settings;
  size_t settings_length;
  unsigned flags;
  int callback_active;
} www_impl_t;
typedef struct {
  char *data;
  size_t length, capacity;
  int failed;
} json_t;
static void append(json_t *j, const char *s, size_t n) {
  if (n > j->capacity - j->length) {
    j->failed = 1;
    return;
  }
  memcpy(j->data + j->length, s, n);
  j->length += n;
}
static void quoted(json_t *j, const char *s, size_t n) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  append(j, "\"", 1);
  for (i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      char escape[2];
      escape[0] = '\\';
      escape[1] = (char)c;
      append(j, escape, 2);
    } else if (c == '\n' || c == '\t') {
      append(j, c == '\n' ? "\\n" : "\\t", 2);
    } else if (c < 32) {
      char escape[6];
      memcpy(escape, "\\u00", 4);
      escape[4] = hex[c >> 4];
      escape[5] = hex[c & 15];
      append(j, escape, 6);
    } else
      append(j, (const char *)&c, 1);
  }
  append(j, "\"", 1);
}
static int identifier(const char *s) {
  size_t i;
  if (!s || !*s || strlen(s) > 64)
    return 0;
  for (i = 0; s[i]; ++i)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
          (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
      return 0;
  return 1;
}
static int utf8(const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i++];
    unsigned int value, min;
    int more;
    if (c < 128)
      continue;
    if (c >= 194 && c <= 223) {
      value = c & 31;
      more = 1;
      min = 128;
    } else if (c >= 224 && c <= 239) {
      value = c & 15;
      more = 2;
      min = 2048;
    } else if (c >= 240 && c <= 244) {
      value = c & 7;
      more = 3;
      min = 65536;
    } else
      return 0;
    while (more--) {
      if (i == n || ((unsigned char)s[i] & 192) != 128)
        return 0;
      value = (value << 6) | ((unsigned char)s[i++] & 63);
    }
    if (value < min || value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu))
      return 0;
  }
  return 1;
}
int sl_www_emit(sl_www_write_t write, void *context, const char *type,
                const char *seq, const sl_www_field_t *fields, size_t count) {
  json_t j;
  size_t i, total = 0;
  int result;
  if (!write || !identifier(type) || count > SL_WWW_FIELDS_MAX ||
      (count && !fields) ||
      (seq && (strlen(seq) > 64 || !utf8(seq, strlen(seq)))))
    return SL_ERROR_INVALID;
  for (i = 0; i < count; ++i) {
    size_t n = fields[i].value ? (fields[i].length ? fields[i].length
                                                   : strlen(fields[i].value))
                               : 0;
    size_t k;
    if ((!fields[i].value && fields[i].length) || !identifier(fields[i].name) ||
        !strcmp(fields[i].name, "type") || !strcmp(fields[i].name, "seq") ||
        n > SL_WWW_CHUNK_MAX - total ||
        (fields[i].value && !utf8(fields[i].value, n)))
      return SL_ERROR_INVALID;
    for (k = 0; k < i; ++k)
      if (!strcmp(fields[k].name, fields[i].name))
        return SL_ERROR_INVALID;
    total += n;
  }
  memset(&j, 0, sizeof(j));
  j.capacity = 6 * total + 4096;
  j.data = malloc(j.capacity);
  if (!j.data)
    return SL_ERROR_NOMEM;
  append(&j, "{\"type\":", 8);
  quoted(&j, type, strlen(type));
  if (seq) {
    append(&j, ",\"seq\":", 7);
    quoted(&j, seq, strlen(seq));
  }
  for (i = 0; i < count; ++i) {
    const char *value = fields[i].value ? fields[i].value : "";
    size_t n = fields[i].length ? fields[i].length : strlen(value);
    append(&j, ",", 1);
    quoted(&j, fields[i].name, strlen(fields[i].name));
    append(&j, ":", 1);
    quoted(&j, value, n);
  }
  append(&j, "}", 1);
  result = j.failed ? SL_ERROR_FULL : write(context, j.data, j.length);
  free(j.data);
  return result;
}
static int save_settings(void *context, const void *data, size_t length) {
  www_impl_t *impl = context;
  impl->settings = malloc(length);
  if (!impl->settings)
    return SL_ERROR_NOMEM;
  memcpy(impl->settings, data, length);
  impl->settings_length = length;
  return SL_OK;
}
void sl_www_config_init(sl_www_config_t *config) {
  if (!config)
    return;
  memset(config, 0, sizeof(*config));
  config->title = "softline";
  config->welcome = "Welcome to softline";
  config->queue_display_delay_ms = 500;
  config->image_max_bytes = 32u * 1024u * 1024u;
}
static int asset(sl_www_t *self, const char *path, sl_www_asset_t *out) {
  www_impl_t *impl = self ? self->impl : NULL;
  size_t i;
  if (!impl || !path || !out || !impl->config.enabled)
    return SL_ERROR_INVALID;
  if (!strcmp(path, "/config.json")) {
    out->path = path;
    out->content_type = "application/json; charset=utf-8";
    out->data = (const unsigned char *)impl->settings;
    out->length = impl->settings_length;
    return SL_OK;
  }
  for (i = 0; i < impl->config.asset_count; ++i)
    if (!strcmp(path, impl->config.assets[i].path)) {
      *out = impl->config.assets[i];
      return SL_OK;
    }
  if (impl->flags & SL_WWW_ANIMATIONS)
    for (i = 0; i < sizeof(www_motion_assets) / sizeof(www_motion_assets[0]);
         ++i)
      if (!strcmp(path, www_motion_assets[i].path)) {
        *out = www_motion_assets[i];
        return SL_OK;
      }
  for (i = 0; i < sizeof(www_assets) / sizeof(www_assets[0]); ++i)
    if (!strcmp(path, www_assets[i].path)) {
      *out = www_assets[i];
      return SL_OK;
    }
  return SL_ERROR_INVALID;
}
static int connect_peer(sl_www_t *self, void *peer, const char *after) {
  www_impl_t *impl = self ? self->impl : NULL;
  int result;
  if (!impl || !impl->config.enabled || impl->callback_active)
    return SL_ERROR_INVALID;
  if (!impl->config.replay)
    return SL_OK;
  impl->callback_active = 1;
  result = impl->config.replay(impl->config.context, peer, after ? after : "");
  impl->callback_active = 0;
  return result;
}
static int hex_value(unsigned char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}
static char *decode(char *s, size_t n) {
  size_t i, used = 0;
  for (i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (c == '%') {
      int high, low;
      if (i + 2 >= n || (high = hex_value((unsigned char)s[i + 1])) < 0 ||
          (low = hex_value((unsigned char)s[i + 2])) < 0)
        return NULL;
      c = (unsigned char)(16 * high + low);
      i += 2;
    } else if (c == '+')
      c = ' ';
    if (!c)
      return NULL;
    s[used++] = (char)c;
  }
  s[used] = 0;
  return s;
}
const char *sl_www_field(const sl_www_request_t *request, const char *name) {
  size_t i;
  if (!request || !name)
    return NULL;
  for (i = 0; i < request->count; ++i)
    if (!strcmp(request->fields[i].name, name))
      return request->fields[i].value;
  return NULL;
}
static int message(sl_www_t *self, void *peer, const void *bytes, size_t length,
                   sl_www_write_t reply, void *reply_context) {
  www_impl_t *impl = self ? self->impl : NULL;
  sl_www_field_t fields[SL_WWW_FIELDS_MAX];
  sl_www_request_t request;
  char *copy, *at, *end;
  size_t count = 0, i;
  int result = SL_ERROR_INVALID;
  if (!impl || !impl->config.enabled || !impl->config.action ||
      impl->callback_active || !bytes || !length ||
      length > SL_WWW_REQUEST_MAX || !reply)
    return SL_ERROR_INVALID;
  copy = malloc(length + 1);
  if (!copy)
    return SL_ERROR_NOMEM;
  memcpy(copy, bytes, length);
  copy[length] = 0;
  if (memchr(copy, 0, length))
    goto done;
  at = copy;
  end = copy + length;
  while (at < end) {
    char *next = memchr(at, '&', (size_t)(end - at));
    char *equals;
    if (!next)
      next = end;
    equals = memchr(at, '=', (size_t)(next - at));
    if (!equals || count == SL_WWW_FIELDS_MAX)
      goto done;
    fields[count].name = decode(at, (size_t)(equals - at));
    fields[count].value = decode(equals + 1, (size_t)(next - equals - 1));
    if (!fields[count].value || !identifier(fields[count].name))
      goto done;
    for (i = 0; i < count; ++i)
      if (!strcmp(fields[i].name, fields[count].name))
        goto done;
    fields[count].length = strlen(fields[count].value);
    if (fields[count].length > 128u * 1024u ||
        !utf8(fields[count].value, fields[count].length))
      goto done;
    ++count;
    at = next + 1;
  }
  request.fields = fields;
  request.count = count;
  request.peer = peer;
  request.reply = reply;
  request.reply_context = reply_context;
  if (!sl_www_field(&request, "action"))
    goto done;
  impl->callback_active = 1;
  result = impl->config.action(impl->config.context, &request);
  impl->callback_active = 0;
done:
  free(copy);
  return result;
}
static void destroy(sl_www_t *self) {
  www_impl_t *impl = self ? self->impl : NULL;
  if (!impl || impl->callback_active)
    return;
  free(impl->settings);
  free(impl);
  free(self);
}
static int binary(sl_www_t *self, void *peer, const void *data, size_t length,
                  sl_www_write_t reply, void *reply_context) {
  www_impl_t *impl = self ? self->impl : NULL;
  sl_www_request_t request;
  int result;
  if (!impl || !impl->config.enabled || !impl->config.binary ||
      impl->callback_active || !data || !length || length > SL_WWW_CHUNK_MAX ||
      !reply)
    return SL_ERROR_INVALID;
  memset(&request, 0, sizeof(request));
  request.peer = peer;
  request.reply = reply;
  request.reply_context = reply_context;
  impl->callback_active = 1;
  result = impl->config.binary(impl->config.context, &request, data, length);
  impl->callback_active = 0;
  return result;
}
static int
simulate_prompt_frames(sl_www_t *self, sl_www_write_t write, void *target,
                       const char *id, const char *text,
                       const sl_prompt_simulation_options_t *options) {
  static const sl_prompt_simulation_options_t defaults = {0};
  sl_www_field_t timing[] = {{"start", NULL, 0},
                             {"pace", NULL, 0},
                             {"submit", NULL, 0},
                             {"mode", NULL, 0},
                             {"id", NULL, 0}};
  sl_www_field_t chunk[] = {{"text", NULL, 0}, {"id", NULL, 0}};
  char start[16], pace[16], submit[16];
  size_t length, offset, id_length = id ? strlen(id) : 0;
  int result;
  www_impl_t *impl = self ? self->impl : NULL;
  if (!options)
    options = &defaults;
  if (!impl || !impl->config.enabled || !write || !text ||
      !text[strspn(text, " \t\n")] ||
      !sl_prompt_simulation_options_valid(options))
    return SL_ERROR_INVALID;
  if (impl->callback_active)
    return SL_ERROR_NOT_READY;
  length = strlen(text);
  if (length > 128u * 1024u)
    return SL_ERROR_FULL;
  if (!sl_quoted_prompt_text_valid(text) || !utf8(text, length))
    return SL_ERROR_INVALID;
  (void)snprintf(start, sizeof(start), "%lu", options->start_delay_ms);
  (void)snprintf(pace, sizeof(pace), "%lu", options->character_delay_ms);
  (void)snprintf(submit, sizeof(submit), "%lu", options->submit_delay_ms);
  timing[0].value = start;
  timing[1].value = pace;
  timing[2].value = submit;
  timing[3].value = options->mode == SL_PROMPT_QUEUE_MODE_STEER ? "s" : "q";
  timing[4].value = chunk[1].value = id;
  timing[4].length = chunk[1].length = id_length;
  impl->callback_active = 1;
  result =
      sl_www_emit(write, target, "simulate_begin", NULL, timing, id ? 5u : 4u);
  for (offset = 0; result == SL_OK && offset < length;
       offset += chunk[0].length) {
    size_t end = offset + SL_WWW_CHUNK_MAX - id_length;
    if (end > length)
      end = length;
    while (end < length && ((unsigned char)text[end] & 192u) == 128u)
      --end;
    chunk[0].value = text + offset;
    chunk[0].length = end - offset;
    result =
        sl_www_emit(write, target, "simulate_text", NULL, chunk, id ? 2u : 1u);
  }
  if (result == SL_OK)
    result = sl_www_emit(write, target, "simulate_end", NULL, timing + 4,
                         id ? 1u : 0u);
  impl->callback_active = 0;
  return result;
}

static int simulate_prompt(sl_www_t *self, sl_www_write_t write, void *target,
                           const char *text,
                           const sl_prompt_simulation_options_t *options) {
  return simulate_prompt_frames(self, write, target, NULL, text, options);
}

sl_www_t *sl_www_create_with_flags(const sl_www_config_t *config,
                                   unsigned flags) {
  sl_www_t *self;
  www_impl_t *impl;
  sl_www_config_t defaults;
  sl_www_field_t fields[7];
  char delay[32], limit[32];
  size_t i;
  if (!config) {
    sl_www_config_init(&defaults);
    config = &defaults;
  }
  if ((flags & ~SL_WWW_ANIMATIONS) ||
      (config->asset_count && !config->assets) ||
      config->queue_display_delay_ms < 0)
    return NULL;
  for (i = 0; i < config->asset_count; ++i) {
    const sl_www_asset_t *a = config->assets + i;
    if (!a->path || a->path[0] != '/' || !a->content_type ||
        !*a->content_type || strchr(a->content_type, '\r') ||
        strchr(a->content_type, '\n') || (!a->data && a->length))
      return NULL;
  }
  self = calloc(1, sizeof(*self));
  impl = calloc(1, sizeof(*impl));
  if (!self || !impl) {
    free(self);
    free(impl);
    return NULL;
  }
  self->impl = impl;
  impl->config = *config;
  impl->flags = flags;
  self->asset = asset;
  self->message = message;
  self->connect = connect_peer;
  self->binary = binary;
  self->destroy = destroy;
  self->simulate_prompt = simulate_prompt;
  memset(fields, 0, sizeof(fields));
  fields[0].name = "title";
  fields[0].value = config->title ? config->title : "softline";
  fields[1].name = "welcome";
  fields[1].value = config->welcome ? config->welcome : "Welcome to softline";
  fields[2].name = "banner";
  fields[2].value = config->banner;
  (void)snprintf(delay, sizeof(delay), "%d", config->queue_display_delay_ms);
  (void)snprintf(limit, sizeof(limit), "%lu",
                 (unsigned long)(config->image_max_bytes
                                     ? config->image_max_bytes
                                     : 32u * 1024u * 1024u));
  fields[3].name = "queue_delay";
  fields[3].value = delay;
  fields[4].name = "image_bytes";
  fields[4].value = limit;
  fields[5].name = "images";
  fields[5].value = config->binary ? "1" : "0";
  fields[6].name = "header";
  fields[6].value = config->header;
  if (sl_www_emit(save_settings, impl, "config", NULL, fields, 7) != SL_OK) {
    destroy(self);
    return NULL;
  }
  return self;
}
sl_www_t *sl_www_create(const sl_www_config_t *config) {
  return sl_www_create_with_flags(config, 0);
}
void sl_www_destroy(sl_www_t *s) {
  if (s && s->destroy)
    s->destroy(s);
}
int sl_www_asset(sl_www_t *s, const char *p, sl_www_asset_t *a) {
  return s && s->asset ? s->asset(s, p, a) : SL_ERROR_INVALID;
}
int sl_www_connect(sl_www_t *s, void *p, const char *after) {
  return s && s->connect ? s->connect(s, p, after) : SL_ERROR_INVALID;
}
int sl_www_message(sl_www_t *s, void *p, const void *data, size_t n,
                   sl_www_write_t write, void *context) {
  return s && s->message ? s->message(s, p, data, n, write, context)
                         : SL_ERROR_INVALID;
}
int sl_www_binary(sl_www_t *s, void *p, const void *data, size_t n,
                  sl_www_write_t write, void *context) {
  return s && s->binary ? s->binary(s, p, data, n, write, context)
                        : SL_ERROR_INVALID;
}

int sl_www_simulate_prompt(sl_www_t *self, sl_www_write_t write, void *target,
                           const char *text,
                           const sl_prompt_simulation_options_t *options) {
  return self && self->simulate_prompt
             ? self->simulate_prompt(self, write, target, text, options)
             : SL_ERROR_INVALID;
}

int sl_www_simulate_prompt_tracked(sl_www_t *self, sl_www_write_t write,
                                   void *target, const char *id,
                                   const char *text,
                                   const sl_prompt_simulation_options_t *opts) {
  if (!identifier(id))
    return SL_ERROR_INVALID;
  return simulate_prompt_frames(self, write, target, id, text, opts);
}

int sl_www_cancel_prompt_simulation(sl_www_t *self, sl_www_write_t write,
                                    void *target, const char *id) {
  www_impl_t *impl = self ? self->impl : NULL;
  sl_www_field_t field = {"id", NULL, 0};
  int result;
  if (!impl || !impl->config.enabled || !write || !identifier(id))
    return SL_ERROR_INVALID;
  if (impl->callback_active)
    return SL_ERROR_NOT_READY;
  field.value = id;
  impl->callback_active = 1;
  result = sl_www_emit(write, target, "simulate_cancel", NULL, &field, 1);
  impl->callback_active = 0;
  return result;
}
