#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)
enum scenario {
  APPROVE,
  REJECT,
  STEER,
  EDIT,
  EDIT_STEER,
  EDIT_EMPTY,
  EDIT_EMPTY_STEER,
  EDIT_EMPTY_BUSY,
  EDIT_EMPTY_ALT_ENTER,
  EDIT_EMPTY_CALLBACK,
  EDIT_EMPTY_REPEAT,
  CLEAR_DRAFT,
  SEND_DRAFT,
  PASTE,
  SEARCH,
  CANCEL,
  BUSY,
  BINDING,
  INTERRUPT,
  END_INPUT,
  CALLBACK_ERROR,
  AUTO_ENABLE,
  AUTO_CLEAR,
  HOST_CLEAR,
  APPROVE_CLEAR,
  AUTO_RETYPE,
  TYPE_NORMAL,
  TYPE_FOCUSED,
  ESC_BACK,
  ENTER_NORMAL,
  EDIT_FOCUSED,
  FOCUSED_BINDING,
  EDIT_OVERRIDE,
  MESSAGE_TRUNCATE,
  MESSAGE_THREE_LINES,
  MESSAGE_WRAP,
  MESSAGE_WORDS,
  MESSAGE_WORDS_FIT,
  MESSAGE_WORDS_UTF8,
  MESSAGE_WORDS_TABS,
  VISIBLE_FOCUS,
  ENTER_FOCUSED,
  ARROW_INPUT,
  UPPER_APPROVE,
  UPPER_REJECT,
  ALT_A_BACK,
  ALT_UNFOCUSED
};
struct ui {
  enum scenario kind;
  int master, phase, failed, questions, escape, input_started;
  int inverse;
  char csi[32];
  size_t csi_len;
  size_t used, mark;
  char text[65536];
  sl_prompt_id_t id, other, following;
};
static int timeout_master = -1;
static void timeout_input(int signo) {
  (void)signo;
  if (timeout_master >= 0)
    (void)write(timeout_master, "\025\004", 2);
}
#define REQUIRE(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "UI %d phase %d line %d: %s\n", state->kind,             \
              state->phase, __LINE__, #c);                                     \
      state->failed = 1;                                                       \
      (void)sl_cancel(sl);                                                     \
      return SL_OK;                                                            \
    }                                                                          \
  } while (0)
