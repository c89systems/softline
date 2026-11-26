#define _POSIX_C_SOURCE 200809L
#include "softline/softline.h"
#include <string.h>
#include <unistd.h>

struct producer {
  const char *text;
  int sent;
};

static int chunk(sl_t *sl, void *data, const char **bytes, size_t *length) {
  struct producer *producer = data;
  (void)sl;
  *bytes = producer->text;
  *length = producer->sent++ ? 0 : strlen(*bytes);
  return SL_OK;
}

static void feed(sl_t *sl, void *data) {
  int *turn = data;
  struct producer producer;
  if (*turn >= 2)
    return;
  producer.text = *turn ? "more\n" : "one\ntwo\nthree\nfour\n";
  producer.sent = 0;
  (void)write(STDOUT_FILENO, "\033]777;feed-start\007", 17);
  if (sl_print_above(sl, chunk, &producer) != SL_OK)
    _exit(2);
  (void)write(STDOUT_FILENO, "\033]777;feed-end\007", 15);
  if (!*turn && sl_set_live_scroll_region(sl, 1) != SL_OK)
    _exit(2);
  if (++*turn == 2)
    sl_cancel(sl);
}

int main(void) {
  sl_config_t config;
  sl_t *sl;
  char *line;
  int turn = 0, result;
  sl_config_init(&config);
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  sl = sl_create_with_config(&config);
  if (!sl || sl_set_idle_callback(sl, feed, &turn) != SL_OK)
    return 2;
  line = sl_readline(sl, "> ");
  result = line != NULL || turn != 2 ||
           sl_last_readline_status(sl) != SL_READLINE_CANCELLED;
  sl_free_string(sl, line);
  sl_destroy(sl);
  return result;
}
