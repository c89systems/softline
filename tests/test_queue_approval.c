#define _DEFAULT_SOURCE
#include "silent_cursor_probe.h"
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int queue_api(void) {
  sl_config_t cfg;
  sl_t *sl;
  sl_prompt_id_t held, steer, ordinary, next, old;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_source_t source;
  char *text = NULL;
  int input[2], output[2];
  CHECK(pipe(input) == 0 && pipe(output) == 0);
  CHECK(write(input[1], "direct\n", 7) == 7);
  close(input[1]);
  sl_config_init(&cfg);
  cfg.prompt_queue = 1;
  cfg.input_fd = input[0];
  cfg.output_fd = output[1];
  cfg.prompt_queue_max_entries = 4;
  sl = sl_create_with_config(&cfg);
  CHECK(sl && sl->prompt_queue_info && sl->prompt_queue_draft_info &&
        sl->prompt_queue_approve && sl->prompt_queue_reject &&
        sl->last_prompt_id);
  CHECK(sl_prompt_queue_append(sl, "held", 1, &held) == SL_OK && held != 0);
  CHECK(sl->prompt_queue_append(sl, "ordinary", 0, &ordinary) == SL_OK);
  CHECK(sl_prompt_queue_insert(sl, 1, "steer", 1, &steer) == SL_OK);
  CHECK(sl_prompt_queue_set_mode(sl, 1, SL_PROMPT_QUEUE_MODE_STEER) == SL_OK);
  CHECK(sl_prompt_queue_append(sl, "last", 0, &next) == SL_OK);
  old = next;
  CHECK(held != steer && steer != ordinary && ordinary != next);
  CHECK(sl_prompt_queue_append(sl, "overflow", 0, &next) == SL_ERROR_FULL &&
        next == 0);
  CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == held &&
        info.held);
  CHECK(sl_prompt_queue_take(sl, 0, &text) == SL_ERROR_INVALID && !text);
  CHECK(sl_last_prompt_id(sl) == 0);
  text = sl_next_prompt(sl, "> ", &source);
  CHECK(text && !strcmp(text, "ordinary") && source == SL_PROMPT_SOURCE_QUEUED);
  CHECK(sl->last_prompt_id(sl) == ordinary);
  sl_free_string(sl, text);
  text = sl_next_prompt(sl, "> ", &source);
  CHECK(text && !strcmp(text, "last") && sl_last_prompt_id(sl) == old);
  sl_free_string(sl, text);
  text = sl_next_prompt(sl, "> ", &source);
  CHECK(text && !strcmp(text, "direct") && source == SL_PROMPT_SOURCE_DIRECT);
  CHECK(sl_last_prompt_id(sl) == 0 && sl_prompt_queue_count(sl) == 2);
  sl_free_string(sl, text);
  CHECK(sl_prompt_queue_replace(sl, 0, "updated") == SL_OK);
  CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == held &&
        info.held);
  CHECK(sl_prompt_queue_approve(sl, steer) == SL_ERROR_INVALID);
  CHECK(strstr(sl_last_error(sl), "FIFO"));
  CHECK(sl_prompt_queue_reject(sl, steer) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == held &&
        info.held);
  CHECK(sl_prompt_queue_info(sl, 1, &info) == SL_OK && info.id == steer &&
        info.held && sl_prompt_queue_count(sl) == 2);
  CHECK(sl_prompt_queue_approve(sl, held) == SL_OK);
  CHECK(sl_prompt_queue_approve(sl, steer) == SL_OK);
  CHECK(sl_prompt_queue_get_mode(sl, 1, &info.mode) == SL_OK &&
        info.mode == SL_PROMPT_QUEUE_MODE_STEER);
  CHECK(sl_prompt_queue_take(sl, 1, &text) == SL_OK && !strcmp(text, "steer"));
  CHECK(sl_last_prompt_id(sl) == steer);
  sl_free_string(sl, text);
  CHECK(sl->prompt_queue_approve(sl, held) == SL_OK);
  text = sl_next_prompt(sl, "> ", &source);
  CHECK(text && !strcmp(text, "updated") && sl_last_prompt_id(sl) == held);
  sl_free_string(sl, text);
  CHECK(sl_prompt_queue_approve(sl, held) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_reject(sl, steer) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_append(sl, "reject", 1, &next) == SL_OK && next > old);
  CHECK(sl_prompt_queue_reject(sl, next) == SL_OK &&
        sl_prompt_queue_count(sl) == 0);
  CHECK(sl_prompt_queue_append(sl, "clear", 1, &old) == SL_OK && old > next);
  CHECK(sl_prompt_queue_clear(sl) == SL_OK);
  CHECK(sl_prompt_queue_append(sl, "new", 0, &next) == SL_OK && next > old);
  CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK && info.id == 0);
  CHECK(sl_prompt_queue_info(sl, 99, &info) == SL_ERROR_INVALID &&
        info.id == 0);
  CHECK(sl_prompt_queue_append(sl, "bad", 2, &old) == SL_ERROR_INVALID &&
        old == 0);
  CHECK(sl_prompt_queue_approve(sl, 0) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_reject(sl, 0) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_info(NULL, 0, &info) == SL_ERROR_INVALID &&
        info.id == 0);
  CHECK(sl_prompt_queue_draft_info(NULL, &info) == SL_ERROR_INVALID &&
        info.id == 0);
  CHECK(sl_prompt_queue_approve(NULL, 1) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_reject(NULL, 1) == SL_ERROR_INVALID);
  CHECK(sl_last_prompt_id(NULL) == 0);
  CHECK(sl_prompt_queue_info(sl, 0, NULL) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_draft_info(sl, NULL) == SL_ERROR_INVALID);
  sl_destroy(sl);
  close(input[0]);
  close(output[0]);
  close(output[1]);
  return 0;
}

