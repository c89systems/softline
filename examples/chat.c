#define _POSIX_C_SOURCE 200809L

#include "softline/softline.h"
#include "softline/transcript.h"

#include <libmdf/mdf.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "chat_charts.h"
#include "chat_demo_data.h"
#include "completion.h"
#include "queue_demo_client.h"
#include "queue_demo_timer.h"
#include "queue_demo_wire.h"
#include "transcript_demo.h"
#include "web_chat.h"

struct chat_state {
  sl_t *sl;
  mdf *note_renderer;
  mdf *response_renderer;
  int note_documents;
  int response_documents;
  int response_open;
  int interactive;
  int columns;
  int watch_fd;
  sl_watch_id_t watch_id;
  pid_t worker_pid;
  int busy;
  int exit_requested;
  unsigned int turn_index;
  demo_charts_t charts;
  size_t response_bytes;
  char previous_response_char;
  int approval_demo;
  unsigned int approval_interval_ms;
  unsigned int injected;
  double next_injection;
  int demo_failed;
  demo_client_t *remote;
  unsigned int approval_seed;
  int remote_shell, shell_requested;
  int remote_state, reconnect_fd;
  int image_active, image_storing;
  sl_watch_id_t reconnect_watch;
  unsigned int reconnect_ms;
  double connecting_since;
  demo_web_chat_t *web;
  size_t terminal_upstream;
  int web_only, null_fd;
  char web_bytes[64];
  size_t web_length;
  sl_transcript_id_t transcripts[2];
};

static void destroy_remote(struct chat_state *state) {
#ifdef SOFTLINE_CBOR_TCP
  demo_client_destroy(state->remote);
#else
  (void)state;
#endif
}

static int start_operation(struct chat_state *state);
static int dispatch_next_turn(struct chat_state *state);
static int sync_geometry(struct chat_state *state);
static int handle_command(struct chat_state *state, const char *line,
                          int *handled);
static void web_flush(struct chat_state *state) {
  demo_web_response(state->web, state->web_bytes, state->web_length);
  state->web_length = 0;
}
static void web_byte(struct chat_state *state, char byte) {
  if (!state->web)
    return;
  state->web_bytes[state->web_length++] = byte;
  if (state->web_length == sizeof(state->web_bytes))
    web_flush(state);
}

/* Keep the reasoning row separate from queue/approval rows while idle.
 * Actual messages retain the normal status prefix. */
static int set_chat_message(sl_t *sl, const char *message) {
  int rc = sl_set_status_message_prefix(sl, message ? NULL : "");
  return rc == SL_OK ? sl_set_status_message(sl, message ? message : " ") : rc;
}

static int command_ready(const struct chat_state *state, const char *line,
                         sl_prompt_queue_mode_t mode, int at_seam) {
  return is_local_command(line) || !state->busy ||
         (at_seam && mode == SL_PROMPT_QUEUE_MODE_STEER &&
          !is_shell_command(line));
}

static int classify_prompt(sl_t *sl, const char *text,
                           sl_prompt_queue_mode_t mode,
                           sl_prompt_queue_policy_t *policy, void *userdata) {
  struct chat_state *state = userdata;
  (void)sl;
  policy->enqueue = text[0] == '/' || text[0] == '!';
  policy->independent = is_local_command(text);
  policy->executable = command_ready(state, text, mode, 0);
  return SL_OK;
}

/* Readiness follows real operation events. Inspection does not consume an
 * entry, clear cancellation, approve it, or change its destination. */
static int sync_queue_readiness(struct chat_state *state, int at_seam) {
  size_t index;
  sl_prompt_queue_entry_info_t draft;
  if (state->remote)
    return 0;
  if (sl_prompt_queue_draft_info(state->sl, &draft) != SL_OK)
    return -1;
  if (draft.id && sl_prompt_queue_set_executable(
                      state->sl, draft.id,
                      !state->busy || draft.policy.independent) != SL_OK)
    return -1;
  for (index = 0; index < sl_prompt_queue_count(state->sl); index++) {
    sl_prompt_queue_entry_info_t info;
    char *text = NULL;
    int ready;
    if (sl_prompt_queue_info(state->sl, index, &info) != SL_OK ||
        sl_prompt_queue_peek(state->sl, index, &text) != SL_OK)
      return -1;
    ready = command_ready(state, text, info.mode, at_seam);
    sl_free_string(state->sl, text);
    if (sl_prompt_queue_set_executable(state->sl, info.id, ready) != SL_OK)
      return -1;
  }
  return 0;
}

/* Both local and remote sessions use the same Softline PTY relay. */
static int run_shell(struct chat_state *state, int preserve_draft) {
  int status, exit_status = -1;
  char message[128] = {0};
  demo_www_t *web = demo_web_listener(state->web);
  demo_www_set_terminal_handoffs(web, state->terminal_upstream + 1);
  if (web)
    (void)demo_www_poll(web, 0);
#ifdef SOFTLINE_CBOR_TCP
  if (state->remote)
    status = demo_client_terminal(state->remote, state->sl, &exit_status);
  else
#endif
    status = sl_terminal_local(state->sl, NULL, &exit_status);
  demo_www_set_terminal_handoffs(web, state->terminal_upstream);
  if (web)
    (void)demo_www_poll(web, 0);
  if (exit_status > 0 && status != SL_TERMINAL_ESCAPED)
    (void)snprintf(message, sizeof(message),
                   "shell exited unsuccessfully (exit %d)\n", exit_status);
  else if (status < 0)
    (void)snprintf(message, sizeof(message), "shell handoff failed (%d)\n",
                   status);
  if (*message &&
      sl_output_stream_write(state->sl, message, strlen(message)) != SL_OK)
    return -1;
  return sync_geometry(state) == 0 &&
                 (preserve_draft || set_chat_message(state->sl, NULL) == SL_OK)
             ? 0
             : -1;
}

static int sink_to_softline(void *userdata, const char *bytes, size_t length) {
  struct chat_state *state;
  state = (struct chat_state *)userdata;
  if (!state || (!bytes && length > 0))
    return -1;
  if (sl_transcript_visible(state->sl))
    return 0;
  return sl_output_stream_write(state->sl, bytes, length) == SL_OK ? 0 : -1;
}

static int refresh_transcripts(struct chat_state *state) {
  int i;
  for (i = 0; i < 2; ++i) {
    char frame[160];
    if (!state->transcripts[i])
      continue;
    demo_transcript_frame(i, (unsigned long)state->response_documents, frame,
                          sizeof(frame));
    if (sl_transcript_reset(state->sl, state->transcripts[i]) != SL_OK ||
        sl_transcript_write(state->sl, state->transcripts[i],
                            i ? SL_TRANSCRIPT_VT : SL_TRANSCRIPT_TEXT, frame,
                            strlen(frame)) != SL_OK)
      return -1;
  }
  return 0;
}

static int shell_editor_key(sl_t *sl, sl_key_t key, void *userdata,
                            sl_key_action_t *action) {
  struct chat_state *state = (struct chat_state *)userdata;
  (void)key;
#ifdef SOFTLINE_CBOR_TCP
  if (state->remote) {
    int result;
    *action = SL_KEY_ACTION_HANDLED;
    if (state->shell_requested)
      return SL_OK;
    result = demo_client_shell_begin(state->remote);
    if (result == SL_OK)
      state->shell_requested = 1;
    else if (result == SL_ERROR_NOT_READY)
      return set_chat_message(
          sl,
          "Shell is unavailable while disconnected; retrying automatically.");
    return result;
  }
#endif
  *action = state->busy ? SL_KEY_ACTION_HANDLED : SL_KEY_ACTION_SUSPEND;
  return state->busy
             ? set_chat_message(sl, "Shell is available only between turns.")
             : SL_OK;
}

static int chat_margin_left(int columns) { return columns >= 5 ? 2 : 0; }