static int send_keys(struct ui *state, const char *keys) {
  return write(state->master, keys, strlen(keys)) == (ssize_t)strlen(keys);
}
static size_t index_of(sl_t *sl, sl_prompt_id_t id) {
  sl_prompt_queue_entry_info_t info;
  size_t i;
  for (i = 0; i < sl_prompt_queue_count(sl); i++)
    if (sl_prompt_queue_info(sl, i, &info) == SL_OK && info.id == id)
      return i;
  return i;
}
static int is_empty_edit(enum scenario kind) {
  return kind >= EDIT_EMPTY && kind <= EDIT_EMPTY_REPEAT;
}
static int is_edit(enum scenario kind) {
  return kind == EDIT || kind == EDIT_STEER || kind == EDIT_FOCUSED ||
         kind == EDIT_OVERRIDE || is_empty_edit(kind);
}
static int is_steer(enum scenario kind) {
  return kind == STEER || kind == EDIT_STEER || kind == EDIT_EMPTY_STEER;
}
static int is_preview(enum scenario kind) {
  return kind >= MESSAGE_TRUNCATE && kind <= MESSAGE_WORDS_TABS;
}
static int binding(sl_t *sl, sl_key_t key, void *data,
                   sl_key_action_t *action) {
  struct ui *state = data;
  (void)key;
  state->phase = 9;
  *action = SL_KEY_ACTION_CANCEL;
  return sl_cancel(sl);
}
static int ready(sl_t *sl, const sl_watch_event_t *event, void *data) {
  struct ui *state = data;
  sl_prompt_queue_entry_info_t info;
  char bytes[4096], *question, *taken = NULL;
  ssize_t count;
  size_t i;
  (void)event;
  while ((count = read(state->master, bytes, sizeof(bytes))) > 0) {
    for (i = 0; i < (size_t)count; i++) {
      unsigned char ch = (unsigned char)bytes[i];
      if (state->escape == 1) {
        state->escape = ch == '[' ? 2 : 0;
        state->csi_len = 0;
        continue;
      }
      if (state->escape == 2) {
        if (ch >= 0x40 && ch <= 0x7e) {
          state->csi[state->csi_len] = 0;
          if (ch == 'm' && !strcmp(state->csi, "1;7"))
            state->inverse++;
          state->escape = 0;
        } else if (state->csi_len + 1 < sizeof(state->csi))
          state->csi[state->csi_len++] = (char)ch;
        continue;
      }
      if (ch == 27) {
        state->escape = 1;
        continue;
      }
      if (ch < 32)
        continue;
      REQUIRE(state->used + 1 < sizeof(state->text));
      state->text[state->used++] = (char)ch;
    }
  }
  state->text[state->used] = 0;
  question = strstr(state->text + state->mark, "Alt-A");
  if (!strstr(state->text + state->mark, "Alt-E") ||
      (!strstr(state->text + state->mark, "Queued:") &&
       !strstr(state->text + state->mark, "Steering:")) ||
      !strstr(state->text, is_preview(state->kind) ? "message-line-00"
                           : is_edit(state->kind) &&
                                   !is_empty_edit(state->kind) &&
                                   state->phase == 2
                               ? "edited café"
                               : "approval message"))
    question = NULL;
  if ((state->kind == ESC_BACK || state->kind == ALT_A_BACK ||
       state->kind == VISIBLE_FOCUS) &&
      state->phase == 1 &&
      strstr(state->text + state->mark,
             "Alt-Y approve / Alt-N reject / Alt-E edit")) {
    REQUIRE(state->inverse > 0);
    REQUIRE(!strstr(state->text, "[FOCUSED]"));
    state->mark = state->used;
    REQUIRE(send_keys(state, state->kind == VISIBLE_FOCUS ? "\014\030\033y"
                             : state->kind == ALT_A_BACK  ? "\033a"
                                                          : "\033"));
    state->phase = state->kind == VISIBLE_FOCUS ? 3 : 2;
    return SL_OK;
  }
  if (state->kind == CLEAR_DRAFT || state->kind == SEND_DRAFT ||
      state->kind == PASTE || state->kind == SEARCH) {
    if (state->phase == 0) {
      REQUIRE(!question);
      if (!state->input_started) {
        REQUIRE(send_keys(state, state->kind == SEARCH ? "\022"
                                 : state->kind == PASTE
                                     ? "\033[200~ynpaste\033[201~"
                                     : "draftyn"));
        state->input_started = 1;
        return SL_OK;
      }
      if (state->kind == SEARCH) {
        if (!sl_history_search_active(sl))
          return SL_OK;
      } else if (strcmp(sl_buffer(sl),
                        state->kind == PASTE ? "ynpaste" : "draftyn")) {
        return SL_OK;
      }
      REQUIRE(sl_prompt_queue_append(sl, "approval message", 1, &state->id) ==
              SL_OK);
      state->phase = 1;
    }
    if (state->phase == 1) {
      REQUIRE(!question);
      if (state->kind == SEARCH) {
        if (!sl_history_search_active(sl))
          return SL_OK;
        REQUIRE(send_keys(state, "\007"));
      } else {
        const char *expected = state->kind == PASTE ? "ynpaste" : "draftyn";
        if (strcmp(sl_buffer(sl), expected))
          return SL_OK;
        REQUIRE(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.held);
        REQUIRE(send_keys(state, state->kind == SEND_DRAFT ? "\r" : "\025"));
      }
      state->phase = 2;
      return SL_OK;
    }
  }
  if (is_edit(state->kind) && state->phase == 1) {
    REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK);
    if (info.id != state->id)
      return SL_OK;
    REQUIRE(info.held);
    if (is_empty_edit(state->kind)) {
      REQUIRE(sl_inject_queued_prompt(sl, "later approval",
                                      &state->following) == SL_OK);
      REQUIRE(sl_prompt_queue_approve(sl, state->following) ==
              SL_ERROR_INVALID);
      REQUIRE(sl_prompt_queue_reject(sl, state->following) == SL_ERROR_INVALID);
      if (state->kind == EDIT_EMPTY_CALLBACK) {
        REQUIRE(sl_set_buffer(sl, "") == SL_OK && sl_submit(sl) == SL_OK);
      } else
        REQUIRE(
            send_keys(state, state->kind == EDIT_EMPTY_ALT_ENTER ? "\025\033\r"
                             : state->kind == EDIT_EMPTY_REPEAT  ? "\025\033e\r"
                                                                 : "\025\r"));
    } else {
      /* Exercise line navigation and deletion inside a held edit, preserving
       * its first line, identity and hold while trimming the second line. */
      REQUIRE(sl_set_buffer(sl, "edited café\nREMOVEsecond lineTRAIL") ==
              SL_OK);
      REQUIRE(send_keys(state, "\001\006\006\006\006\006\006\025\005"
                               "\002\002\002\002\002\013\031\037\014\r"));
    }
    state->phase = 2;
    return SL_OK;
  }
  if (question && state->phase != 3) {
    sl_prompt_id_t offered = is_empty_edit(state->kind) && state->phase == 2
                                 ? state->following
                                 : state->id;
    state->mark = state->used;
    state->questions++;
    REQUIRE(sl_prompt_queue_info(sl, index_of(sl, offered), &info) == SL_OK &&
            info.id == offered && info.held);
    REQUIRE(strstr(state->text, info.mode == SL_PROMPT_QUEUE_MODE_STEER
                                    ? "Steering:"
                                    : "Queued:"));
    if (is_edit(state->kind) && state->phase == 2) {
      if (is_empty_edit(state->kind)) {
        REQUIRE(sl_prompt_queue_draft_info(sl, &info) == SL_OK && !info.id);
        REQUIRE(sl_prompt_queue_count(sl) == 2);
        REQUIRE(index_of(sl, state->id) == sl_prompt_queue_count(sl));
        REQUIRE(strstr(state->text, "later approval"));
      } else
        REQUIRE(strstr(state->text, "edited café") &&
                strstr(state->text, "second line"));
    }
    if (is_preview(state->kind)) {
      REQUIRE(strstr(state->text, "message-line-00"));
      if (state->kind >= MESSAGE_WORDS) {
        const char *expected =
            state->kind == MESSAGE_WORDS_UTF8
                ? "  message-line-00  café café café  résumé fini..."
            : state->kind == MESSAGE_WORDS_TABS
                ? "  message-line-00  alpha beta  gamma delta..."
            : state->kind == MESSAGE_WORDS_FIT
                ? "  message-line-00  alpha beta gamma  delta epsilon"
                : "  message-line-00  alpha beta gamma  delta epsilon...";
        REQUIRE(strstr(state->text, expected));
        if (state->kind == MESSAGE_WORDS_FIT)
          REQUIRE(!strstr(state->text, "..."));
      } else {
        REQUIRE(strstr(state->text, "message-line-01"));
        REQUIRE(strstr(state->text, state->kind == MESSAGE_THREE_LINES
                                        ? "message-line-02"
                                        : "message-line-02..."));
        if (state->kind == MESSAGE_THREE_LINES)
          REQUIRE(!strstr(state->text, "..."));
      }
      REQUIRE(!strstr(state->text, "message-line-03"));
      REQUIRE(!strstr(state->text, "message-line-39"));
      REQUIRE(send_keys(state, "\033a\033[6~\033[5~\033a\033y"));
      state->phase = 3;
    } else if (state->kind == TYPE_NORMAL || state->kind == TYPE_FOCUSED ||
               state->kind == ENTER_NORMAL || state->kind == ENTER_FOCUSED ||
               state->kind == ARROW_INPUT || state->kind == ALT_UNFOCUSED ||
               ((state->kind == ESC_BACK || state->kind == ALT_A_BACK) &&
                state->phase == 2)) {
      REQUIRE(send_keys(state, state->kind == ENTER_NORMAL    ? "\r"
                               : state->kind == ENTER_FOCUSED ? "\033a\r"
                               : state->kind == ARROW_INPUT   ? "\033a\033[A\r"
                               : state->kind == ALT_UNFOCUSED
                                   ? "\033y\033nordinary\r"
                               : state->kind == TYPE_FOCUSED ? "\033ayn\r"
                                                             : "yn\r"));
      state->phase = 3;
    } else if (state->kind == ESC_BACK || state->kind == ALT_A_BACK ||
               state->kind == VISIBLE_FOCUS) {
      REQUIRE(send_keys(state, "\033a"));
      state->phase = 1;
    } else if (state->kind == CANCEL && state->phase == 0) {
      state->phase = 1;
      REQUIRE(sl_cancel(sl) == SL_OK);
    } else if (state->kind == INTERRUPT || state->kind == END_INPUT) {
      REQUIRE(send_keys(state, state->kind == INTERRUPT ? "\003" : "\004"));
      state->phase = 3;
    } else if (state->kind == CALLBACK_ERROR) {
      return SL_ERROR_IO;
    } else if (state->kind >= AUTO_CLEAR && state->kind <= AUTO_RETYPE) {
      REQUIRE(sl_set_buffer(sl, "protected draft") == SL_OK);
      if (state->kind == HOST_CLEAR) {
        sl_prompt_id_t old = state->id;
        REQUIRE(sl_prompt_queue_reject(sl, old) == SL_OK);
        REQUIRE(sl->set_auto_approval(sl, 1) == SL_OK);
        REQUIRE(sl->inject_queued_prompt(sl, "approval message", &state->id) ==
                SL_OK);
        REQUIRE(state->id > old);
      } else if (state->kind == APPROVE_CLEAR) {
        REQUIRE(sl_prompt_queue_approve(sl, state->id) == SL_OK);
      } else {
        REQUIRE(sl->set_auto_approval(sl, 1) == SL_OK);
      }
      REQUIRE(!strcmp(sl_buffer(sl), "protected draft"));
      REQUIRE(send_keys(state, state->kind == AUTO_RETYPE ? "\025continued\r"
                                                          : "\025"));
      state->phase = 3;
    } else if (state->kind == AUTO_ENABLE) {
      int enabled = 0;
      REQUIRE(sl->inject_steer_prompt(sl, "second pending request",
                                      &state->other) == SL_OK);
      REQUIRE(sl_prompt_queue_info(sl, 1, &info) == SL_OK && info.held);
      REQUIRE(sl->set_auto_approval(sl, 1) == SL_OK);
      REQUIRE(sl->get_auto_approval(sl, &enabled) == SL_OK && enabled);
      REQUIRE(sl_prompt_queue_info(sl, 0, &info) == SL_OK && !info.held &&
              info.id == state->id);
      REQUIRE(sl_prompt_queue_info(sl, 1, &info) == SL_OK && !info.held &&
              info.id == state->other &&
              info.mode == SL_PROMPT_QUEUE_MODE_STEER);
      state->phase = 3;
    } else if (is_edit(state->kind) && state->phase == 0) {
      REQUIRE(send_keys(state, state->kind == EDIT_FOCUSED ||
                                       is_empty_edit(state->kind)
                                   ? "\033a\033e"
                                   : "\033e"));
      state->phase = 1;
    } else {
      REQUIRE(send_keys(state, state->kind == REJECT ? "\033a\033nordinary\r"
                               : state->kind == UPPER_REJECT
                                   ? "\033a\033Nordinary\r"
                               : state->kind == UPPER_APPROVE ? "\033a\033Y"
                               : state->kind == BINDING       ? "y"
                                                              : "\033a\033y"));
      state->phase = 3;
    }
    return SL_OK;
  }
  if (state->phase == 3 &&
      (state->kind == STEER || is_edit(state->kind) || state->kind == BUSY ||
       state->kind == AUTO_ENABLE)) {
    sl_prompt_id_t offered =
        is_empty_edit(state->kind) ? state->following : state->id;
    REQUIRE(sl_prompt_queue_info(sl, index_of(sl, offered), &info) == SL_OK &&
            info.id == offered);
    if (info.held)
      return SL_OK;
    if (state->kind == BUSY) {
      REQUIRE(info.mode == SL_PROMPT_QUEUE_MODE_QUEUED);
      REQUIRE(sl_set_status_busy(sl, 0) == SL_OK);
    } else {
      REQUIRE(info.mode == (is_steer(state->kind) && !is_empty_edit(state->kind)
                                ? SL_PROMPT_QUEUE_MODE_STEER
                                : SL_PROMPT_QUEUE_MODE_QUEUED));
      REQUIRE(sl_prompt_queue_take(sl, index_of(sl, offered), &taken) == SL_OK);
      REQUIRE(
          !strcmp(taken, is_empty_edit(state->kind) ? "later approval"
                         : state->kind == STEER || state->kind == AUTO_ENABLE
                             ? "approval message"
                             : "edited café\nsecond line"));
      REQUIRE(sl_last_prompt_id(sl) == offered);
      sl_free_string(sl, taken);
      REQUIRE(sl_cancel(sl) == SL_OK);
    }
    state->phase = 4;
  }
  return SL_OK;
}
static int run(enum scenario kind, int native, int width) {
  struct ui state;
  sl_config_t config;
  sl_prompt_source_t source;
  sl_prompt_queue_entry_info_t info;
  sl_watch_id_t watch;
  struct termios before, after;
  struct winsize size;
  sl_t *sl;
  char *line;
  char message[1024] = "approval message";
  int i;
  int slave, deferred = kind >= CLEAR_DRAFT && kind <= SEARCH;
  memset(&state, 0, sizeof(state));
  memset(&size, 0, sizeof(size));
  state.kind = kind;
  if (kind == MESSAGE_TRUNCATE) {
    for (i = 0; i < 40; i++)
      (void)snprintf(message + (size_t)i * 16u,
                     sizeof(message) - (size_t)i * 16u, "message-line-%02d\n",
                     i);
  } else if (kind == MESSAGE_THREE_LINES) {
    strcpy(message, "message-line-00\nmessage-line-01\nmessage-line-02\n");
  } else if (kind == MESSAGE_WRAP) {
    strcpy(message, "message-line-00abcmessage-line-01def"
                    "message-line-02ghimessage-line-03jkl");
  } else if (kind == MESSAGE_WORDS || kind == MESSAGE_WORDS_FIT) {
    strcpy(message, "message-line-00 alpha beta gamma delta epsilon");
    if (kind == MESSAGE_WORDS)
      strcat(message, " zeta eta theta iota");
  } else if (kind == MESSAGE_WORDS_UTF8) {
    strcpy(message, "message-line-00 café café café résumé fini continued "
                    "and more words");
  } else if (kind == MESSAGE_WORDS_TABS) {
    strcpy(message,
           "message-line-00\talpha\tbeta\tgamma delta epsilon zeta eta");
  }
  size.ws_col = (unsigned short)width;
  size.ws_row = 24;
  CHECK(openpty(&state.master, &slave, NULL, NULL, &size) == 0);
  CHECK(tcgetattr(slave, &before) == 0);
  CHECK(fcntl(state.master, F_SETFL, O_NONBLOCK) == 0);
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  sl = sl_create_with_config(&config);
  CHECK(sl);
  if (is_edit(kind) || kind == AUTO_ENABLE)
    CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
          SL_OK);
  if (kind == BUSY || kind == EDIT_EMPTY_BUSY || kind == EDIT_EMPTY_ALT_ENTER) {
    CHECK(sl_set_prompt_queue_profile(
              sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK);
    CHECK(sl_set_status_busy(sl, 1) == SL_OK);
  }
  if (!deferred) {
    CHECK(sl_prompt_queue_append(sl, message, 1, &state.id) == SL_OK);
    if (is_steer(kind))
      CHECK(sl_prompt_queue_set_mode(sl, 0, SL_PROMPT_QUEUE_MODE_STEER) ==
            SL_OK);
    if (is_edit(kind)) {
      CHECK(sl_prompt_queue_append(sl, "newer ordinary", 0, &state.other) ==
            SL_OK);
      CHECK(sl_prompt_queue_set_mode(sl, 1, SL_PROMPT_QUEUE_MODE_STEER) ==
            SL_OK);
    }
  }
  if (native) {
    CHECK(sl_output_stream_begin(sl) == SL_OK);
  }
  if (kind == BINDING)
    CHECK(sl_bind_key(sl, (sl_key_t)'y', binding, &state) == SL_OK);
  if (kind == FOCUSED_BINDING || kind == EDIT_OVERRIDE || kind == ESC_BACK)
    CHECK(sl_bind_key(sl,
                      kind == ESC_BACK ? SL_KEY_ESCAPE
                      : kind == EDIT_OVERRIDE
                          ? SL_KEY_ALT_E
                          : (sl_key_t)(SL_KEY_ALT_BASE + 'y'),
                      binding, &state) == SL_OK);
  if (kind == SEARCH) {
    CHECK(sl_history_add(sl, "history draft") == SL_OK);
  } else if (kind == ARROW_INPUT) {
    CHECK(sl_history_add(sl, "history-draft") == SL_OK);
  }
  CHECK(sl_watch_add(sl, state.master, SL_WATCH_READ, ready, &state, &watch) ==
        SL_OK);
  timeout_master = state.master;
  alarm(3);
  line = sl_next_prompt(sl, "> ", &source);
  if (kind == SEND_DRAFT) {
    CHECK(line && !strcmp(line, "draftyn") &&
          source == SL_PROMPT_SOURCE_DIRECT);
    CHECK(sl_last_prompt_id(sl) == 0 && !state.questions);
    sl_free_string(sl, line);
    line = sl_next_prompt(sl, "> ", &source);
  } else if (kind == CANCEL) {
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_CANCELLED);
    CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.held &&
          info.id == state.id);
    line = sl_next_prompt(sl, "> ", &source);
  }
  alarm(0);
  timeout_master = -1;
  if (state.failed ||
      state.questions != (is_edit(kind) || kind == CANCEL || kind == ESC_BACK ||
                                  kind == ALT_A_BACK
                              ? 2
                              : 1))
    fprintf(stderr, "UI %d native %d phase %d questions %d: %s\n", kind, native,
            state.phase, state.questions, state.text);
  CHECK(!state.failed &&
        state.questions == (is_edit(kind) || kind == CANCEL ||
                                    kind == ESC_BACK || kind == ALT_A_BACK
                                ? 2
                                : 1));
  if ((kind >= TYPE_NORMAL && kind <= ENTER_NORMAL) || kind == ENTER_FOCUSED ||
      kind == ARROW_INPUT || kind == ALT_A_BACK || kind == ALT_UNFOCUSED) {
    CHECK(line &&
          !strcmp(line, kind == ENTER_NORMAL || kind == ENTER_FOCUSED ? ""
                        : kind == ARROW_INPUT   ? "history-draft"
                        : kind == ALT_UNFOCUSED ? "ordinary"
                        : kind == TYPE_FOCUSED  ? "yn"
                                                : "yn"));
    CHECK(source == SL_PROMPT_SOURCE_DIRECT && sl_last_prompt_id(sl) == 0);
    CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.held &&
          info.id == state.id);
    sl_free_string(sl, line);
  } else if (kind == STEER || is_edit(kind) || kind == BINDING ||
             kind == INTERRUPT || kind == END_INPUT || kind == CALLBACK_ERROR ||
             kind == AUTO_ENABLE) {
    CHECK(!line && sl_last_readline_status(sl) ==
                       (kind == INTERRUPT        ? SL_READLINE_INTERRUPTED
                        : kind == END_INPUT      ? SL_READLINE_EOF
                        : kind == CALLBACK_ERROR ? SL_READLINE_ERROR
                                                 : SL_READLINE_CANCELLED));
    if (kind == BINDING)
      CHECK(state.phase == 9 && sl_prompt_queue_info(sl, 0, &info) == SL_OK &&
            info.held);
    if (kind == INTERRUPT || kind == END_INPUT || kind == CALLBACK_ERROR)
      CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.held &&
            info.id == state.id);
  } else {
    CHECK(line &&
          !strcmp(line, kind == REJECT || kind == UPPER_REJECT ? "ordinary"
                        : kind == AUTO_RETYPE                  ? "continued"
                                                               : message));
    CHECK(source ==
          (kind == REJECT || kind == UPPER_REJECT || kind == AUTO_RETYPE
               ? SL_PROMPT_SOURCE_DIRECT
               : SL_PROMPT_SOURCE_QUEUED));
    CHECK(sl_last_prompt_id(sl) ==
          (kind == REJECT || kind == UPPER_REJECT || kind == AUTO_RETYPE
               ? 0
               : state.id));
    if (kind == AUTO_RETYPE)
      CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && !info.held &&
            info.id == state.id);
    sl_free_string(sl, line);
  }
  if (native)
    CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  CHECK(tcgetattr(slave, &after) == 0 && before.c_lflag == after.c_lflag &&
        before.c_iflag == after.c_iflag && before.c_oflag == after.c_oflag);
  close(slave);
  close(state.master);
  return 0;
}