static int injection_policy(void) {
  sl_config_t cfg;
  sl_t *sl, *other;
  sl_prompt_id_t queued, steer, explicit_hold, next;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_queue_delivery_t delivery;
  int enabled = -1;
  char *text = NULL;
  sl_config_init(&cfg);
  cfg.prompt_queue = 1;
  cfg.prompt_queue_max_entries = 3;
  sl = sl_create_with_config(&cfg);
  other = sl_create_with_config(&cfg);
  CHECK(sl && other && sl->inject_queued_prompt && sl->inject_steer_prompt &&
        sl->set_auto_approval && sl->get_auto_approval);
  CHECK(sl_get_auto_approval(sl, &enabled) == SL_OK && enabled == 0);
  CHECK(sl->inject_queued_prompt(sl, "queued", &queued) == SL_OK);
  CHECK(sl->inject_steer_prompt(sl, "steer", &steer) == SL_OK);
  CHECK(sl_prompt_queue_append(sl, "explicit hold", 1, &explicit_hold) ==
        SL_OK);
  CHECK(sl_prompt_queue_take(sl, 0, &text) == SL_ERROR_INVALID && !text);
  CHECK(sl_inject_queued_prompt(sl, "full", &next) == SL_ERROR_FULL && !next);
  CHECK(sl_prompt_queue_info(sl, 1, &info) == SL_OK && info.id == steer &&
        info.held && info.mode == SL_PROMPT_QUEUE_MODE_STEER);
  CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  CHECK(sl_set_auto_approval(sl, 1) == SL_OK);
  CHECK(sl_get_prompt_queue_delivery(sl, &delivery) == SL_OK &&
        delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL);
  CHECK(sl->get_auto_approval(sl, &enabled) == SL_OK && enabled == 1);
  CHECK(sl_get_auto_approval(other, &enabled) == SL_OK && !enabled);
  CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == queued &&
        !info.held);
  CHECK(sl_prompt_queue_info(sl, 1, &info) == SL_OK && info.id == steer &&
        !info.held && info.mode == SL_PROMPT_QUEUE_MODE_STEER);
  CHECK(sl_prompt_queue_info(sl, 2, &info) == SL_OK &&
        info.id == explicit_hold && !info.held);
  CHECK(sl_set_auto_approval(sl, 1) == SL_OK &&
        sl_set_auto_approval(sl, 0) == SL_OK);
  CHECK(sl_prompt_queue_take(sl, 0, &text) == SL_OK && !strcmp(text, "queued"));
  sl_free_string(sl, text);
  CHECK(sl_inject_queued_prompt(sl, "new hold", &next) == SL_OK);
  CHECK(sl_prompt_queue_info(sl, 2, &info) == SL_OK && info.id == next &&
        info.held);
  CHECK(sl_set_auto_approval(sl, 1) == SL_OK &&
        sl_prompt_queue_clear(sl) == SL_OK);
  CHECK(sl_inject_steer_prompt(sl, "future steer", &next) == SL_OK);
  CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && !info.held &&
        info.mode == SL_PROMPT_QUEUE_MODE_STEER);
  CHECK(sl_inject_queued_prompt(sl, "future queued", &next) == SL_OK);
  CHECK(sl_prompt_queue_info(sl, 1, &info) == SL_OK && !info.held &&
        info.mode == SL_PROMPT_QUEUE_MODE_QUEUED);
  CHECK(sl_prompt_queue_append(sl, "future explicit", 1, NULL) == SL_OK);
  CHECK(sl_prompt_queue_info(sl, 2, &info) == SL_OK && !info.held);
  CHECK(sl_set_prompt_queue(sl, 0, 3, 3) == SL_OK);
  CHECK(sl_inject_steer_prompt(sl, "disabled", &next) == SL_ERROR_INVALID &&
        !next);
  CHECK(sl_get_auto_approval(sl, &enabled) == SL_OK && enabled);
  CHECK(sl_set_prompt_queue(sl, 1, 3, 3) == SL_OK);
  CHECK(sl_inject_queued_prompt(sl, NULL, &next) == SL_ERROR_INVALID && !next);
  CHECK(sl_inject_steer_prompt(sl, "", &next) == SL_ERROR_INVALID && !next);
  CHECK(sl_inject_queued_prompt(NULL, "x", &next) == SL_ERROR_INVALID && !next);
  CHECK(sl_inject_steer_prompt(NULL, "x", &next) == SL_ERROR_INVALID && !next);
  CHECK(sl_set_auto_approval(NULL, 1) == SL_ERROR_INVALID);
  CHECK(sl_set_auto_approval(sl, -1) == SL_ERROR_INVALID);
  CHECK(sl_set_auto_approval(sl, 2) == SL_ERROR_INVALID);
  CHECK(sl_get_auto_approval(NULL, &enabled) == SL_ERROR_INVALID);
  CHECK(sl_get_auto_approval(sl, NULL) == SL_ERROR_INVALID);
  sl_destroy(sl);
  sl_destroy(other);
  return 0;
}

enum edit_kind {
  TAB,
  ENTER,
  CALLBACK,
  STEER,
  APPROVE,
  REJECT,
  CANCEL,
  CLEAR_CANCEL,
  EOF_EDIT,
  FAIL_EDIT,
  INTERRUPT_EDIT,
  EMPTY_SUBMIT,
  PROMOTE,
  IDLE_APPROVE,
  BOUND_SUBMIT,
  BUSY_ENTER,
  NORMAL_EDIT,
  NORMAL_REQUEUE,
  NORMAL_EMPTY,
  SEARCH_REJECT,
  IDLE_ORDINARY,
  APPROVE_BUSY,
  AUTO_IDLE,
  AUTO_EDIT,
  AUTO_DRAFT,
  AUTO_PASTE,
  AUTO_SEARCH,
  DIRECT,
  FIFO_EDIT
};
struct edit {
  enum edit_kind kind;
  int master, slave, wake[2], step, failed;
  sl_prompt_id_t id, other;
};
#define REQUIRE(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "edit %d step %d line %d: %s\n", state->kind,            \
              state->step, __LINE__, #c);                                      \
      state->failed = 1;                                                       \
      (void)sl_cancel(sl);                                                     \
      return;                                                                  \
    }                                                                          \
  } while (0)