static int new_renderer(struct chat_state *state, mdf **out) {
  mdf_options options;
  mdf_sink sink;
  sl_osc8_info_t links;
  if (sl_get_osc8_info(state->sl, &links) != SL_OK)
    return -1;
  mdf_options_init(&options);
  options.output_fd = STDOUT_FILENO;
  options.width = state->columns;
  options.margin_left = chat_margin_left(state->columns);
  options.boring = !state->interactive;
  options.osc8 = links.enabled;
  if (mdf_create(MDF_FORMAT_ANSI, &options, out) != MDF_OK)
    return -1;
  sink.userdata = state;
  sink.write = sink_to_softline;
  if ((*out)->set_sink(*out, &sink) != MDF_OK) {
    (*out)->destroy(*out);
    *out = NULL;
    return -1;
  }
  return 0;
}

static int sync_geometry(struct chat_state *state) {
  int columns;
  columns = mdf_terminal_width(STDOUT_FILENO, 80);
  if (columns == state->columns)
    return 0;
  /* The composer updates both handles in one owner-thread callback. A very
   * narrow terminal drops the margin so libmdf retains three content columns.
   */
  if (state->note_renderer->set_geometry(state->note_renderer, columns,
                                         chat_margin_left(columns),
                                         0) != MDF_OK ||
      state->response_renderer->set_geometry(state->response_renderer, columns,
                                             chat_margin_left(columns),
                                             0) != MDF_OK)
    return -1;
  state->columns = columns;
  return 0;
}

static int begin_note_document(struct chat_state *state) {
  if (sync_geometry(state) != 0)
    return -1;
  if (state->note_documents > 0 &&
      state->note_renderer->begin_document(state->note_renderer) != MDF_OK)
    return -1;
  return 0;
}

static int finish_note_document(struct chat_state *state) {
  if (state->note_renderer->finish_document(state->note_renderer) != MDF_OK)
    return -1;
  state->note_documents++;
  return 0;
}

static int render_note(struct chat_state *state, const char *note) {
  web_flush(state);
  demo_web_message(state->web, "note", note);
  if (begin_note_document(state) != 0 ||
      state->note_renderer->feed(state->note_renderer, "\n", 1) != MDF_OK ||
      state->note_renderer->feed(state->note_renderer, note, strlen(note)) !=
          MDF_OK ||
      state->note_renderer->feed(state->note_renderer, "\n\n", 2) != MDF_OK)
    return -1;
  return finish_note_document(state);
}

static int render_user_prompt(struct chat_state *state, const char *line) {
  web_flush(state);
  demo_web_message(state->web, "user", line);
  if (sl_transcript_visible(state->sl))
    return 0;
  return sl_output_stream_write_quoted_prompt(state->sl, line) == SL_OK ? 0
                                                                        : -1;
}

/* The host presents transfers in its existing reasoning row. Transport and
 * acquisition never set status text or allocate a separate upload row. */
static int refresh_chat_activity(struct chat_state *state) {
  const char *message = NULL;
  int active = state->busy || state->image_active;
  if (state->image_active)
    message = state->image_storing ? "Saving image..." : "Uploading image...";
  else if (state->busy)
    message = state->response_bytes >= 15 ? "Reasoning..." : "Thinking...";
  demo_web_status(state->web, message, active);
  return sl_set_status_busy(state->sl, active) == SL_OK &&
                 sl_set_status_spinner(state->sl, active) == SL_OK &&
                 set_chat_message(state->sl, message) == SL_OK
             ? 0
             : -1;
}
static int set_busy(struct chat_state *state, int busy) {
  state->busy = busy;
  if (refresh_chat_activity(state))
    return -1;
  return sync_queue_readiness(state, 0);
}

static int show_goodbye(struct chat_state *state) {
  if (!state->interactive)
    return 0;
  if (sl_set_status_spinner(state->sl, 0) != SL_OK ||
      sl_set_status_busy(state->sl, 0) != SL_OK ||
      set_chat_message(state->sl, "Goodbye.") != SL_OK)
    return -1;
  return sl_output_stream_reconcile(state->sl) == SL_OK ? 0 : -1;
}

static int finish_response_document(struct chat_state *state) {
  web_flush(state);
  demo_web_response_end(state->web);
  if (!state->response_open)
    return 0;
  if (state->response_renderer->finish_document(state->response_renderer) !=
      MDF_OK)
    return -1;
  state->response_open = 0;
  state->response_documents++;
  return refresh_transcripts(state);
}

static int command_message(struct chat_state *state, const char *message) {
  demo_web_status(state->web, message, state->busy);
  return set_chat_message(state->sl, message) == SL_OK &&
                 (sl_transcript_visible(state->sl) ||
                  sl_output_stream_reconcile(state->sl) == SL_OK)
             ? 0
             : -1;
}

static int handle_command(struct chat_state *state, const char *line,
                          int *handled) {
  int enabled, stopped;
  *handled = 1;
  if (command_named(line, "/terminal-lock")) {
    demo_www_t *web = demo_web_listener(state->web);
    size_t active;
    if (!web)
      return command_message(state, "No web chat is served here.");
    demo_www_terminal_status(web, &active, &enabled);
    enabled = terminal_lock_setting(line, enabled);
    if (enabled < 0)
      return command_message(state, "Usage: /terminal-lock [on|off]");
    demo_www_set_terminal_prompt_lock(web, enabled);
    return command_message(state, enabled ? "Terminal prompt lock on."
                                          : "Terminal prompt lock off.");
  }
  if (!strcmp(line, "/status")) {
    char text[256];
    if (sl_get_prompt_queue_stopped(state->sl, &stopped) != SL_OK ||
        sl_get_auto_approval(state->sl, &enabled) != SL_OK)
      return -1;
    (void)snprintf(text, sizeof(text),
                   "Status: %s / backlog %s / %lu queued / auto-approval %s",
                   state->busy ? "busy" : "idle",
                   stopped ? "paused" : "running",
                   (unsigned long)sl_prompt_queue_count(state->sl),
                   enabled ? "on" : "off");
    return command_message(state, text);
  }
  if (!strcmp(line, "/resume"))
    return sl_prompt_queue_resume(state->sl) == SL_OK
               ? command_message(state, "Backlog resumed.")
               : -1;
  if (command_named(line, "/approve")) {
    if (sl_get_auto_approval(state->sl, &enabled) != SL_OK)
      return -1;
    if (!strcmp(line, "/approve"))
      enabled = !enabled;
    else if (!strcmp(line, "/approve on"))
      enabled = 1;
    else if (!strcmp(line, "/approve off"))
      enabled = 0;
    else
      return command_message(state, "Usage: /approve [on|off]");
    return sl_set_auto_approval(state->sl, enabled) == SL_OK
               ? command_message(
                     state,
                     enabled
                         ? "Auto-approval ON (all waiting requests approved)."
                         : "Auto-approval OFF (new requests need approval).")
               : -1;
  }
  if (command_named(line, "/steer"))
    return render_user_prompt(state, line) == 0
               ? render_note(state, "Steering applied.")
               : -1;
  *handled = 0;
  return 0;
}

static int deliver_steers(struct chat_state *state, int at_seam) {
  size_t index;
  sl_prompt_queue_entry_info_t info;
  char *line;
  if (sync_queue_readiness(state, at_seam) != 0)
    return -1;
  for (index = 0; index < sl_prompt_queue_count(state->sl);) {
    if (sl_prompt_queue_info(state->sl, index, &info) != SL_OK)
      return -1;
    if (info.mode != SL_PROMPT_QUEUE_MODE_STEER || !info.eligible) {
      index++;
      continue;
    }
    /* libmdf retains a block separator until the next block arrives. Close
     * this response segment before inserting a separately rendered prompt. */
    if (at_seam && finish_response_document(state) != 0)
      return -1;
    at_seam = 0;
    line = NULL;
    if (sl_prompt_queue_take(state->sl, index, &line) != SL_OK)
      return -1;
    if (strcmp(line, "/quit") == 0) {
      sl_free_string(state->sl, line);
      state->exit_requested = 1;
      return show_goodbye(state) == 0 && sl_cancel(state->sl) == SL_OK ? 0 : -1;
    }
    {
      int handled;
      int rc = handle_command(state, line, &handled);
      if (rc != 0 || handled) {
        sl_free_string(state->sl, line);
        if (rc != 0)
          return -1;
        continue;
      }
    }
    if (render_user_prompt(state, line) != 0) {
      sl_free_string(state->sl, line);
      return -1;
    }
    sl_free_string(state->sl, line);
  }
  return sync_queue_readiness(state, 0);
}