/* A real external terminal driver: the library handle has no idle callback,
 * watch, or application binding to make its approval question progress. */
static int without_watch(int queued, sl_prompt_theme_t theme,
                         const char *style) {
  int master, slave, status, focused = 0;
  pid_t child;
  struct winsize size;
  char output[16384];
  char expected[64];
  size_t used = 0;
  ssize_t count;
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
  child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    sl_config_t cfg;
    sl_t *sl;
    sl_prompt_id_t id;
    char *line, *taken = NULL;
    close(master);
    sl_config_init(&cfg);
    cfg.prompt_queue = 1;
    cfg.prompt_theme = theme;
    cfg.input_fd = cfg.output_fd = slave;
    sl = sl_create_with_config(&cfg);
    if (!sl || sl_prompt_queue_append(sl, "question", 1, &id) != SL_OK)
      _exit(2);
    line = queued ? sl_next_prompt(sl, "> ", NULL) : sl_readline(sl, "> ");
    if (!line || strcmp(line, queued ? "question" : "ordinary"))
      _exit(3);
    if (!queued && (sl_last_prompt_id(sl) != 0 ||
                    sl_prompt_queue_take(sl, 0, &taken) != SL_OK ||
                    strcmp(taken, "question")))
      _exit(4);
    sl_free_string(sl, line);
    sl_free_string(sl, taken);
    sl_destroy(sl);
    close(slave);
    _exit(0);
  }
  close(slave);
  timeout_master = master;
  alarm(3);
  while (used + 1 < sizeof(output) &&
         (count = read(master, output + used, sizeof(output) - used - 1)) > 0) {
    used += (size_t)count;
    output[used] = 0;
    if (!focused && strstr(output, "Alt-A focus") &&
        strstr(output, "question")) {
      (void)snprintf(expected, sizeof(expected), "%sA ", style);
      CHECK(strstr(output, expected));
      (void)snprintf(expected, sizeof(expected), "%squestion", style);
      CHECK(strstr(output, expected));
      CHECK(write(master, "\033a", 2) == 2);
      focused = 1;
    } else if (focused &&
               strstr(output, "Alt-Y approve / Alt-N reject / Alt-E edit")) {
      const char *keys = queued ? "\033y" : "\033yordinary\r";
      CHECK(!strstr(output, "[FOCUSED]"));
      CHECK(strstr(output, "\033[1;7mA \033[1;7m"));
      CHECK(write(master, keys, strlen(keys)) == (ssize_t)strlen(keys));
      break;
    }
  }
  CHECK(waitpid(child, &status, 0) == child);
  alarm(0);
  timeout_master = -1;
  close(master);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return 0;
}
int main(int argc, char **argv) {
  static const char *const queue_styles[] = {"",
                                             "\033[38;2;244;114;182m",
                                             "\033[38;2;255;184;108m",
                                             "\033[38;2;211;134;155m",
                                             "\033[38;2;224;184;90m",
                                             "\033[38;2;51;255;51m",
                                             "\033[38;2;255;42;109m",
                                             "\033[38;2;255;139;139m",
                                             "\033[38;2;185;103;255m",
                                             "\033[34m"};
  int native, kind, theme;
  CHECK(signal(SIGALRM, timeout_input) != SIG_ERR);
  CHECK(signal(SIGINT, SIG_IGN) != SIG_ERR);
  if (argc > 1 && !strcmp(argv[1], "deferred")) {
    for (native = 0; native < 2; native++)
      for (kind = CLEAR_DRAFT; kind <= SEARCH; kind++)
        if (run((enum scenario)kind, native, 80))
          return 1;
    return 0;
  }
  if (argc > 1)
    return run(!strcmp(argv[1], "clear") ? AUTO_CLEAR : APPROVE, 0, 80);
  for (theme = SL_PROMPT_THEME_PLAIN; theme <= SL_PROMPT_THEME_DEFAULT; ++theme)
    CHECK(!without_watch(0, (sl_prompt_theme_t)theme, queue_styles[theme]) &&
          !without_watch(1, (sl_prompt_theme_t)theme, queue_styles[theme]));
  for (native = 0; native < 2; native++)
    for (kind = APPROVE; kind <= ALT_UNFOCUSED; kind++)
      if (run((enum scenario)kind, native,
              kind == EDIT ||
                      (kind >= MESSAGE_WRAP && kind <= MESSAGE_WORDS_TABS)
                  ? 20
                  : 80))
        return 1;
  puts("Approval controls, typing deferral, routing, editing and terminal "
       "restoration passed.");
  return 0;
}
