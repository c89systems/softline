#include <softline/softline.h>
#include <stdio.h>
#include <string.h>

static int inspect(sl_t *sl, sl_key_t key, void *data,
                   sl_key_action_t *action) {
  const unsigned char *text = (const unsigned char *)sl_buffer(sl);
  unsigned long sum = 0;
  size_t length = strlen((const char *)text), i;
  (void)key;
  (void)data;
  for (i = 0; i < length; ++i)
    sum = (sum + text[i]) % 65521;
  printf("\033]2;draft-%lu-%lu-%lu\007", (unsigned long)length,
         (unsigned long)sl_cursor(sl), sum);
  *action = SL_KEY_ACTION_HANDLED;
  return fflush(stdout) ? SL_ERROR_IO : SL_OK;
}

static int producer(sl_t *sl, void *data, const char **chunk, size_t *length) {
  int *sent = data;
  (void)sl;
  *chunk = *sent ? NULL : "producer marker\n";
  *length = *sent ? 0 : 16;
  *sent = 1;
  return SL_OK;
}
static int viewport_key(sl_t *sl, sl_key_t key, void *data,
                        sl_key_action_t *action) {
  int sent = 0;
  (void)data;
  *action = SL_KEY_ACTION_HANDLED;
  if (key == SL_KEY_ALT_BASE + 'g')
    return sl_set_cursor(sl, 0);
  if (key == SL_KEY_ALT_BASE + 'h')
    return sl_set_cursor(sl, strlen(sl_buffer(sl)));
  if (*(int *)data)
    return sl_output_stream_write(sl, "producer marker\n", 16);
  return sl_print_above(sl, producer, &sent);
}

/* A public-API editor with a render barrier. The title is outside the cell
 * transcript and follows a synchronous redraw of the preceding input/resize. */
static int barrier(sl_t *sl, sl_key_t key, void *data,
                   sl_key_action_t *action) {
  unsigned long *serial = data;
  (void)key;
  if (sl_set_status_message(sl, NULL) != SL_OK)
    return SL_ERROR_IO;
  printf("\033]2;rendered-%lu\007", ++*serial);
  if (fflush(stdout))
    return SL_ERROR_IO;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int theme(sl_t *sl, sl_key_t key, void *data, sl_key_action_t *action) {
  (void)data;
  *action = SL_KEY_ACTION_HANDLED;
  return sl_set_prompt_theme(sl, (sl_prompt_theme_t)(key - SL_KEY_F1));
}

int main(int argc, char **argv) {
  sl_t *sl = sl_create();
  unsigned long serial = 0;
  int index;
  char *line;
  if (!sl || sl_set_prompt_theme(sl, SL_PROMPT_THEME_PLAIN) != SL_OK ||
      sl_bind_key(sl, SL_KEY_ALT_M, barrier, &serial) != SL_OK)
    return 1;
  if (argc == 2 && !strcmp(argv[1], "--completion")) {
    sl_completion_t item = {"hello world and a very long suggestion",
                            SL_COMPLETION_START};
    if (sl_set_completions(sl, &item, 1) != SL_OK)
      return 1;
    line = sl_readline(sl, "softline> ");
    if (!line)
      return 1;
    printf("submitted: %s\nHOST_AFTER\n", line);
    sl_free_string(sl, line);
    sl_destroy(sl);
    return 0;
  }
  if (argc == 2 && (!strcmp(argv[1], "--viewport") ||
                    !strcmp(argv[1], "--viewport-native"))) {
    int native = !strcmp(argv[1], "--viewport-native");
    if (sl_bind_key(sl, SL_KEY_ALT_BASE + 't', inspect, NULL) != SL_OK ||
        sl_bind_key(sl, SL_KEY_ALT_BASE + 'g', viewport_key, NULL) != SL_OK ||
        sl_bind_key(sl, SL_KEY_ALT_BASE + 'h', viewport_key, NULL) != SL_OK ||
        sl_bind_key(sl, SL_KEY_ALT_BASE + 'o', viewport_key, &native) != SL_OK)
      return 1;
    if (native && (sl_set_statusline(sl, 1, 0) != SL_OK ||
                   sl_set_status_element(sl, 0, "viewport status") != SL_OK ||
                   sl_output_stream_begin(sl) != SL_OK))
      return 1;
    line = native ? sl_next_prompt(sl, "softline> ", NULL)
                  : sl_readline(sl, "softline> ");
    if (native && sl_output_stream_end(sl) != SL_OK)
      return 1;
    if (!line)
      return 1;
    printf("submitted-length: %lu\n", (unsigned long)strlen(line));
    sl_free_string(sl, line);
    sl_destroy(sl);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--themes")) {
    for (index = 0; index <= SL_PROMPT_THEME_DEFAULT; ++index)
      if (sl_bind_key(sl, SL_KEY_F1 + index, theme, NULL) != SL_OK)
        return 1;
    if (sl_set_prompt_theme(sl, SL_PROMPT_THEME_DEFAULT) != SL_OK ||
        sl_set_statusline(sl, 1, 0) != SL_OK ||
        sl_set_status_element(sl, 0, "theme probe") != SL_OK ||
        sl_output_stream_begin(sl) != SL_OK)
      return 1;
    line = sl_next_prompt(sl, "> ", NULL);
    if (!line || sl_output_stream_end(sl) != SL_OK)
      return 1;
    printf("submitted: %s\n", line);
    sl_free_string(sl, line);
    sl_destroy(sl);
    return 0;
  }
  while ((line = sl_readline(sl, "softline> ")) != NULL) {
    printf("submitted: %s\n", line);
    sl_free_string(sl, line);
  }
  sl_destroy(sl);
  return 0;
}