static unsigned int character_delay_us(void) {
  const char *value;
  char *end;
  long milliseconds;
  value = getenv("SOFTLINE_CHAT_CHAR_MS");
  if (!value || value[0] == '\0')
    return 20000u;
  errno = 0;
  milliseconds = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || milliseconds < 0 ||
      milliseconds > 60000)
    return 20000u;
  return (unsigned int)milliseconds * 1000u;
}

static void wait_character(unsigned int microseconds) {
  struct timespec delay;
  delay.tv_sec = (time_t)(microseconds / 1000000u);
  delay.tv_nsec = (long)(microseconds % 1000000u) * 1000L;
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
    ;
}

static int finish_operation(struct chat_state *state) {
  if (state->watch_id != 0 &&
      sl_watch_remove(state->sl, state->watch_id) != SL_OK)
    return -1;
  state->watch_id = 0;
  if (state->watch_fd >= 0) {
    close(state->watch_fd);
    state->watch_fd = -1;
  }
  if (state->worker_pid > 0) {
    if (waitpid(state->worker_pid, NULL, 0) != state->worker_pid)
      return -1;
    state->worker_pid = -1;
  }
  if (finish_response_document(state) != 0 || set_busy(state, 0) != 0)
    return -1;
  /* A steer that misses the final response seam is still a user turn. Give
   * the oldest pending entry its own response instead of quoting it alone. */
  return dispatch_next_turn(state);
}

static int cancel_operation(struct chat_state *state) {
  if (state->watch_id != 0 &&
      sl_watch_remove(state->sl, state->watch_id) != SL_OK)
    return -1;
  state->watch_id = 0;
  if (state->watch_fd >= 0) {
    close(state->watch_fd);
    state->watch_fd = -1;
  }
  if (state->worker_pid > 0) {
    if (kill(state->worker_pid, SIGTERM) != 0 && errno != ESRCH)
      return -1;
    while (waitpid(state->worker_pid, NULL, 0) < 0) {
      if (errno != EINTR)
        return -1;
    }
    state->worker_pid = -1;
  }
  if (finish_response_document(state) != 0)
    return -1;
  return set_busy(state, 0);
}

static int operation_watch(sl_t *sl, const sl_watch_event_t *event,
                           void *userdata) {
  struct chat_state *state;
  char bytes[64];
  ssize_t amount;
  ssize_t i;
  (void)sl;
  state = (struct chat_state *)userdata;
  if (!state || !event ||
      (event->events & (SL_WATCH_READ | SL_WATCH_HANGUP | SL_WATCH_ERROR)) == 0)
    return SL_ERROR_INVALID;
  if (sync_geometry(state) != 0)
    return SL_ERROR;
  amount = read(state->watch_fd, bytes, sizeof(bytes));
  if (amount > 0) {
    for (i = 0; i < amount; i++) {
      if (!state->response_open) {
        if (state->response_renderer->begin_document(
                state->response_renderer) != MDF_OK)
          return SL_ERROR_IO;
        state->response_open = 1;
      }
      if (state->response_renderer->feed(state->response_renderer, bytes + i,
                                         1) != MDF_OK)
        return SL_ERROR_IO;
      web_byte(state, bytes[i]);
      if (bytes[i] == '\n' && state->previous_response_char == '\n') {
        if (deliver_steers(state, 1) != 0)
          return SL_ERROR_IO;
        if (state->exit_requested)
          return SL_OK;
      }
      state->previous_response_char = bytes[i];
      state->response_bytes++;
      if (state->response_bytes == 15 && refresh_chat_activity(state) != 0)
        return SL_ERROR_IO;
    }
    web_flush(state);
    return SL_OK;
  }
  if (amount == 0)
    return finish_operation(state) == 0 ? SL_OK : SL_ERROR_IO;
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    return SL_OK;
  return SL_ERROR_IO;
}

static int start_operation(struct chat_state *state) {
  const char *answer;
  const char *segments[2];
  char chart[DEMO_CHART_CAPACITY];
  int pipe_fds[2];
  int flags;
  pid_t pid;
  unsigned int delay;
  int gate_fd = -1;
  const char *gate = getenv("SOFTLINE_CHAT_GATE_FD");
  size_t i, segment;
  if (gate) {
    char *end;
    long value;
    errno = 0;
    value = strtol(gate, &end, 10);
    if (errno || end == gate || *end || value < 3 || value > 9) {
      fprintf(stderr, "SOFTLINE_CHAT_GATE_FD must be an inherited FD 3..9\n");
      return -1;
    }
    gate_fd = (int)value;
  }
  if (state->busy || sync_geometry(state) != 0)
    return -1;
  if (state->response_documents > 0 && state->response_renderer->begin_document(
                                           state->response_renderer) != MDF_OK)
    return -1;
  state->response_open = 1;
  state->response_bytes = 0;
  state->previous_response_char = '\0';
  answer =
      response_templates[state->turn_index++ % (sizeof(response_templates) /
                                                sizeof(response_templates[0]))];
  if (demo_chart_next(&state->charts, chart, sizeof(chart)) < 0)
    return -1;
  segments[0] = answer;
  segments[1] = chart;
  delay = character_delay_us();
  if (pipe(pipe_fds) != 0)
    return -1;
  flags = fcntl(pipe_fds[0], F_GETFL);
  if (flags < 0 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return -1;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    (void)signal(SIGINT, SIG_IGN);
    if (gate_fd >= 0) {
      char token;
      ssize_t amount;
      do {
        do {
          amount = read(gate_fd, &token, 1);
        } while (amount < 0 && errno == EINTR);
        if (amount != 1)
          _exit(1);
      } while (token != '\n');
    }
    for (segment = 0; segment < 2; ++segment) {
      for (i = 0; segments[segment][i] != '\0'; i++) {
        if (write(pipe_fds[1], segments[segment] + i, 1) != 1)
          _exit(0);
        if (delay > 0)
          wait_character(delay);
      }
    }
    close(pipe_fds[1]);
    _exit(0);
  }
  close(pipe_fds[1]);
  state->watch_fd = pipe_fds[0];
  state->worker_pid = pid;
  state->watch_id = 0;
  if (set_busy(state, 1) != 0 ||
      (!state->web_only &&
       sl_watch_add(state->sl, state->watch_fd,
                    SL_WATCH_READ | SL_WATCH_HANGUP | SL_WATCH_ERROR,
                    operation_watch, state, &state->watch_id) != SL_OK)) {
    (void)cancel_operation(state);
    return -1;
  }
  return 0;
}

static int dispatch_next_turn(struct chat_state *state) {
  char *line;
  size_t index;
  sl_prompt_queue_entry_info_t info;
  for (index = 0; index < sl_prompt_queue_count(state->sl);) {
    if (sl_prompt_queue_info(state->sl, index, &info) != SL_OK)
      return -1;
    if (!info.eligible || (info.policy.enqueue && !state->web)) {
      index++;
      continue;
    }
    if (state->web && info.policy.enqueue) {
      char *peek = NULL;
      int shell;
      if (sl_prompt_queue_peek(state->sl, index, &peek) != SL_OK)
        return -1;
      shell = is_shell_command(peek);
      sl_free_string(state->sl, peek);
      if (shell) {
        ++index;
        continue;
      }
    }
    line = NULL;
    if (sl_prompt_queue_take(state->sl, index, &line) != SL_OK)
      return -1;
    if (state->web && info.policy.enqueue) {
      int handled;
      if (handle_command(state, line, &handled)) {
        sl_free_string(state->sl, line);
        return -1;
      }
      if (handled) {
        sl_free_string(state->sl, line);
        continue;
      }
    }
    if (strcmp(line, "/quit") == 0) {
      sl_free_string(state->sl, line);
      state->exit_requested = 1;
      return show_goodbye(state) == 0 && sl_cancel(state->sl) == SL_OK ? 0 : -1;
    }
    if (render_user_prompt(state, line) != 0 || start_operation(state) != 0) {
      sl_free_string(state->sl, line);
      return -1;
    }
    sl_free_string(state->sl, line);
    break;
  }
  return 0;
}