static int fail_key(sl_t *sl, sl_key_t key, void *data,
                    sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)data;
  (void)action;
  return SL_ERROR;
}

static int submit_key(sl_t *sl, sl_key_t key, void *data,
                      sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)data;
  *action = SL_KEY_ACTION_SUBMIT;
  return SL_OK;
}

static void edit_idle(sl_t *sl, void *data) {
  struct edit *state = data;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_id_t extra;
  char *text = NULL;
  int step = state->step++;
  if (step == 0) {
    if (state->kind == AUTO_PASTE || state->kind == AUTO_SEARCH) {
      const char *keys = state->kind == AUTO_PASTE ? "\033[200~" : "\022";
      REQUIRE(sl_set_auto_approval(sl, 1) == SL_OK);
      REQUIRE(write(state->master, keys, strlen(keys)) ==
              (ssize_t)strlen(keys));
    } else if (state->kind == AUTO_IDLE) {
      REQUIRE(sl_set_auto_approval(sl, 1) == SL_OK);
    } else if (state->kind == IDLE_APPROVE) {
      REQUIRE(sl_prompt_queue_approve(sl, state->id) == SL_OK);
    } else if (state->kind == IDLE_ORDINARY) {
      REQUIRE(sl_prompt_queue_append(sl, "host ordinary", 0, &state->other) ==
              SL_OK);
    } else if (state->kind == APPROVE_BUSY) {
      REQUIRE(sl_prompt_queue_approve(sl, state->id) == SL_OK);
    } else if (state->kind == DIRECT || state->kind == AUTO_DRAFT) {
      REQUIRE(sl_set_buffer(sl, "ordinary") == SL_OK);
      REQUIRE((state->kind == AUTO_DRAFT
                   ? sl_set_auto_approval(sl, 1)
                   : sl_prompt_queue_approve(sl, state->id)) == SL_OK);
      REQUIRE(!strcmp(sl_buffer(sl), "ordinary"));
      REQUIRE(sl_submit(sl) == SL_OK);
    } else {
      const char *keys = state->kind == PROMOTE ? "\033\r" : "\033e";
      REQUIRE(write(state->master, keys, 2) == 2);
    }
    return;
  }
  if (state->kind == AUTO_PASTE || state->kind == AUTO_SEARCH) {
    REQUIRE(sl_prompt_queue_info(sl, 0, &info) == SL_OK && !info.held);
    REQUIRE(sl_buffer(sl)[0] == '\0');
    if (state->kind == AUTO_SEARCH)
      REQUIRE(sl->history_search_active(sl));
    if (step == 2) {
      const char *keys =
          state->kind == AUTO_PASTE ? "ordinary\033[201~\r" : "\007ordinary\r";
      REQUIRE(write(state->master, keys, strlen(keys)) ==
              (ssize_t)strlen(keys));
    }
    return;
  }
  if (state->kind == IDLE_APPROVE || state->kind == IDLE_ORDINARY ||
      state->kind == AUTO_IDLE)
    return;
  if (state->kind == APPROVE_BUSY) {
    REQUIRE(step == 1 && sl_prompt_queue_count(sl) == 1);
    REQUIRE(sl_set_status_busy(sl, 0) == SL_OK);
    return;
  }
  if (step == 1 && state->kind == PROMOTE) {
    REQUIRE(sl_prompt_queue_info(sl, 0, &info) == SL_OK &&
            info.id == state->id && info.held);
    REQUIRE(sl_prompt_queue_count(sl) == 1);
    REQUIRE(sl_set_buffer(sl, "ordinary") == SL_OK && sl_submit(sl) == SL_OK);
    return;
  }
  if (step == 1) {
    REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK &&
            info.id == state->id &&
            info.held ==
                (state->kind != NORMAL_EDIT && state->kind != NORMAL_REQUEUE &&
                 state->kind != NORMAL_EMPTY));
    REQUIRE(sl_prompt_queue_count(sl) == 0);
    REQUIRE(sl_set_prompt_queue(sl, 0, 2, 2) == SL_ERROR_INVALID);
    REQUIRE(sl_set_buffer(sl, "edited") == SL_OK);
    switch (state->kind) {
    case FIFO_EDIT:
      REQUIRE(sl_inject_steer_prompt(sl, "later approval", &state->other) ==
              SL_OK);
      REQUIRE(sl_prompt_queue_approve(sl, state->other) == SL_ERROR_INVALID);
      REQUIRE(sl_prompt_queue_reject(sl, state->other) == SL_ERROR_INVALID);
      REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK &&
              info.id == state->id && info.held);
      REQUIRE(!strcmp(sl_buffer(sl), "edited"));
      REQUIRE(write(state->master, "\r", 1) == 1);
      break;
    case APPROVE:
      REQUIRE(sl_prompt_queue_approve(sl, state->id) == SL_OK);
      REQUIRE(sl_submit(sl) == SL_OK);
      break;
    case AUTO_EDIT:
      REQUIRE(sl_inject_steer_prompt(sl, "another hold", &extra) == SL_OK);
      REQUIRE(sl_set_auto_approval(sl, 1) == SL_OK);
      REQUIRE(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == extra &&
              !info.held);
      REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK &&
              info.id == state->id && !info.held);
      REQUIRE(!strcmp(sl_buffer(sl), "edited"));
      REQUIRE(sl_submit(sl) == SL_OK);
      break;
    case NORMAL_EDIT:
      REQUIRE(sl_submit(sl) == SL_OK);
      break;
    case BOUND_SUBMIT:
      REQUIRE(sl_bind_key(sl, SL_KEY_ALT_M, submit_key, NULL) == SL_OK);
      REQUIRE(write(state->master, "\033m", 2) == 2);
      break;
    case SEARCH_REJECT:
      REQUIRE(write(state->master, "\022", 1) == 1);
      break;
    case REJECT:
      REQUIRE(sl_prompt_queue_reject(sl, state->id) == SL_OK);
      REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK && !info.id);
      REQUIRE(!strcmp(sl_buffer(sl), ""));
      REQUIRE(sl_set_buffer(sl, "ordinary") == SL_OK && sl_submit(sl) == SL_OK);
      break;
    case CANCEL:
      REQUIRE(sl_prompt_queue_append(sl, "other", 0, &extra) == SL_OK);
      REQUIRE(sl_prompt_queue_append(sl, "full", 0, &extra) == SL_ERROR_FULL &&
              !extra);
      REQUIRE(sl_set_prompt_queue(sl, 1, 1, 1) == SL_ERROR_INVALID);
      REQUIRE(sl_cancel(sl) == SL_OK);
      break;
    case CLEAR_CANCEL:
      REQUIRE(sl_prompt_queue_clear(sl) == SL_OK);
      REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK &&
              info.id == state->id && info.held);
      REQUIRE(sl_cancel(sl) == SL_OK);
      break;
    case EOF_EDIT:
      REQUIRE(sl_set_buffer(sl, "") == SL_OK);
      REQUIRE(write(state->master, "\004", 1) == 1);
      break;
    case FAIL_EDIT:
      REQUIRE(sl_bind_key(sl, SL_KEY_ALT_M, fail_key, NULL) == SL_OK);
      REQUIRE(write(state->master, "\033m", 2) == 2);
      break;
    case INTERRUPT_EDIT:
      REQUIRE(write(state->master, "\003", 1) == 1);
      break;
    case EMPTY_SUBMIT:
    case NORMAL_EMPTY:
      REQUIRE(sl_set_buffer(sl, "") == SL_OK && sl_submit(sl) == SL_OK);
      break;
    case CALLBACK:
      REQUIRE(sl_submit(sl) == SL_OK);
      break;
    default: {
      const char *keys = state->kind == TAB || state->kind == NORMAL_REQUEUE
                             ? "\t"
                         : state->kind == STEER ? "\033\r"
                                                : "\r";
      REQUIRE(write(state->master, keys, strlen(keys)) ==
              (ssize_t)strlen(keys));
      break;
    }
    }
    return;
  }
  REQUIRE(step == 2);
  if (state->kind == SEARCH_REJECT) {
    REQUIRE(sl_history_search_active(sl));
    REQUIRE(sl_prompt_queue_reject(sl, state->id) == SL_OK);
    REQUIRE(!sl_history_search_active(sl));
    REQUIRE(!strcmp(sl_buffer(sl), ""));
  } else if (state->kind == EMPTY_SUBMIT || state->kind == NORMAL_EMPTY) {
    REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK && !info.id);
    REQUIRE(sl_prompt_queue_count(sl) == 0);
    REQUIRE(sl_prompt_queue_reject(sl, state->id) == SL_ERROR_INVALID);
    REQUIRE(sl_last_prompt_id(sl) == 0);
  } else {
    if (state->kind == FIFO_EDIT) {
      REQUIRE(sl_prompt_queue_info(sl, 1, &info) == SL_OK &&
              info.id == state->other && info.held);
      REQUIRE(sl_prompt_queue_approve(sl, state->other) == SL_ERROR_INVALID);
      REQUIRE(sl_prompt_queue_reject(sl, state->other) == SL_ERROR_INVALID);
    }
    REQUIRE(sl_prompt_queue_info(sl, 0, &info) == SL_OK &&
            info.id == state->id &&
            info.held == (state->kind != NORMAL_REQUEUE));
    REQUIRE(info.mode == (state->kind == STEER || state->kind == NORMAL_REQUEUE
                              ? SL_PROMPT_QUEUE_MODE_STEER
                              : SL_PROMPT_QUEUE_MODE_QUEUED));
    REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK && !info.id);
    REQUIRE(sl_prompt_queue_peek(sl, 0, &text) == SL_OK &&
            !strcmp(text, "edited"));
    sl_free_string(sl, text);
    text = NULL;
    if (state->kind != NORMAL_REQUEUE) {
      REQUIRE(sl_prompt_queue_take(sl, 0, &text) == SL_ERROR_INVALID && !text);
      if (state->kind == FIFO_EDIT) {
        REQUIRE(sl_set_auto_approval(sl, 1) == SL_OK);
        REQUIRE(sl_prompt_queue_info(sl, 1, &info) == SL_OK &&
                info.id == state->other && !info.held &&
                info.mode == SL_PROMPT_QUEUE_MODE_STEER);
      } else {
        REQUIRE(sl_prompt_queue_approve(sl, state->id) == SL_OK);
      }
    }
    REQUIRE(sl_prompt_queue_take(sl, 0, &text) == SL_OK &&
            !strcmp(text, "edited"));
    REQUIRE(sl_last_prompt_id(sl) == state->id);
    sl_free_string(sl, text);
    if (state->kind == FIFO_EDIT) {
      REQUIRE(sl_prompt_queue_take(sl, 0, &text) == SL_OK &&
              !strcmp(text, "later approval"));
      REQUIRE(sl_last_prompt_id(sl) == state->other);
      sl_free_string(sl, text);
    }
  }
  REQUIRE(sl_set_buffer(sl, "ordinary") == SL_OK && sl_submit(sl) == SL_OK);
}

