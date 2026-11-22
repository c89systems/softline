#include "softline/softline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct upload {
  sl_image_paste_id_t id;
  unsigned long hash;
  size_t bytes;
  int ended, aborted, stage;
};
static int upload_chunk(void *context, const sl_image_paste_event_t *event) {
  struct upload *u = context;
  size_t i;
  u->id = event->id;
  if (event->operation == SL_IMAGE_PASTE_DATA) {
    if (event->offset != u->bytes)
      return SL_ERROR_INVALID;
    for (i = 0; i < event->length; ++i)
      u->hash = ((u->hash ^ event->data[i]) * 16777619UL) & 0xffffffffUL;
    u->bytes += event->length;
  } else if (event->operation == SL_IMAGE_PASTE_END)
    u->ended = 1;
  else if (event->operation == SL_IMAGE_PASTE_ABORT)
    u->aborted = 1;
  return SL_OK;
}
static void upload_ack(sl_t *sl, void *context) {
  struct upload *u = context;
  if (u->aborted) {
    (void)sl_submit(sl);
    return;
  }
  if (!u->ended)
    return;
  if (!u->stage) {
    if (strstr(sl_buffer(sl), "/backend/"))
      _exit(3);
    if (sl_image_paste_complete(sl, u->id, "/backend/image", NULL) != SL_OK)
      _exit(3);
    u->stage = 1;
  } else if (strstr(sl_buffer(sl), "/backend/image"))
    (void)sl_submit(sl);
}

static int override_paste(sl_t *sl, sl_key_t key, void *userdata,
                          sl_key_action_t *action) {
  (void)key;
  (void)userdata;
  if (sl_insert(sl, "OVERRIDE") != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int write_all(int fd, const char *bytes, size_t length) {
  while (length > 0) {
    ssize_t written = write(fd, bytes, length);
    if (written <= 0)
      return -1;
    bytes += written;
    length -= (size_t)written;
  }
  return 0;
}

int main(int argc, char **argv) {
  sl_config_t config;
  sl_t *sl;
  char *line;
  const char *error;
  int result_fd;
  int adapter = argc == 3 && !strncmp(argv[2], "adapter", 7);
  struct upload upload;
  char stats[80];
  memset(&upload, 0, sizeof(upload));
  upload.hash = 2166136261UL;
  if (argc < 2 || argc > 3)
    return 2;
  if (argc == 3 && !strcmp(argv[2], "lua-adapter")) {
    const char *lua = getenv("SOFTLINE_TEST_LUA");
    const char *script = getenv("SOFTLINE_TEST_LUA_IMAGE_SCRIPT");
    if (!lua || !script)
      return 2;
    execl(lua, lua, script, argv[1], (char *)NULL);
    return 2;
  }
  result_fd = atoi(argv[1]);
  sl_config_init(&config);
  if (adapter) {
    config.image_paste_callback = upload_chunk;
    config.image_paste_context = &upload;
    if (!strcmp(argv[2], "adapter-large"))
      config.image_paste_max_bytes = 32u * 1024u * 1024u;
    else if (!strcmp(argv[2], "adapter-wide"))
      config.image_paste_max_bytes = 64u * 1024u * 1024u;
  }
  if (argc >= 3 && strcmp(argv[2], "limit") == 0)
    config.line_max_len = 16;
  config.image_paste_path_template = getenv("SOFTLINE_TEST_IMAGE_TEMPLATE");
  if (getenv("SOFTLINE_TEST_DISABLE_IMAGE_PASTE"))
    config.disable_image_paste = 1;
  sl = sl_create_with_config(&config);
  if (!sl)
    return 2;
  if (adapter && sl_set_idle_callback(sl, upload_ack, &upload) != SL_OK)
    return 2;
  if (argc >= 3 && strcmp(argv[2], "setter") == 0 &&
      sl_set_image_paste_path_template(
          sl, getenv("SOFTLINE_TEST_IMAGE_SETTER")) != SL_OK)
    return 2;
  if (argc >= 3 && strcmp(argv[2], "override") == 0 &&
      sl_bind_key(sl, SL_KEY_CTRL_V, override_paste, NULL) != SL_OK)
    return 2;
  line = sl_readline(sl, "> ");
  if (!line)
    return 2;
  error = sl_last_error(sl);
  if (adapter && !upload.aborted) {
    (void)snprintf(stats, sizeof(stats), "%lu:%lu", (unsigned long)upload.bytes,
                   upload.hash);
    error = stats;
  }
  if (write_all(result_fd, line, strlen(line)) != 0 ||
      write_all(result_fd, "\n", 1) != 0 ||
      write_all(result_fd, error ? error : "", error ? strlen(error) : 0) !=
          0 ||
      write_all(result_fd, "\n", 1) != 0)
    return 2;
  sl_free_string(sl, line);
  sl_destroy(sl);
  return 0;
}