static double monotonic_seconds(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    return -1;
  return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void schedule_injection(struct chat_state *state, double now) {
  unsigned int interval = state->approval_interval_ms;
  if (!interval)
    interval = 10000u + (unsigned int)rand() % 10001u;
  state->next_injection = now + (double)interval / 1000.0;
}

/* The demo uses the editor's owner-thread idle hook. No timer worker or
 * competing input loop is needed; typing and stream watches keep priority. */
static void approval_demo_idle(sl_t *sl, void *userdata) {
  struct chat_state *state = userdata;
  sl_prompt_queue_delivery_t delivery;
  sl_prompt_queue_entry_info_t draft;
  double now = monotonic_seconds();
  int result = SL_OK;
  if (demo_web_tick(state->web))
    state->demo_failed = 1;
#ifdef SOFTLINE_CBOR_TCP
  if (state->remote) {
    if (demo_client_idle(state->remote) != SL_OK)
      state->demo_failed = 1;
    demo_web_queue(state->web);
    return;
  }
#endif
  if (now < 0)
    result = SL_ERROR_IO;
  else if (state->approval_demo && now >= state->next_injection) {
    schedule_injection(state, now);
    if (sl_prompt_queue_count(sl) < 4) {
      char message[2048];
      sl_prompt_id_t id;
      int steering = (state->injected % 2u) != 0;
      unsigned int shape = state->injected ? (unsigned int)rand() % 4u : 0u;
      const char *details = shape ? approval_details[shape]
                            : steering
                                ? "Focus the next answer on verification."
                                : "Summarize the next steps before continuing.";
      (void)snprintf(message, sizeof(message), "Demo %s request #%u: %s",
                     steering ? "steering" : "queued", state->injected + 1u,
                     details);
      result = steering ? sl_inject_steer_prompt(sl, message, &id)
                        : sl_inject_queued_prompt(sl, message, &id);
      if (result == SL_OK)
        state->injected++;
    }
  }
  /* An approved steer arriving between turns gets its own response. Respect
   * the queue's stopped state after cancellation and an unfinished draft. */
  if (result == SL_OK && !state->busy && (state->web || !sl_buffer(sl)[0]) &&
      (state->web || !sl->history_search_active(sl))) {
    result = sl_prompt_queue_draft_info(sl, &draft);
    if (result == SL_OK && !draft.id) {
      result = sl_get_prompt_queue_delivery(sl, &delivery);
      if (result == SL_OK && delivery == SL_PROMPT_QUEUE_DELIVERY_AUTO &&
          dispatch_next_turn(state) != 0)
        result = SL_ERROR_IO;
    }
  }
  if (result != SL_OK) {
    state->demo_failed = 1;
    (void)sl_cancel(sl);
  }
  demo_web_queue(state->web);
}

static int setup_approval_demo(struct chat_state *state) {
  const char *enabled = getenv("SOFTLINE_CHAT_APPROVAL_DEMO");
  const char *interval = getenv("SOFTLINE_CHAT_APPROVAL_MS");
  const char *seed_text = getenv("SOFTLINE_CHAT_APPROVAL_SEED");
  unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
  double now;
  if (!state->interactive || !enabled || strcmp(enabled, "1"))
    return 0;
  state->approval_demo = 1;
  if (interval && interval[0]) {
    char *end;
    long value;
    errno = 0;
    value = strtol(interval, &end, 10);
    if (errno || *end || value < 1 || value > 60000) {
      fprintf(stderr, "SOFTLINE_CHAT_APPROVAL_MS must be 1..60000\n");
      return -1;
    }
    state->approval_interval_ms = (unsigned int)value;
  }
  now = monotonic_seconds();
  if (now < 0)
    return -1;
  if (seed_text && seed_text[0]) {
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(seed_text, &end, 10);
    if (seed_text[0] < '0' || seed_text[0] > '9' || errno || *end ||
        value > UINT_MAX) {
      fprintf(stderr, "SOFTLINE_CHAT_APPROVAL_SEED must be 0..%u\n", UINT_MAX);
      return -1;
    }
    seed = (unsigned int)value;
  }
  srand(seed);
  state->approval_seed = seed;
  schedule_injection(state, now);
  if (state->remote)
    return 0;
  return sl_set_prompt_queue_delivery(state->sl,
                                      SL_PROMPT_QUEUE_DELIVERY_AUTO) == SL_OK &&
                 sl_set_idle_callback(state->sl, approval_demo_idle, state) ==
                     SL_OK
             ? 0
             : -1;
}

static void keep_chat_on_interrupt(int signo) { (void)signo; }

static int cancel_editor_key(sl_t *sl, sl_key_t key, void *userdata,
                             sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)userdata;
  if (!action)
    return SL_ERROR_INVALID;
  *action = SL_KEY_ACTION_CANCEL;
  return SL_OK;
}

/* Frontend exit controls never depend on remote queue admission. A queued
 * entry being edited keeps its normal submission/approval semantics. */
static int exit_editor_key(sl_t *sl, sl_key_t key, void *userdata,
                           sl_key_action_t *action) {
  struct chat_state *state = (struct chat_state *)userdata;
  const char *text = sl_buffer(sl);
  sl_prompt_queue_entry_info_t draft;
  int immediate = key == SL_KEY_ALT_BASE + 'q';
  int leaving = key == SL_KEY_CTRL_D && text[0] == '\0';
  *action = SL_KEY_ACTION_PASS;
  if (immediate) {
    state->exit_requested = 1;
    *action = SL_KEY_ACTION_SUSPEND;
    return show_goodbye(state) == 0 ? SL_OK : SL_ERROR_IO;
  }
  if (sl->history_search_active(sl))
    return SL_OK;
  if (leaving) {
    state->exit_requested = 1;
    *action = SL_KEY_ACTION_SUSPEND;
    return show_goodbye(state) == 0 ? SL_OK : SL_ERROR_IO;
  }
  if (state->remote && key != SL_KEY_CTRL_D && !strcmp(text, "/quit")) {
    if (sl_prompt_queue_draft_info(sl, &draft) != SL_OK)
      return SL_ERROR_IO;
    if (!draft.id) {
      state->exit_requested = 1;
      *action = SL_KEY_ACTION_SUSPEND;
      return show_goodbye(state) == 0 ? SL_OK : SL_ERROR_IO;
    }
  }
  return SL_OK;
}