/* Advance after the preceding key sequence has been consumed, rather than
 * spending a UI idle interval on every assertion. Leave the wake byte pending
 * while terminal input is ready so the real editor still handles every key. */
static int edit_ready(sl_t *sl, const sl_watch_event_t *event, void *data) {
  struct edit *state = data;
  struct pollfd input;
  char byte;
  (void)event;
  input.fd = state->slave;
  input.events = POLLIN;
  if (poll(&input, 1, 0) != 0)
    return SL_OK;
  if (read(state->wake[0], &byte, 1) != 1)
    return SL_ERROR_IO;
  edit_idle(sl, data);
  return write(state->wake[1], "n", 1) == 1 ? SL_OK : SL_ERROR_IO;
}

static int edit_case(enum edit_kind kind, int native) {
  struct edit state;
  struct termios before, after;
  struct winsize size;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_source_t source;
  sl_config_t cfg;
  sl_watch_id_t watch;
  sl_t *sl;
  char *line, *original = NULL, bytes[32768];
  size_t used = 0;
  ssize_t count;
  int slave;
  memset(&state, 0, sizeof(state));
  state.kind = kind;
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(openpty(&state.master, &slave, NULL, NULL, &size) == 0);
  state.slave = slave;
  CHECK(pipe(state.wake) == 0 && write(state.wake[1], "n", 1) == 1);
  memset(&before, 0, sizeof(before));
  CHECK(tcgetattr(slave, &before) == 0);
  sl_config_init(&cfg);
  cfg.input_fd = cfg.output_fd = slave;
  cfg.prompt_queue = 1;
  cfg.prompt_queue_display_delay_ms = 0;
  cfg.prompt_queue_max_entries = 2;
  sl = sl_create_with_config(&cfg);
  CHECK(sl &&
        sl_prompt_queue_append(sl, "original",
                               kind != NORMAL_EDIT && kind != NORMAL_REQUEUE &&
                                   kind != NORMAL_EMPTY,
                               &state.id) == SL_OK);
  if (kind == NORMAL_REQUEUE)
    CHECK(sl_prompt_queue_set_mode(sl, 0, SL_PROMPT_QUEUE_MODE_STEER) == SL_OK);
  if (kind != IDLE_APPROVE && kind != IDLE_ORDINARY && kind != APPROVE_BUSY &&
      kind != DIRECT && kind != AUTO_IDLE && kind != AUTO_DRAFT &&
      kind != AUTO_PASTE && kind != AUTO_SEARCH)
    CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
          SL_OK);
  if (kind == STEER || kind == BUSY_ENTER || kind == APPROVE_BUSY ||
      kind == PROMOTE) {
    CHECK(sl_set_prompt_queue_profile(
              sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK);
    if (kind == STEER || kind == BUSY_ENTER || kind == APPROVE_BUSY)
      CHECK(sl_set_status_busy(sl, 1) == SL_OK);
  }
  if (native) {
    CHECK(write(state.master, "\033[1;1R", 6) == 6);
    CHECK(sl_output_stream_begin(sl) == SL_OK);
  }
  if (kind == IDLE_APPROVE || kind == APPROVE_BUSY || kind == AUTO_IDLE)
    CHECK(sl_set_idle_callback(sl, edit_idle, &state) == SL_OK);
  else
    CHECK(sl_watch_add(sl, state.wake[0], SL_WATCH_READ, edit_ready, &state,
                       &watch) == SL_OK);
  line = sl_next_prompt(sl, "> ", &source);
  CHECK(!state.failed);
  if (kind == CANCEL || kind == CLEAR_CANCEL || kind == EOF_EDIT ||
      kind == FAIL_EDIT || kind == INTERRUPT_EDIT) {
    sl_readline_status_t expected = kind == EOF_EDIT    ? SL_READLINE_EOF
                                    : kind == FAIL_EDIT ? SL_READLINE_ERROR
                                    : kind == INTERRUPT_EDIT
                                        ? SL_READLINE_INTERRUPTED
                                        : SL_READLINE_CANCELLED;
    CHECK(!line && sl_last_readline_status(sl) == expected);
    CHECK(sl_last_prompt_id(sl) == 0);
    CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK);
    CHECK(info.id == state.id && info.held);
    CHECK(sl_prompt_queue_peek(sl, 0, &original) == SL_OK);
    CHECK(!strcmp(original, "original"));
    sl_free_string(sl, original);
  } else {
    const char *expected =
        kind == APPROVE || kind == AUTO_EDIT || kind == NORMAL_EDIT ? "edited"
        : kind == IDLE_APPROVE || kind == APPROVE_BUSY || kind == AUTO_IDLE
            ? "original"
        : kind == IDLE_ORDINARY ? "host ordinary"
                                : "ordinary";
    CHECK(line && !strcmp(line, expected));
    CHECK(sl_last_prompt_id(sl) ==
          (kind == APPROVE || kind == AUTO_EDIT || kind == AUTO_IDLE ||
                   kind == IDLE_APPROVE || kind == APPROVE_BUSY ||
                   kind == NORMAL_EDIT
               ? state.id
           : kind == IDLE_ORDINARY ? state.other
                                   : 0));
    CHECK(source == (kind == IDLE_APPROVE || kind == AUTO_IDLE ||
                             kind == IDLE_ORDINARY || kind == APPROVE_BUSY
                         ? SL_PROMPT_SOURCE_QUEUED
                         : SL_PROMPT_SOURCE_DIRECT));
    sl_free_string(sl, line);
  }
  CHECK(sl_set_idle_callback(sl, NULL, NULL) == SL_OK);
  if (native)
    CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  memset(&after, 0, sizeof(after));
  CHECK(tcgetattr(slave, &after) == 0 && before.c_iflag == after.c_iflag &&
        before.c_oflag == after.c_oflag && before.c_cflag == after.c_cflag &&
        before.c_lflag == after.c_lflag &&
        !memcmp(before.c_cc, after.c_cc, sizeof(before.c_cc)));
  CHECK(fcntl(state.master, F_SETFL, O_NONBLOCK) == 0);
  while ((count = read(state.master, bytes + used, sizeof(bytes) - used - 1)) >
         0)
    used += (size_t)count;
  bytes[used] = 0;
  CHECK(strstr(bytes, kind == NORMAL_EDIT || kind == NORMAL_EMPTY ? "Q 1. "
                      : kind == NORMAL_REQUEUE ? "S 1. "
                                               : "1 approval") != NULL);
  if (kind != IDLE_APPROVE && kind != IDLE_ORDINARY && kind != APPROVE_BUSY &&
      kind != DIRECT && kind != PROMOTE && kind != NORMAL_EDIT &&
      kind != NORMAL_REQUEUE && kind != NORMAL_EMPTY && kind != AUTO_IDLE &&
      kind != AUTO_DRAFT && kind != AUTO_PASTE && kind != AUTO_SEARCH)
    CHECK(strstr(bytes, "H editing: approval required") != NULL);
  close(slave);
  close(state.master);
  close(state.wake[0]);
  close(state.wake[1]);
  return 0;
}

