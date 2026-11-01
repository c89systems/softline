#include "queue_demo_client.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void interrupted(int signal_number) { (void)signal_number; }
static int reconnect_key(sl_t *sl, sl_key_t key, void *context,
                         sl_key_action_t *action) {
  (void)sl;
  (void)key;
  *action = SL_KEY_ACTION_HANDLED;
  return demo_client_reconnect(context);
}
static int theme(sl_t *sl) {
  static const char *const names[] = {
      "default",    "plain",     "accent", "dracula", "gruvbox",
      "monochrome", "monogreen", "outrun", "riced",   "synthwave"};
  static const sl_prompt_theme_t themes[] = {
      SL_PROMPT_THEME_DEFAULT,   SL_PROMPT_THEME_PLAIN,
      SL_PROMPT_THEME_ACCENT,    SL_PROMPT_THEME_DRACULA,
      SL_PROMPT_THEME_GRUVBOX,   SL_PROMPT_THEME_MONOCHROME,
      SL_PROMPT_THEME_MONOGREEN, SL_PROMPT_THEME_OUTRUN,
      SL_PROMPT_THEME_RICED,     SL_PROMPT_THEME_SYNTHWAVE};
  const char *name = getenv("SOFTLINE_PROMPT_THEME");
  size_t i;
  if (!name || !*name)
    name = "gruvbox";
  for (i = 0; i < sizeof(themes) / sizeof(themes[0]); ++i)
    if (!strcmp(name, names[i]))
      return sl_set_prompt_theme(sl, themes[i]);
  fprintf(stderr, "invalid SOFTLINE_PROMPT_THEME: %s\n", name);
  return SL_ERROR_INVALID;
}
int main(int argc, char **argv) {
  static const char *const status[] = {
      "TCP queue demo", "owner: separate process", "Alt-R reconnect"};
  demo_client_t *client;
  sl_config_t config;
  sl_t *sl;
  int result = 0;
  long port;
  char *end;
  if (argc != 3) {
    fprintf(stderr, "usage: %s 127.0.0.1 PORT\n", argv[0]);
    return 1;
  }
  port = strtol(argv[2], &end, 10);
  if (!*argv[2] || *end || port < 1 || port > 65535) {
    fprintf(stderr, "invalid queue port\n");
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, interrupted);
  client = demo_client_create(argv[1], (unsigned short)port);
  if (!client) {
    fprintf(stderr,
            "queue adapter requires numeric 127.0.0.1 and a valid port\n");
    return 1;
  }
  sl_config_init(&config);
  config.prompt_queue = 1;
  config.prompt_queue_max_entries = 64;
  config.prompt_queue_preview_entries = 3;
  config.queue_backend = demo_client_backend(client);
  config.queue_backend_context = client;
  sl = sl_create_with_config(&config);
  if (!sl) {
    demo_client_destroy(client);
    fprintf(stderr, "cannot create editor\n");
    return 1;
  }
  if (sl_set_prompt_queue_profile(sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) !=
          SL_OK ||
      sl_set_statusline(sl, 1, 0) != SL_OK ||
      sl_set_status_elements(sl, status, 3) != SL_OK || theme(sl) != SL_OK ||
      demo_client_attach(client, sl) != SL_OK ||
      sl_bind_key(sl, SL_KEY_ALT_BASE + 'r', reconnect_key, client) != SL_OK ||
      sl_output_stream_begin(sl) != SL_OK) {
    result = 1;
    goto done;
  }
  (void)sl_output_stream_write(
      sl,
      "Remote queue demo. Enter queues, Alt-Enter steers, Alt-E edits.\n"
      "Commands: /status /stop /resume /approve [on|off] /inject.\n"
      "Alt-R reconnects and resolves original request IDs. Ctrl-D on an empty "
      "draft detaches.\n",
      strlen("Remote queue demo. Enter queues, Alt-Enter steers, Alt-E edits.\n"
             "Commands: /status /stop /resume /approve [on|off] /inject.\n"
             "Alt-R reconnects and resolves original request IDs. Ctrl-D on an "
             "empty draft detaches.\n"));
  for (;;) {
    char *line = sl_next_prompt(sl, NULL, NULL);
    sl_readline_status_t status_code = sl_last_readline_status(sl);
    if (line) {
      sl_free_string(sl, line);
      fprintf(stderr, "unexpected remote prompt delivery\n");
      result = 1;
      break;
    }
    if (status_code == SL_READLINE_INTERRUPTED) {
      (void)demo_client_cancel(client);
      continue;
    }
    if (status_code == SL_READLINE_CANCELLED)
      continue;
    if (status_code == SL_READLINE_ERROR) {
      fprintf(stderr, "remote editor: %s\n", sl_last_error(sl));
      result = 1;
    }
    break;
  }
done:
  (void)sl_output_stream_end(sl);
  sl_destroy(sl);
  demo_client_destroy(client);
  return result;
}