static int set_prompt_theme_from_environment(sl_t *sl) {
  const char *name;
  sl_prompt_theme_t theme;
  name = getenv("SOFTLINE_PROMPT_THEME");
  theme = SL_PROMPT_THEME_DEFAULT;
  if (name && name[0] != '\0') {
    if (strcmp(name, "plain") == 0)
      theme = SL_PROMPT_THEME_PLAIN;
    else if (strcmp(name, "accent") == 0)
      theme = SL_PROMPT_THEME_ACCENT;
    else if (strcmp(name, "dracula") == 0)
      theme = SL_PROMPT_THEME_DRACULA;
    else if (strcmp(name, "gruvbox") == 0)
      theme = SL_PROMPT_THEME_GRUVBOX;
    else if (strcmp(name, "monochrome") == 0)
      theme = SL_PROMPT_THEME_MONOCHROME;
    else if (strcmp(name, "monogreen") == 0)
      theme = SL_PROMPT_THEME_MONOGREEN;
    else if (strcmp(name, "outrun") == 0)
      theme = SL_PROMPT_THEME_OUTRUN;
    else if (strcmp(name, "riced") == 0)
      theme = SL_PROMPT_THEME_RICED;
    else if (strcmp(name, "synthwave") == 0)
      theme = SL_PROMPT_THEME_SYNTHWAVE;
    else if (strcmp(name, "default") != 0) {
      fprintf(stderr, "invalid SOFTLINE_PROMPT_THEME: %s\n", name);
      return -1;
    }
  }
  return sl_set_prompt_theme(sl, theme) == SL_OK ? 0 : -1;
}

static void report_failure(struct chat_state *state, const char *operation) {
  const char *softline_error;
  const char *mdf_error;
  int flags = -1;
  softline_error = state->sl ? sl_last_error(state->sl) : NULL;
  mdf_error = state->response_renderer
                  ? state->response_renderer->error(state->response_renderer)
                  : NULL;
  /* A fatal diagnostic must not turn bounded terminal failure into another
   * blocking write to the same saturated terminal. */
  if (isatty(STDERR_FILENO)) {
    flags = fcntl(STDERR_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK))
      return;
  }
  fprintf(stderr, "chat %s failed: %s%s%s\n", operation,
          softline_error ? softline_error : "unknown error",
          mdf_error ? "; libmdf: " : "", mdf_error ? mdf_error : "");
  if (flags >= 0)
    (void)fcntl(STDERR_FILENO, F_SETFL, flags);
}

#ifdef SOFTLINE_CBOR_TCP
static int remote_event(void *context, int kind, const char *text) {
  struct chat_state *state = context;
  size_t i;
  int unavailable;
  switch (kind) {
  case DEMO_TERMINAL_STATE: {
    unsigned long active;
    int enabled;
    char *end;
    if (!text || *text < '0' || *text > '9')
      return SL_ERROR_INVALID;
    errno = 0;
    active = strtoul(text, &end, 10);
    if (errno || active > (size_t)-1 - DEMO_WWW_FDS || *end != ' ' ||
        (end[1] != '0' && end[1] != '1') || end[2])
      return SL_ERROR_INVALID;
    enabled = end[1] == '1';
    state->terminal_upstream = (size_t)active;
    demo_www_set_terminal_state(demo_web_listener(state->web), (size_t)active,
                                enabled);
    return SL_OK;
  }
  case DEMO_IMAGE_STARTED:
  case DEMO_IMAGE_STORING:
    state->image_active = 1;
    state->image_storing = kind == DEMO_IMAGE_STORING;
    return refresh_chat_activity(state) == 0 ? SL_OK : SL_ERROR_IO;
  case DEMO_IMAGE_FINISHED:
  case DEMO_IMAGE_FAILED:
    state->image_active = state->image_storing = 0;
    if (state->remote_state != DEMO_READY)
      return SL_OK;
    if (refresh_chat_activity(state))
      return SL_ERROR_IO;
    if (kind == DEMO_IMAGE_FAILED && !state->busy && text)
      return set_chat_message(state->sl, text);
    return SL_OK;
  case DEMO_DISCONNECTED:
  case DEMO_CONNECTING:
  case DEMO_SYNCHRONIZING:
    unavailable = state->remote_state == DEMO_DISCONNECTED ||
                  state->remote_state == DEMO_CONNECTING ||
                  state->remote_state == DEMO_SYNCHRONIZING;
    state->remote_state = kind;
    if (kind == DEMO_CONNECTING)
      state->connecting_since = monotonic_seconds();
    else if (kind == DEMO_DISCONNECTED)
      state->shell_requested = 0;
    /* Transport retries are observable to the host, but availability has
     * not changed until a synchronized snapshot arrives. Keep its frame. */
    if (unavailable)
      return SL_OK;
    /* Keep the previous availability message visible while changing activity
     * indicators; a connection attempt must not briefly look ready. */
    if (finish_response_document(state) ||
        sl_set_status_spinner(state->sl, 0) != SL_OK ||
        sl_set_status_busy(state->sl, 0) != SL_OK)
      return SL_ERROR_IO;
    state->busy = 0;
    demo_web_status(state->web,
                    "Disconnected; retrying automatically. Alt-R reconnects.",
                    0);
    return set_chat_message(
        state->sl, "Disconnected; retrying automatically. Alt-R reconnects.");
  case DEMO_READY:
    state->remote_state = kind;
    return set_busy(state, 0) == 0 ? SL_OK : SL_ERROR_IO;
  case DEMO_PAUSED:
    state->remote_state = kind;
    return SL_OK;
  case DEMO_PROMPT:
    return render_user_prompt(state, text) == 0 ? SL_OK : SL_ERROR_IO;
  case DEMO_RESPONSE_BEGIN:
    if (finish_response_document(state))
      return SL_ERROR_IO;
    if (state->response_documents && state->response_renderer->begin_document(
                                         state->response_renderer) != MDF_OK)
      return SL_ERROR_IO;
    state->response_open = 1;
    state->response_bytes = 0;
    state->previous_response_char = 0;
    return set_busy(state, 1) == 0 ? SL_OK : SL_ERROR_IO;
  case DEMO_OUTPUT:
    if (sync_geometry(state))
      return SL_ERROR_IO;
    for (i = 0; text[i]; ++i) {
      if (!state->response_open) {
        if (state->response_renderer->begin_document(
                state->response_renderer) != MDF_OK)
          return SL_ERROR_IO;
        state->response_open = 1;
      }
      if (state->response_renderer->feed(state->response_renderer, text + i,
                                         1) != MDF_OK)
        return SL_ERROR_IO;
      web_byte(state, text[i]);
      if (++state->response_bytes == 15 && refresh_chat_activity(state) != 0)
        return SL_ERROR_IO;
    }
    web_flush(state);
    return SL_OK;
  case DEMO_SEGMENT_END:
    return finish_response_document(state) == 0 ? SL_OK : SL_ERROR_IO;
  case DEMO_OUTPUT_END:
    return finish_response_document(state) == 0 && set_busy(state, 0) == 0
               ? SL_OK
               : SL_ERROR_IO;
  case DEMO_MESSAGE:
    return command_message(state, text) == 0 ? SL_OK : SL_ERROR_IO;
  case DEMO_NOTE:
    return render_note(state, text) == 0 ? SL_OK : SL_ERROR_IO;
  case DEMO_SHELL:
    if (finish_response_document(state))
      return SL_ERROR_IO;
    state->shell_requested = 0;
    state->remote_shell = 1;
    if (text && render_user_prompt(state, text))
      return SL_ERROR_IO;
    return sl_last_readline_status(state->sl) == SL_READLINE_SUSPENDED
               ? SL_OK
               : sl_suspend(state->sl);
  case DEMO_QUIT:
    state->exit_requested = 1;
    return show_goodbye(state) == 0 ? sl_suspend(state->sl) : SL_ERROR_IO;
  default:
    return SL_ERROR_INVALID;
  }
}

/* Reconnect policy belongs to this chat host. The adapter owns the transport
 * and reports state through remote_event; Softline owns neither policy. A
 * watched timer continues dispatching while the user is actively typing. */
static int reconnect_tick(sl_t *sl, const sl_watch_event_t *event,
                          void *context) {
  struct chat_state *state = context;
  (void)sl;
  if (demo_timer_drain(event->fd))
    return SL_ERROR_IO;
  if (state->remote_state == DEMO_DISCONNECTED ||
      ((state->remote_state == DEMO_CONNECTING ||
        state->remote_state == DEMO_SYNCHRONIZING) &&
       monotonic_seconds() - state->connecting_since >= 3))
    return demo_client_reconnect(state->remote);
  return SL_OK;
}

