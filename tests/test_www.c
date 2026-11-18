#include <softline/www.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "www line %d: %s\n", __LINE__, #c);                      \
      return 1;                                                                \
    }                                                                          \
  } while (0)
typedef struct {
  sl_www_t *web;
  int calls, replays, binaries, fail, reentrant;
  char *output;
  size_t length;
} host_t;
static int write_frame(void *context, const void *data, size_t length) {
  host_t *h = context;
  if (h->fail)
    return SL_ERROR_NOT_READY;
  free(h->output);
  h->output = malloc(length + 1);
  if (!h->output)
    return SL_ERROR_NOMEM;
  memcpy(h->output, data, length);
  h->output[length] = 0;
  h->length = length;
  return SL_OK;
}
static int action(void *context, const sl_www_request_t *request) {
  host_t *h = context;
  sl_www_field_t field;
  const char *text = sl_www_field(request, "text");
  if (!text || strcmp(text, "a+b & \342\202\254\n") ||
      strcmp(sl_www_field(request, "action"), "send") || request->peer != h)
    return SL_ERROR;
  ++h->calls;
  h->reentrant = sl_www_connect(h->web, h, "x");
  sl_www_destroy(h->web); /* Callback reentry must not destroy the handler. */
  field.name = "text";
  field.value = text;
  field.length = 0;
  return sl_www_emit(request->reply, request->reply_context, "ack", NULL,
                     &field, 1);
}
static int replay(void *context, void *peer, const char *after) {
  host_t *h = context;
  if (peer != h || strcmp(after, "42"))
    return SL_ERROR;
  ++h->replays;
  return SL_OK;
}
static int binary(void *context, const sl_www_request_t *request,
                  const void *bytes, size_t length) {
  host_t *h = context;
  static const unsigned char expected[] = {0, 255, 1, 0};
  if (request->peer != h || length != sizeof(expected) ||
      memcmp(bytes, expected, length))
    return SL_ERROR;
  ++h->binaries;
  return SL_OK;
}
int main(void) {
  static const char request[] = "action=send&text=a%2Bb+%26+%E2%82%AC%0A";
  static const char *bad[] = {"",
                              "action",
                              "action=%",
                              "action=%0",
                              "action=%zz",
                              "action=send&action=cancel",
                              "action=send&text=%00",
                              "bad%00name=x",
                              "action=send&text=%C0%80",
                              "action=send&text=%ED%A0%80",
                              "action=send&text=%F4%90%80%80",
                              "action=send&text=%E2%82",
                              "text=none"};
  static const unsigned char bytes[] = {0, 255, 1, 0};
  sl_www_config_t config;
  sl_www_asset_t asset;
  sl_www_field_t fields[2];
  char header[] = "<a href=\"./\"><svg aria-label='custom header'></svg></a>";
  host_t h;
  size_t i;
  memset(&h, 0, sizeof(h));
  sl_www_config_init(&config);
  CHECK(!config.enabled && !config.header &&
        !strcmp(config.welcome, "Welcome to softline"));
  h.web = sl_www_create(&config);
  CHECK(h.web && h.web->asset(h.web, "/", &asset) == SL_ERROR_INVALID);
  sl_www_destroy(h.web);
  {
    sl_www_asset_t custom;
    custom.path = "/banner.svg";
    custom.content_type = "image/svg+xml";
    custom.data = (const unsigned char *)"<svg/>";
    custom.length = 6;
    config.enabled = 1;
    config.assets = &custom;
    config.asset_count = 1;
    h.web = sl_www_create(&config);
    CHECK(h.web && sl_www_asset(h.web, "/banner.svg", &asset) == SL_OK &&
          asset.length == 6);
    CHECK(sl_www_asset(h.web, "/config.json", &asset) == SL_OK &&
          write_frame(&h, asset.data, asset.length) == SL_OK &&
          strstr(h.output, "\"header\":\"\""));
    sl_www_destroy(h.web);
    custom.content_type = "image/svg+xml\r\nInjected: value";
    CHECK(!sl_www_create(&config));
    config.assets = NULL;
    config.asset_count = 0;
  }
  config.enabled = 1;
  config.action = action;
  config.binary = binary;
  config.replay = replay;
  config.context = &h;
  config.title = "Custom \"title\"";
  config.banner = "<svg aria-label='host banner'></svg>";
  config.header = header;
  h.web = sl_www_create(&config);
  /* The receiver owns configuration bytes after construction. */
  header[1] = 'X';
  CHECK(h.web && sl_www_asset(h.web, "/", &asset) == SL_OK &&
        asset.length > 100 && !memcmp(asset.data, "<!doctype html>", 15));
  CHECK(sl_www_asset(h.web, "/JetBrainsMono-Regular.woff2", &asset) == SL_OK &&
        asset.length > 10000);
  CHECK(sl_www_asset(h.web, "/smd.js", &asset) == SL_OK && asset.length > 1000);
  CHECK(sl_www_asset(h.web, "/config.json", &asset) == SL_OK && asset.length &&
        memchr(asset.data, '\\', asset.length));
  CHECK(
      write_frame(&h, asset.data, asset.length) == SL_OK &&
      strstr(h.output, "\"header\":\"<a href=\\\"./\\\"><svg "
                       "aria-label='custom header'></svg></a>\"") &&
      strstr(h.output, "\"banner\":\"<svg aria-label='host banner'></svg>\""));
  CHECK(sl_www_asset(h.web, "/../LICENSE", &asset) == SL_ERROR_INVALID);
  CHECK(sl_www_connect(h.web, &h, "42") == SL_OK && h.replays == 1);
  CHECK(h.web->message(h.web, &h, request, sizeof(request) - 1, write_frame,
                       &h) == SL_OK &&
        h.calls == 1 && h.reentrant == SL_ERROR_INVALID);
  CHECK(h.output && strstr(h.output, "\"type\":\"ack\"") &&
        strstr(h.output, "\\u000a"));
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    CHECK(sl_www_message(h.web, &h, bad[i], strlen(bad[i]), write_frame, &h) ==
          SL_ERROR_INVALID);
  CHECK(sl_www_binary(h.web, &h, bytes, sizeof(bytes), write_frame, &h) ==
            SL_OK &&
        h.binaries == 1);
  CHECK(sl_www_binary(h.web, &h, bytes, SL_WWW_CHUNK_MAX + 1, write_frame,
                      &h) == SL_ERROR_INVALID);
  memset(fields, 0, sizeof(fields));
  fields[0].name = "text";
  fields[0].value = "x\"\\\n";
  CHECK(
      sl_www_emit(write_frame, &h, "delta", "1", fields, 1) == SL_OK &&
      !strcmp(
          h.output,
          "{\"type\":\"delta\",\"seq\":\"1\",\"text\":\"x\\\"\\\\\\u000a\"}"));
  fields[1] = fields[0];
  CHECK(sl_www_emit(write_frame, &h, "delta", NULL, fields, 2) ==
        SL_ERROR_INVALID);
  fields[0].name = "type";
  CHECK(sl_www_emit(write_frame, &h, "delta", NULL, fields, 1) ==
        SL_ERROR_INVALID);
  fields[0].name = "text";
  fields[0].value = NULL;
  fields[0].length = 1;
  CHECK(sl_www_emit(write_frame, &h, "delta", NULL, fields, 1) ==
        SL_ERROR_INVALID);
  fields[0].value = "x";
  fields[0].length = 0;
  fields[0].value = "\300\200";
  CHECK(sl_www_emit(write_frame, &h, "delta", NULL, fields, 1) ==
        SL_ERROR_INVALID);
  fields[0].value = "x";
  h.fail = 1;
  CHECK(sl_www_emit(write_frame, &h, "delta", NULL, fields, 1) ==
        SL_ERROR_NOT_READY);
  sl_www_destroy(h.web);
  free(h.output);
  {
    sl_config_t editor_config;
    sl_t *editor;
    sl_prompt_queue_entry_info_t info;
    int stopped;
    sl_config_init(&editor_config);
    editor_config.prompt_queue = 1;
    editor = sl_create_with_config(&editor_config);
    CHECK(editor && sl_set_buffer(editor, "independent draft") == SL_OK);
    CHECK(sl_set_prompt_queue_profile(
              editor, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK);
    CHECK(sl_prompt_queue_append(editor, "first", 0, NULL) == SL_OK);
    CHECK(editor->prompt_queue_stop(editor) == SL_OK);
    CHECK(sl_get_prompt_queue_stopped(editor, &stopped) == SL_OK && stopped);
    CHECK(!strcmp(sl_buffer(editor), "independent draft"));
    CHECK(sl_prompt_queue_info(editor, 0, &info) == SL_OK && info.stopped &&
          !info.eligible);
    CHECK(sl_prompt_queue_append(editor, "second", 0, NULL) == SL_OK);
    CHECK(sl_prompt_queue_info(editor, 1, &info) == SL_OK && info.stopped);
    CHECK(sl_prompt_queue_resume(editor) == SL_OK);
    CHECK(sl_prompt_queue_stop(editor) == SL_OK);
    sl_destroy(editor);
  }
  return 0;
}