enum order_mutation {
  ORDER_UNCHANGED,
  ORDER_APPEND,
  ORDER_INSERT_BEFORE,
  ORDER_INSERT_AT,
  ORDER_TAKE,
  ORDER_REJECT,
  ORDER_CLEAR,
  ORDER_MIXED
};
enum order_finish {
  ORDER_CANCEL,
  ORDER_TAB,
  ORDER_EMPTY,
  ORDER_ENTER,
  ORDER_API,
  ORDER_EOF
};
struct order_edit {
  int master, slave, wake[2], phase, position;
  int growth;
  enum order_mutation mutation;
  enum order_finish finish;
  sl_prompt_id_t ids[256], edited;
};
static int order_ready(sl_t *sl, const sl_watch_event_t *event, void *data) {
  struct order_edit *state = data;
  sl_prompt_queue_entry_info_t info;
  struct pollfd input;
  char byte, bytes[4096], *text = NULL;
  sl_prompt_id_t ignored;
  int index, i;
  (void)event;
  input.fd = state->slave;
  input.events = POLLIN;
  if (poll(&input, 1, 0) != 0)
    return SL_OK;
  while (read(state->master, bytes, sizeof(bytes)) > 0) {
  }
  CHECK(read(state->wake[0], &byte, 1) == 1);
  if (state->phase++ == 0) {
    CHECK(write(state->master, "\033e", 2) == 2);
  } else if (state->phase == 2) {
    CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK &&
          info.id == state->edited && info.held == (state->position != 2) &&
          info.mode == SL_PROMPT_QUEUE_MODE_STEER);
    for (i = 0; i < state->growth; ++i) {
      char label[32];
      sprintf(label, "entry %d", i);
      if (i % 2) {
        CHECK(sl_prompt_queue_insert(sl, 0, label, 0, &state->ids[i]) == SL_OK);
        CHECK(sl_prompt_queue_set_mode(sl, 0, SL_PROMPT_QUEUE_MODE_STEER) ==
              SL_OK);
      } else {
        CHECK(sl_inject_queued_prompt(sl, label, &state->ids[i]) == SL_OK);
      }
    }
    if (state->growth) {
      CHECK(sl_set_prompt_queue(sl, 1, state->growth + 1, 2) == SL_OK);
      CHECK(sl_prompt_queue_append(sl, "full", 0, &ignored) == SL_ERROR_FULL &&
            !ignored);
    }
    index = state->position ? state->position - 1 : 0;
    switch (state->mutation) {
    case ORDER_APPEND:
      CHECK(sl_prompt_queue_append(sl, "D", 1, &state->ids['D']) == SL_OK);
      CHECK(sl_prompt_queue_append(sl, "full", 0, &ignored) == SL_ERROR_FULL &&
            !ignored);
      if (info.held) {
        CHECK(sl_prompt_queue_approve(sl, state->ids['D']) == SL_ERROR_INVALID);
        CHECK(sl_prompt_queue_reject(sl, state->ids['D']) == SL_ERROR_INVALID);
      }
      break;
    case ORDER_INSERT_BEFORE:
    case ORDER_INSERT_AT:
      if (state->mutation == ORDER_INSERT_AT)
        index = state->position;
      CHECK(sl_prompt_queue_insert(sl, (size_t)index, "X", 0,
                                   &state->ids['X']) == SL_OK);
      break;
    case ORDER_TAKE:
      CHECK(sl_prompt_queue_take(sl, (size_t)index, &text) == SL_OK);
      sl_free_string(sl, text);
      break;
    case ORDER_REJECT:
      CHECK(sl_prompt_queue_info(sl, (size_t)index, &info) == SL_OK);
      CHECK(sl_prompt_queue_reject(sl, info.id) == SL_OK);
      break;
    case ORDER_CLEAR:
      CHECK(sl_prompt_queue_clear(sl) == SL_OK);
      CHECK(sl_prompt_queue_append(sl, "D", 0, &state->ids['D']) == SL_OK);
      break;
    case ORDER_MIXED:
      CHECK(sl_prompt_queue_insert(sl, 0, "X", 0, &state->ids['X']) == SL_OK);
      CHECK(sl_prompt_queue_take(sl, 1, &text) == SL_OK);
      sl_free_string(sl, text);
      CHECK(sl_prompt_queue_append(sl, "D", 0, &state->ids['D']) == SL_OK);
      break;
    default:
      break;
    }
    CHECK(sl_prompt_queue_insert(sl, 99, "invalid", 0, &ignored) ==
              SL_ERROR_INVALID &&
          !ignored);
    CHECK(sl_set_buffer(sl, state->finish == ORDER_EMPTY ||
                                    state->finish == ORDER_EOF
                                ? ""
                                : "edited") == SL_OK);
    if (state->finish == ORDER_CANCEL)
      CHECK(sl_cancel(sl) == SL_OK);
    else if (state->finish == ORDER_API)
      CHECK(sl_prompt_queue_enqueue_draft(sl) == SL_OK);
    else
      CHECK(write(state->master,
                  state->finish == ORDER_TAB   ? "\t"
                  : state->finish == ORDER_EOF ? "\004"
                                               : "\r",
                  1) == 1);
  } else {
    CHECK(state->phase == 3);
    CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK && !info.id);
    CHECK(sl_cancel(sl) == SL_OK);
  }
  CHECK(write(state->wake[1], "n", 1) == 1);
  return SL_OK;
}
static int edit_order_case(int position, enum order_mutation mutation,
                           enum order_finish finish, int native) {
  static const char *const expected[3][8] = {
      {"CAB", "CABD", "CXAB", "CXAB", "CB", "CB", "CD", "CXBD"},
      {"CAB", "CABD", "XCAB", "CAXB", "AB", "AB", "AD", "XABD"},
      {"CAB", "CABD", "CXAB", "CABX", "CB", "CB", "BD", "XABD"}};
  struct order_edit state;
  struct winsize size;
  sl_prompt_queue_entry_info_t info;
  sl_config_t cfg;
  sl_watch_id_t watch;
  sl_t *sl;
  const char *order = expected[position][mutation];
  char remaining[8];
  char *line, *text = NULL;
  size_t i;
  memset(&state, 0, sizeof(state));
  state.position = position;
  state.mutation = mutation;
  state.finish = finish;
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(openpty(&state.master, &state.slave, NULL, NULL, &size) == 0);
  CHECK(fcntl(state.master, F_SETFL, O_NONBLOCK) == 0);
  CHECK(pipe(state.wake) == 0 && write(state.wake[1], "n", 1) == 1);
  sl_config_init(&cfg);
  cfg.prompt_queue = 1;
  cfg.prompt_queue_max_entries = 4;
  cfg.input_fd = cfg.output_fd = state.slave;
  sl = sl_create_with_config(&cfg);
  CHECK(sl && sl_set_prompt_queue_delivery(
                  sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
  CHECK(sl_prompt_queue_append(sl, "A", position == 1, &state.ids['A']) ==
        SL_OK);
  CHECK(sl_prompt_queue_append(sl, "B", 0, &state.ids['B']) == SL_OK);
  CHECK(sl_prompt_queue_insert(sl, 0, "C", position == 0, &state.ids['C']) ==
        SL_OK);
  state.edited = state.ids[(unsigned char)"CAB"[position]];
  if (finish == ORDER_EMPTY) {
    size_t read_index, written = 0;
    for (read_index = 0; order[read_index]; ++read_index)
      if (order[read_index] != "CAB"[position])
        remaining[written++] = order[read_index];
    remaining[written] = '\0';
    order = remaining;
  }
  CHECK(sl_prompt_queue_set_mode(sl, (size_t)position,
                                 SL_PROMPT_QUEUE_MODE_STEER) == SL_OK);
  if (native) {
    CHECK(write(state.master, "\033[1;1R", 6) == 6);
    CHECK(sl_output_stream_begin(sl) == SL_OK);
  }
  CHECK(sl_watch_add(sl, state.wake[0], SL_WATCH_READ, order_ready, &state,
                     &watch) == SL_OK);
  line = sl_next_prompt(sl, "> ", NULL);
  CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_CANCELLED);
  CHECK(state.phase == (finish == ORDER_CANCEL ? 2 : 3));
  CHECK(sl_prompt_queue_count(sl) == strlen(order));
  for (i = 0; order[i]; ++i) {
    unsigned char label = (unsigned char)order[i];
    const char *value = (state.ids[label] == state.edited &&
                         finish != ORDER_CANCEL && finish != ORDER_EMPTY)
                            ? "edited"
                            : NULL;
    CHECK(sl_prompt_queue_info(sl, i, &info) == SL_OK &&
          info.id == state.ids[label]);
    CHECK(info.mode == (info.id == state.edited ? SL_PROMPT_QUEUE_MODE_STEER
                                                : SL_PROMPT_QUEUE_MODE_QUEUED));
    CHECK(info.held == (info.id == state.edited
                            ? position != 2
                            : label == 'D' && mutation == ORDER_APPEND));
    CHECK(sl_prompt_queue_peek(sl, i, &text) == SL_OK);
    CHECK(value ? !strcmp(text, value) : text[0] == label && !text[1]);
    sl_free_string(sl, text);
    if (info.held)
      CHECK(sl_prompt_queue_approve(sl, info.id) == SL_OK);
  }
  for (i = 0; order[i]; ++i) {
    CHECK(sl_prompt_queue_take(sl, 0, &text) == SL_OK);
    CHECK(sl_last_prompt_id(sl) == state.ids[(unsigned char)order[i]]);
    sl_free_string(sl, text);
  }
  if (native)
    CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  close(state.master);
  close(state.slave);
  close(state.wake[0]);
  close(state.wake[1]);
  return 0;
}

/* Cross both geometric allocation boundaries and the default logical limit
 * while an edited entry owns a slot outside the visible queue. */
static int edit_capacity_case(int growth, int held, enum order_finish finish) {
  struct order_edit state;
  struct winsize size;
  sl_prompt_queue_entry_info_t info;
  sl_config_t cfg;
  sl_watch_id_t watch;
  sl_t *sl;
  char *line, *text = NULL, label[32];
  int j, i, inserted = growth / 2;
  int first = finish == ORDER_EMPTY;
  memset(&state, 0, sizeof(state));
  state.position = held ? 0 : 2;
  state.growth = growth;
  state.finish = finish;
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(openpty(&state.master, &state.slave, NULL, NULL, &size) == 0);
  CHECK(fcntl(state.master, F_SETFL, O_NONBLOCK) == 0);
  CHECK(pipe(state.wake) == 0 && write(state.wake[1], "n", 1) == 1);
  sl_config_init(&cfg);
  cfg.prompt_queue = 1;
  cfg.input_fd = cfg.output_fd = state.slave;
  sl = sl_create_with_config(&cfg);
  CHECK(sl && sl_set_prompt_queue_delivery(
                  sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
  CHECK(sl_prompt_queue_append(sl, "original", held, &state.edited) == SL_OK);
  CHECK(sl_prompt_queue_set_mode(sl, 0, SL_PROMPT_QUEUE_MODE_STEER) == SL_OK);
  CHECK(sl_watch_add(sl, state.wake[0], SL_WATCH_READ, order_ready, &state,
                     &watch) == SL_OK);
  line = sl_next_prompt(sl, "> ", NULL);
  CHECK(!line &&
        sl_last_readline_status(sl) ==
            (finish == ORDER_EOF ? SL_READLINE_EOF : SL_READLINE_CANCELLED));
  CHECK(state.phase == (finish == ORDER_CANCEL || finish == ORDER_EOF ? 2 : 3));
  CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK && !info.id);
  CHECK(sl_prompt_queue_count(sl) == (size_t)(growth + 1 - first));
  for (j = first; j <= growth; ++j) {
    CHECK(sl_prompt_queue_info(sl, (size_t)(j - first), &info) == SL_OK);
    if (j == 0) {
      CHECK(info.id == state.edited && info.held == held &&
            info.mode == SL_PROMPT_QUEUE_MODE_STEER);
      strcpy(label, finish == ORDER_CANCEL || finish == ORDER_EMPTY ||
                            finish == ORDER_EOF
                        ? "original"
                        : "edited");
    } else {
      /* Inserting at the reserved gap follows the edited entry. */
      i = j <= inserted ? 2 * (inserted - j) + 1 : 2 * (j - inserted - 1);
      CHECK(info.id == state.ids[i] && info.held == !(i % 2) &&
            info.mode == (i % 2 ? SL_PROMPT_QUEUE_MODE_STEER
                                : SL_PROMPT_QUEUE_MODE_QUEUED));
      sprintf(label, "entry %d", i);
    }
    CHECK(sl_prompt_queue_peek(sl, (size_t)(j - first), &text) == SL_OK &&
          !strcmp(text, label));
    sl_free_string(sl, text);
  }
  if (held && !first)
    CHECK(sl_prompt_queue_approve(sl, state.edited) == SL_OK);
  for (i = 0; i < growth; i += 2)
    CHECK(sl_prompt_queue_approve(sl, state.ids[i]) == SL_OK);
  for (j = first; j <= growth; ++j) {
    i = j <= inserted ? 2 * (inserted - j) + 1 : 2 * (j - inserted - 1);
    CHECK(sl_prompt_queue_take(sl, 0, &text) == SL_OK);
    CHECK(sl_last_prompt_id(sl) == (j == 0 ? state.edited : state.ids[i]));
    sl_free_string(sl, text);
  }
  sl_destroy(sl);
  close(state.master);
  close(state.slave);
  close(state.wake[0]);
  close(state.wake[1]);
  return 0;
}

int main(void) {
  static const int growth[] = {8, 16, 63};
  int kind, native, position, mutation, finish, held;
  size_t i;
  CHECK(signal(SIGINT, SIG_IGN) != SIG_ERR);
  if (queue_api() || injection_policy())
    return 1;
  for (i = 0; i < sizeof(growth) / sizeof(growth[0]); ++i)
    for (held = 0; held <= 1; ++held)
      for (finish = ORDER_CANCEL; finish <= ORDER_EOF; ++finish) {
        if (finish == ORDER_ENTER && !held)
          continue;
        if (edit_capacity_case(growth[i], held, (enum order_finish)finish)) {
          fprintf(stderr,
                  "Queue edit capacity growth=%d held=%d finish=%d failed\n",
                  growth[i], held, finish);
          return 1;
        }
      }
  /* Cover all mutations once, then repeat basic and mixed mutations with
   * native output; both paths use the same queue bookkeeping. */
  for (native = 0; native <= 1; ++native)
    for (position = 2; position >= 0; --position)
      for (mutation = ORDER_UNCHANGED; mutation <= ORDER_MIXED; ++mutation)
        for (finish = ORDER_CANCEL; finish <= ORDER_API; ++finish) {
          if ((finish == ORDER_ENTER && position == 2) ||
              (native && mutation != ORDER_UNCHANGED &&
               mutation != ORDER_MIXED))
            continue;
          if (edit_order_case(position, (enum order_mutation)mutation,
                              (enum order_finish)finish, native)) {
            fprintf(stderr,
                    "Queue edit order position=%d mutation=%d finish=%d "
                    "native=%d failed\n",
                    position, mutation, finish, native);
            return 1;
          }
        }
  for (native = 0; native <= 1; native++)
    for (kind = TAB; kind <= FIFO_EDIT; kind++)
      if (edit_case((enum edit_kind)kind, native)) {
        fprintf(stderr, "Queue approval scenario %d native=%d failed\n", kind,
                native);
        return 1;
      }
  puts("Per-entry approval, stable edit identity, mixed delivery and terminal "
       "modes passed.");
  return 0;
}