static int setup_reconnect(struct chat_state *state) {
  const char *value = getenv("SOFTLINE_CHAT_RECONNECT_MS");
  char *end;
  unsigned long interval = 2000;
  if (value) {
    errno = 0;
    interval = strtoul(value, &end, 10);
    if (!*value || *end || errno || interval < 10 || interval > 60000) {
      fprintf(stderr, "SOFTLINE_CHAT_RECONNECT_MS must be 10..60000\n");
      return -1;
    }
  }
  state->reconnect_ms = (unsigned int)interval;
  state->reconnect_fd = demo_timer_create(state->reconnect_ms);
  if (state->reconnect_fd < 0) {
    perror("reconnect timer");
    return -1;
  }
  return sl_watch_add(state->sl, state->reconnect_fd, SL_WATCH_READ,
                      reconnect_tick, state, &state->reconnect_watch) == SL_OK
             ? 0
             : -1;
}

static int reconnect_editor(sl_t *sl, sl_key_t key, void *context,
                            sl_key_action_t *action) {
  struct chat_state *state = context;
  (void)sl;
  (void)key;
  *action = SL_KEY_ACTION_HANDLED;
  state->shell_requested = 0;
  return demo_client_reconnect(state->remote);
}

#endif
static int web_submit(void *context, const char *text, int steer,
                      sl_queue_request_id_t *request_id) {
  struct chat_state *state = context;
  int result;
  size_t index;
  if (is_shell_command(text) || !strcmp(text, "/quit"))
    return SL_ERROR_UNSUPPORTED;
  if (state->remote) {
    sl_queue_request_t request;
    memset(&request, 0, sizeof(request));
    request.operation = SL_QUEUE_OFFER;
    request.text = text;
    request.mode =
        steer ? SL_PROMPT_QUEUE_MODE_STEER : SL_PROMPT_QUEUE_MODE_QUEUED;
    return sl_queue_backend_request(state->sl, &request, request_id);
  }
  if (is_local_command(text)) {
    int handled;
    return handle_command(state, text, &handled) == 0 ? SL_OK : SL_ERROR;
  }
  index = sl_prompt_queue_count(state->sl);
  {
    int stopped = 0;
    (void)sl_get_prompt_queue_stopped(state->sl, &stopped);
    if (stopped && !state->busy && text[0] != '/' && text[0] != '!')
      index = 0;
    result = sl_prompt_queue_insert(state->sl, index, text, 0, NULL);
    if (result == SL_OK && stopped && index == 0 && text[0] != '/' &&
        text[0] != '!')
      result = sl_prompt_queue_resume(state->sl);
  }
  if (result == SL_OK && steer)
    result =
        sl_prompt_queue_set_mode(state->sl, index, SL_PROMPT_QUEUE_MODE_STEER);
  if (result == SL_OK && sync_queue_readiness(state, 0))
    result = SL_ERROR_IO;
  /* The queue accepts immediately; only its visual projection is delayed. */
  return result;
}
static int web_control(void *context, const char *action, const char *id,
                       const char *revision) {
  struct chat_state *state = context;
  if (!strcmp(action, "cancel")) {
#ifdef SOFTLINE_CBOR_TCP
    if (state->remote)
      return demo_client_cancel(state->remote);
#endif
    return sl_prompt_queue_stop(state->sl) == SL_OK && !cancel_operation(state)
               ? SL_OK
               : SL_ERROR;
  }
  if (strcmp(action, "approve") && strcmp(action, "reject"))
    return SL_ERROR_INVALID;
  if (state->remote) {
    sl_queue_request_t request;
    memset(&request, 0, sizeof(request));
    request.operation =
        !strcmp(action, "approve") ? SL_QUEUE_APPROVE : SL_QUEUE_WITHDRAW;
    request.item_id = id;
    request.item_revision = revision;
    return sl_queue_backend_request(state->sl, &request, NULL);
  } else {
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(id, &end, 10);
    if (errno || !*id || *end || !value)
      return SL_ERROR_INVALID;
    return !strcmp(action, "approve")
               ? sl_prompt_queue_approve(state->sl, value)
               : sl_prompt_queue_reject(state->sl, value);
  }
}
#ifdef SOFTLINE_CBOR_TCP
static int web_image(void *context, sl_t *source,
                     const sl_image_paste_event_t *event) {
  struct chat_state *state = context;
  return demo_client_image_event(state->remote, source, event);
}
#endif
static int web_edit(void *context, const char *id, const char *revision,
                    const char *text, const char *token,
                    sl_prompt_queue_mode_t mode, sl_queue_request_id_t *out) {
  struct chat_state *state = context;
  size_t i;
  if (text && (is_shell_command(text) || !strcmp(text, "/quit")))
    return SL_ERROR_UNSUPPORTED;
  if (state->remote) {
    sl_queue_request_t request;
    int status;
    if (!token) {
      sl_queue_backend_info_t info;
      sl_queue_entry_t entry;
      if (sl_queue_backend_info(state->sl, &info) != SL_OK)
        return SL_ERROR;
      for (i = 0; i < info.visible_count; ++i)
        if (sl_queue_backend_entry(state->sl, i, &entry) == SL_OK &&
            !strcmp(entry.id, id) && entry.edit_held)
          return SL_ERROR_CONFLICT;
    }
    memset(&request, 0, sizeof(request));
    request.operation = token ? text ? SL_QUEUE_END_EDIT : SL_QUEUE_ABORT_EDIT
                              : SL_QUEUE_BEGIN_EDIT;
    request.item_id = id;
    request.item_revision = revision;
    request.edit_token = token;
    request.text = token ? text : NULL;
    request.mode = mode;
    status = sl_queue_backend_request(state->sl, &request, out);
    if (status != SL_OK && token && text) {
      request.operation = SL_QUEUE_ABORT_EDIT;
      request.text = NULL;
      (void)sl_queue_backend_request(state->sl, &request, NULL);
    }
    return status;
  }
  for (i = 0; i < sl_prompt_queue_count(state->sl); ++i) {
    sl_prompt_queue_entry_info_t info;
    char current[32];
    if (sl_prompt_queue_info(state->sl, i, &info) != SL_OK)
      return SL_ERROR;
    (void)snprintf(current, sizeof(current), "%lu", info.id);
    if (!strcmp(current, id))
      return sl_prompt_queue_replace(state->sl, i, text);
  }
  return SL_ERROR_CONFLICT;
}
#ifdef SOFTLINE_CBOR_TCP
static void web_result(void *context, const sl_queue_result_t *result) {
  struct chat_state *state = context;
  demo_web_result(state->web, result);
}
#endif
static volatile sig_atomic_t web_quit;
static void stop_web(int signum) {
  (void)signum;
  web_quit = 1;
}
static int run_web_only(struct chat_state *state) {
  (void)signal(SIGINT, stop_web);
  (void)signal(SIGTERM, stop_web);
  while (!web_quit && !state->exit_requested && !state->demo_failed) {
    struct pollfd fds[DEMO_WWW_FDS + 1];
    int count, timeout, result;
    approval_demo_idle(state->sl, state);
    if (state->demo_failed)
      break;
    count =
        demo_www_descriptors(demo_web_listener(state->web), fds, DEMO_WWW_FDS);
    if (count < 0)
      return -1;
    fds[count].fd = state->watch_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    /* The bounded tick also drives optional host approval-demo injection. */
    timeout = demo_web_timeout(state->web, 100);
    result = poll(fds, (nfds_t)(count + 1), timeout);
    if (result < 0 && errno != EINTR)
      return -1;
    if (result > 0 && fds[count].revents) {
      sl_watch_event_t event;
      memset(&event, 0, sizeof(event));
      event.fd = state->watch_fd;
      event.events = SL_WATCH_READ;
      if (operation_watch(state->sl, &event, state) != SL_OK)
        return -1;
    }
  }
  return state->demo_failed ? -1 : 0;
}

int main(int argc, char **argv) {
  static const char *const status_elements[] = {"streaming demo", "ctx 36%",
                                                "demo/project", "queue demo"};
  struct chat_state state;
  sl_prompt_source_t source;
  sl_readline_status_t status;
  char *line;
  int interactive;
  int exit_code;
  int cancelled_busy;

  memset(&state, 0, sizeof(state));
  demo_charts_init(&state.charts,
                   (unsigned long)time(NULL) ^ (unsigned long)getpid());
  state.reconnect_fd = -1;
  state.watch_fd = -1;
  state.worker_pid = -1;
  state.null_fd = -1;
  state.web_only = argc == 2 && !strcmp(argv[1], "--web-only");
  interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
  if (state.web_only)
    interactive = 1;
  state.interactive = interactive;
  exit_code = 0;
#ifdef SOFTLINE_CBOR_TCP
  if (argc == 3) {
    sl_config_t config;
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (!*argv[2] || *end || port < 1 || port > 65535 || !interactive) {
      fprintf(stderr,
              "Remote chat requires an interactive terminal and HOST PORT.\n");
      return 1;
    }
    state.remote = demo_client_create(argv[1], (unsigned short)port);
    if (!state.remote)
      return 1;
    sl_config_init(&config);
    config.prompt_queue = 1;
    config.prompt_queue_max_entries = 64;
    config.queue_backend = demo_client_backend(state.remote);
    config.queue_backend_context = state.remote;
    if (demo_client_image_config(state.remote, &config) != SL_OK) {
      destroy_remote(&state);
      return 1;
    }
    state.sl = sl_create_with_config(&config);
  } else
#endif
      if (state.web_only) {
    sl_config_t config;
    state.null_fd = open("/dev/null", O_RDWR);
    if (state.null_fd < 0)
      return 1;
    sl_config_init(&config);
    config.input_fd = config.output_fd = state.null_fd;
    state.sl = sl_create_with_config(&config);
  } else if (argc == 1)
    state.sl = sl_create();
  else {
#ifdef SOFTLINE_CBOR_TCP
    fprintf(stderr, "usage: %s [127.0.0.1 PORT | --web-only]\n", argv[0]);
#else
    fprintf(stderr,
            "usage: %s [--web-only]. TCP chat requires SL_ENABLE_CBOR_TCP.\n",
            argv[0]);
#endif
    return 1;
  }
  if (!state.sl) {
    fprintf(stderr, "failed to create softline\n");
    destroy_remote(&state);
    return 1;
  }
  {
    const char *delay = getenv("SOFTLINE_QUEUE_DISPLAY_DELAY_MS");
    if (delay) {
      char *end;
      long value;
      errno = 0;
      value = strtol(delay, &end, 10);
      if (*delay < '0' || *delay > '9' || *end || errno || value < 0 ||
          value > INT_MAX ||
          sl_set_prompt_queue_display_delay(state.sl, (int)value) != SL_OK) {
        fprintf(
            stderr,
            "SOFTLINE_QUEUE_DISPLAY_DELAY_MS must be an integer from 0 to %d\n",
            INT_MAX);
        sl_destroy(state.sl);
        destroy_remote(&state);
        return 1;
      }
    }
  }
  {
    const char *links = getenv("SOFTLINE_OSC8");
    sl_osc8_mode_t mode = SL_OSC8_AUTO;
    if (links && *links) {
      if (!strcmp(links, "on"))
        mode = SL_OSC8_ON;
      else if (!strcmp(links, "off"))
        mode = SL_OSC8_OFF;
      else if (strcmp(links, "auto")) {
        fprintf(stderr, "SOFTLINE_OSC8 must be off, on or auto\n");
        sl_destroy(state.sl);
        destroy_remote(&state);
        return 1;
      }
    }
    if (sl_set_osc8_mode(state.sl, mode) != SL_OK) {
      sl_destroy(state.sl);
      destroy_remote(&state);
      return 1;
    }
  }
  if (demo_completion_setup(state.sl, 1) != SL_OK) {
    report_failure(&state, "completion setup");
    sl_destroy(state.sl);
    destroy_remote(&state);
    return 1;
  }
  if (sl_history_open(state.sl, "softline.examples.chat",
                      getenv("SOFTLINE_HISTORY_DIR")) != SL_OK ||
      sl_history_set_auto_add(state.sl, 1) != SL_OK) {
    report_failure(&state, "history setup");
    sl_destroy(state.sl);
    destroy_remote(&state);
    return 1;
  }
  state.columns = mdf_terminal_width(STDOUT_FILENO, 80);
  {
    demo_web_ops_t ops;
    int enabled;
    memset(&ops, 0, sizeof(ops));
    ops.submit = web_submit;
    ops.control = web_control;
    ops.edit = web_edit;
    ops.terminal_unlock_command = "/terminal-lock off";
#ifdef SOFTLINE_CBOR_TCP
    if (state.remote)
      ops.image = web_image;
#endif
    state.web = demo_web_open(state.sl, &ops, &state, 8080, &enabled);
    if ((enabled && !state.web) || (state.web_only && !enabled)) {
      fprintf(stderr, "web listener: %s\n",
              enabled ? strerror(errno) : "--web-only requires SOFTLINE_WEB=1");
      exit_code = 1;
      goto cleanup;
    }
    if (state.web && !state.web_only &&
        demo_www_attach(demo_web_listener(state.web), state.sl) != SL_OK) {
      report_failure(&state, "web watches");
      exit_code = 1;
      goto cleanup;
    }
    if (state.web)
      fprintf(stderr, "WEB http://127.0.0.1:%u\n",
              (unsigned int)demo_web_port(state.web));
#ifdef SOFTLINE_CBOR_TCP
    if (state.web && state.remote)
      demo_client_results(state.remote, web_result, &state);
#endif
  }
  if (interactive)
    (void)signal(SIGINT, keep_chat_on_interrupt);
  if (interactive && !state.web_only) {
    int i;
    for (i = 0; i < 2; ++i)
      if (sl_transcript_add(state.sl, demo_transcript_titles[i],
                            DEMO_TRANSCRIPT_ROWS, DEMO_TRANSCRIPT_COLUMNS,
                            &state.transcripts[i]) != SL_OK) {
        report_failure(&state, "transcript setup");
        exit_code = 1;
        goto cleanup;
      }
    if (refresh_transcripts(&state)) {
      report_failure(&state, "transcript content");
      exit_code = 1;
      goto cleanup;
    }
  }
  if ((interactive &&
       ((!state.remote && sl_set_prompt_queue(state.sl, 1, 64, 3) != SL_OK) ||
        sl_set_prompt_queue_profile(
            state.sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) != SL_OK ||
        (!state.remote &&
         sl_set_prompt_queue_delivery(
             state.sl, SL_PROMPT_QUEUE_DELIVERY_AUTO) != SL_OK) ||
        (!state.remote && sl_set_prompt_queue_policy(state.sl, classify_prompt,
                                                     &state) != SL_OK) ||
        sl_set_statusline(state.sl, 1, 0) != SL_OK ||
        sl_set_status_elements(state.sl, status_elements,
                               sizeof(status_elements) /
                                   sizeof(status_elements[0])) != SL_OK ||
        set_prompt_theme_from_environment(state.sl) != 0 ||
        sl_bind_key(state.sl, SL_KEY_ESCAPE, cancel_editor_key, NULL) !=
            SL_OK ||
        sl_bind_key(state.sl, SL_KEY_ALT_BASE + 's', shell_editor_key,
                    &state) != SL_OK ||
        sl_bind_key(state.sl, SL_KEY_CTRL_D, exit_editor_key, &state) !=
            SL_OK ||
        sl_bind_key(state.sl, SL_KEY_ALT_BASE + 'q', exit_editor_key, &state) !=
            SL_OK)) ||
      state.sl->output_stream_begin(state.sl) != SL_OK ||
      new_renderer(&state, &state.note_renderer) != 0 ||
      new_renderer(&state, &state.response_renderer) != 0) {
    report_failure(&state, "setup");
    exit_code = 1;
    goto cleanup;
  }
  if (interactive) {
    if (set_busy(&state, 0) != 0 || setup_approval_demo(&state) != 0) {
      exit_code = 1;
      goto cleanup;
    }
    if (state.web &&
        sl_set_idle_callback(state.sl, approval_demo_idle, &state) != SL_OK) {
      exit_code = 1;
      goto cleanup;
    }
#ifdef SOFTLINE_CBOR_TCP
    if (state.remote) {
      demo_client_events(state.remote, remote_event, &state);
      demo_client_options(state.remote, character_delay_us(),
                          state.approval_demo, state.approval_interval_ms,
                          state.approval_seed);
      if (demo_client_attach(state.remote, state.sl) != SL_OK ||
          setup_reconnect(&state) != 0 ||
          sl_set_idle_callback(state.sl, approval_demo_idle, &state) != SL_OK ||
          sl_bind_key(state.sl, SL_KEY_ALT_BASE + 'r', reconnect_editor,
                      &state) != SL_OK ||
          sl_bind_key(state.sl, SL_KEY_ENTER, exit_editor_key, &state) !=
              SL_OK ||
          sl_bind_key(state.sl, SL_KEY_ALT_ENTER, exit_editor_key, &state) !=
              SL_OK ||
          sl_bind_key(state.sl, SL_KEY_CTRL_ENTER, exit_editor_key, &state) !=
              SL_OK) {
        exit_code = 1;
        goto cleanup;
      }
    }
#endif
    if (render_note(
            &state,
            "Enter sends or queues; Alt-Enter steers or promotes; "
            "Alt-E edits queue. Esc/Ctrl-C cancels; `/quit` or Alt-Q leaves. "
            "`!sh` or `/shell` opens your shell between turns; "
            "Policy demo: `/status` / `/approve` / `/steer` / `/resume`. "
            "Alt-S opens it while preserving your draft.") != 0) {
      report_failure(&state, "initial output");
      exit_code = 1;
      goto cleanup;
    }
    if (render_note(
            &state,
            "Completion demo: type `h` for a dim suggestion; Tab accepts, "
            "more Tabs cycle. Backspace restarts matching. "
            "Try `say ver` + Tab on any line; Ctrl-J adds a line; "
            "`/q` + Tab completes `/quit`. "
            "`/` resolves commands; `@` lists one directory at a time (except "
            ".git). "
            "Tab cycles; Right/Ctrl-F commits and previews the next list. "
            "Enter or a separating space removes "
            "`@`. "
            "[Hyperlink demo](https://pkt.systems).") != 0) {
      report_failure(&state, "completion demo output");
      exit_code = 1;
      goto cleanup;
    }
    if (state.approval_demo &&
        render_note(
            &state,
            "Approval demo: queued and steering requests arrive "
            "every 10-20 seconds. Alt-A focuses approvals; Alt-Y approves, "
            "Alt-N rejects while focused, Alt-E edits, Esc returns. "
            "`/approve` toggles auto-approval; `/approve on|off` sets it.") !=
            0) {
      exit_code = 1;
      goto cleanup;
    }
  }
  if (state.web_only) {
    exit_code = run_web_only(&state) ? 1 : 0;
    goto cleanup;
  }
  for (;;) {
    source = SL_PROMPT_SOURCE_NONE;
    line = sl_next_prompt(state.sl, NULL, &source);
    if (state.demo_failed) {
      sl_free_string(state.sl, line);
      report_failure(&state, "approval demo");
      exit_code = 1;
      break;
    }
    if (state.exit_requested) {
      if (line)
        sl_free_string(state.sl, line);
      break;
    }
    if (!line) {
      status = sl_last_readline_status(state.sl);
      if (status == SL_READLINE_SUSPENDED) {
#ifdef SOFTLINE_CBOR_TCP
        if (state.remote) {
          if (!state.remote_shell) {
            exit_code = 1;
            break;
          }
          demo_client_pause(state.remote);
        }
#endif
        if (run_shell(&state, 1) != 0) {
          report_failure(&state, "shell handoff");
          exit_code = 1;
          break;
        }
#ifdef SOFTLINE_CBOR_TCP
        if (state.remote) {
          state.remote_shell = 0;
          (void)demo_client_reconnect(state.remote);
        }
#endif
        continue;
      }
      if (status == SL_READLINE_CANCELLED ||
          status == SL_READLINE_INTERRUPTED) {
        cancelled_busy = interactive && state.busy;
#ifdef SOFTLINE_CBOR_TCP
        if (state.remote) {
          state.shell_requested = 0;
          if (demo_client_cancel(state.remote) != SL_OK) {
            (void)set_chat_message(
                state.sl,
                "Disconnected; retrying automatically. Alt-R reconnects.");
            continue;
          }
          (void)finish_response_document(&state);
          (void)set_busy(&state, 0);
        }
#endif
        if ((cancelled_busy && !state.remote &&
             cancel_operation(&state) != 0) ||
            (interactive &&
             render_note(&state, cancelled_busy ? "*Operation cancelled.*"
                                                : "*Cancelled.*") != 0)) {
          report_failure(&state, "cancel");
          exit_code = 1;
          break;
        }
        continue;
      }
      if (status == SL_READLINE_ERROR) {
        report_failure(&state, "readline");
        exit_code = 1;
      }
      break;
    }
    if (strcmp(line, "/quit") == 0) {
      if (show_goodbye(&state) != 0)
        exit_code = 1;
      sl_free_string(state.sl, line);
      break;
    }
    if (line[0] == '\0') {
      sl_free_string(state.sl, line);
      if (interactive && render_note(&state, "*Empty turn ignored.*") != 0) {
        exit_code = 1;
        break;
      }
      continue;
    }
    if (is_shell_command(line)) {
      int failed = 0;
      if (!interactive || state.busy) {
        failed = render_note(
            &state, !interactive ? "Shell requires an interactive terminal."
                                 : "Shell is available only between turns.");
      } else {
        failed =
            render_user_prompt(&state, line) != 0 || run_shell(&state, 0) != 0;
      }
      sl_free_string(state.sl, line);
      if (failed) {
        report_failure(&state, "shell handoff");
        exit_code = 1;
        break;
      }
      continue;
    }
    if (interactive) {
      int handled;
      if (handle_command(&state, line, &handled) != 0) {
        sl_free_string(state.sl, line);
        report_failure(&state, "command");
        exit_code = 1;
        break;
      }
      if (handled) {
        sl_free_string(state.sl, line);
        continue;
      }
    }
    if (render_user_prompt(&state, line) != 0) {
      sl_free_string(state.sl, line);
      report_failure(&state, "prompt render");
      exit_code = 1;
      break;
    }
    if (interactive && !state.busy && start_operation(&state) != 0) {
      sl_free_string(state.sl, line);
      report_failure(&state, "operation start");
      exit_code = 1;
      break;
    }
    sl_free_string(state.sl, line);
    (void)source;
  }

cleanup:
#ifdef SOFTLINE_CBOR_TCP
  demo_client_results(state.remote, NULL, NULL);
#endif
  demo_web_close(state.web);
  state.web = NULL;
  if (state.reconnect_watch)
    (void)sl_watch_remove(state.sl, state.reconnect_watch);
  if (state.reconnect_fd >= 0)
    close(state.reconnect_fd);
  if (state.busy && !state.remote)
    (void)cancel_operation(&state);
  if (state.note_renderer)
    state.note_renderer->destroy(state.note_renderer);
  if (state.response_renderer)
    state.response_renderer->destroy(state.response_renderer);
  if (state.sl) {
    if (state.sl->output_stream_end)
      (void)state.sl->output_stream_end(state.sl);
    sl_destroy(state.sl);
  }
  destroy_remote(&state);
  if (state.null_fd >= 0)
    close(state.null_fd);
  return exit_code;
}
