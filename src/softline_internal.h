#ifndef SOFTLINE_INTERNAL_H
#define SOFTLINE_INTERNAL_H

#include "softline/softline.h"
#include "softline_completion.h"
#include "softline_edit.h"
#include "softline_history.h"
#include "softline_surface.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define SL_BUF_INITIAL 256
#define SL_LINE_DEFAULT_MAX (128u * 1024u)
#define SL_HISTORY_DEFAULT_MAX 100
#define SL_PROMPT_QUEUE_DEFAULT_MAX 64
#define SL_PROMPT_QUEUE_DEFAULT_PREVIEWS 3
#define SL_ERROR_LEN 160
#define SL_DEFAULT_PROMPT "> "
#define SL_MAX_KEY_BINDINGS 64
#define SL_MAX_WATCHES 32
#define SL_PENDING_INPUT_MAX 32

typedef struct sl_key_binding {
  sl_key_t key;
  sl_key_callback_t callback;
  void *userdata;
} sl_key_binding_t;

typedef struct sl_history {
  char **items;
  int len;
  int max_len;
} sl_history_t;

typedef struct sl_prompt_queue_entry {
  char *text;
  sl_prompt_queue_entry_info_t info;
  int history_recorded;
} sl_prompt_queue_entry_t;

typedef struct sl_prompt_queue {
  sl_prompt_queue_entry_t *items;
  int len;
  int cap;
  int max_entries;
  int preview_entries;
  int enabled;
  int remote;
  sl_prompt_id_t serial;
  sl_prompt_id_t last_id;
  sl_prompt_queue_mode_t last_mode;
  int auto_approval;
  sl_prompt_id_t approval_id;
  int approval_focused;
  /* Own the original while its text is edited; its queue slot stays reserved.
   */
  sl_prompt_queue_entry_t editing;
  /* Reserved position among visible entries, adjusted by queue mutations.
   * An insertion at this position follows the reserved entry. */
  int editing_index;
  /* A queued-turns cancellation retains drafts but stops automatic FIFO
   * release until the user intentionally submits or promotes a turn. */
  int stopped;
  int host_controls_delivery;
  sl_prompt_queue_delivery_t delivery;
  sl_prompt_queue_profile_t profile;
  sl_prompt_queue_keys_t keys;
  sl_prompt_queue_policy_callback_t policy_callback;
  void *policy_userdata;
} sl_prompt_queue_t;

typedef struct sl_backend sl_backend_t;

typedef struct sl_statusline {
  char *elements[SL_STATUS_MAX_ELEMENTS];
  size_t count;
  size_t start_element;
  int enabled;
  int busy;
  int spinner;
  char idle_marker;
  int truncated;
  int spinner_frame;
  int spinner_time_valid;
  struct timeval spinner_time;
} sl_statusline_t;

typedef struct sl_watch {
  sl_watch_id_t id;
  int fd;
  unsigned int events;
  sl_watch_callback_t callback;
  void *userdata;
} sl_watch_t;

/* Logical prompt rows retained while the terminal reflows their physical
 * cells. Transcript bytes never enter this frame. */
typedef struct {
  char **lines;
  size_t *lens;
  int *cols;
  int rows;
  int editor_first;
  int cursor_row;
  int cursor_col;
} sl_reflow_frame_t;

typedef struct sl_history_search {
  char *saved;
  char *query;
  size_t query_len;
  size_t query_cap;
  int match_index;
  int has_match;
  char *prompt;
} sl_history_search_t;

typedef struct sl_impl {
  struct sl_pty *terminal_children;
  int terminal_active;
  int input_fd;
  int output_fd;
  int screen_width;
  int live_scroll_region;
  int clear_prompt_on_exit;
  int auto_scroll_pinned;
  int scroll_region_probe_pending;
  int output_stream_active;
  /* Feed calls never wait for a terminal cursor-position reply. */
  int output_write_active;
  int output_trailing_newlines;
  int output_ansi_state;
  /* Hold at most ESC [, 128 CSI bytes, and a terminator across writes. */
  char output_pending[132];
  size_t output_pending_len;
  sl_surface_t *output_surface;
  int cursor_position_probe;
  int probed_cursor_col;
  /* Last native teardown position, for terminals without cursor reports. */
  int native_cursor_valid;
  int native_cursor_below;
  int native_history_rows; /* Viewport shifts only; no transcript storage. */
  char *buf;
  size_t len;
  size_t cap;
  size_t line_max_len;
  int disable_image_paste;
  char *image_paste_path_template;
  size_t cursor;
  sl_edit_t edit;
  sl_completion_list_t completions;
  int preferred_column;
  int preferred_column_width;
  int preferred_column_valid;
  int control_x_pending;
  int raw_active;
  struct termios original_termios;
  tcflag_t original_output_oflag;
  int output_oflag_changed;
  sl_history_t history;
  /* Sole reverse-search mode flag; readable by application key bindings. */
  int history_search_active;
  char *history_key;
  sl_history_append_callback_t history_append;
  void *history_userdata;
  sl_history_store_t *history_store;
  int history_busy;
  int history_auto_add;
  sl_prompt_queue_t prompt_queue;
  sl_backend_t *backend;
  sl_prompt_theme_t prompt_theme;
  char *quoted_prompt_prefix;
  sl_quote_style_t quoted_prompt_style;
  int quoted_prompt_style_custom;
  sl_statusline_t statusline;
  char *status_message;
  char *status_message_prefix;
  sl_theme_color_t status_message_prefix_color;
  sl_theme_color_t status_message_text_color;
  int history_index;
  char *history_edit;
  int bracketed_paste;
  int cursor_hidden;
  int rendered_rows;
  int rendered_editor_first;
  int rendered_top_row;
  int rendered_cursor_row;
  int rendered_cursor_col;
  /* Logical cursor is part of the prompt frame; this tracks whether the
   * terminal cursor currently matches it. Output may invalidate only this. */
  int rendered_cursor_valid;
  /* Owner-requested repaint; cached geometry remains available for reflow. */
  int render_reset;
  int rendered_width;
  int rendered_height;
  int native_prompt_rows;
  /* First visual input row in the local editor viewport. Never producer text.
   */
  int editor_view_first;
  char **rendered_lines;
  size_t *rendered_lens;
  int *rendered_cols;
  int rendered_cap;
  sl_reflow_frame_t reflow_frame;
  const char *active_prompt;
  /* Owned prompt text only; producer transcript is never retained. */
  char *retained_prompt;
  int output_reconcile_active;
  int active_readline;
  int readline_suspended;
  sl_history_search_t suspended_search;
  int request_submit;
  int request_cancel;
  int request_suspend;
  int request_queue_dispatch;
  int plain_pending;
  char plain_pending_ch;
  char *pending_input;
  size_t pending_input_len;
  size_t pending_input_cap;
  sl_readline_status_t last_readline_status;
  sl_idle_callback_t idle_callback;
  void *idle_userdata;
  sl_watch_t watches[SL_MAX_WATCHES];
  sl_watch_id_t next_watch_id;
  /* Next registry slot considered first when dispatching ready watches. */
  unsigned int watch_dispatch_cursor;
  /* Nonzero while one or more watch callbacks are using this handle. */
  unsigned int watch_callback_depth;
  sl_key_binding_t key_bindings[SL_MAX_KEY_BINDINGS];
  char error[SL_ERROR_LEN];
} sl_impl_t;

void sl_terminal_reap(sl_impl_t *impl);
void sl_terminal_cleanup(sl_impl_t *impl);
void sl_terminal_abandon(sl_t *self);

/* Internal editor/cache operations shared with the transport-independent queue
 * adapter. These are not application queue admission or consumption seams. */
int sl_queue_cached_edit_begin(sl_t *self, int index);
void sl_queue_cached_edit_restore(sl_t *self);
int sl_backend_create(sl_t *self, const sl_config_t *config);
void sl_backend_destroy(sl_t *self);
int sl_backend_callback_active(const sl_impl_t *impl);
void sl_backend_pump(sl_t *self);
int sl_backend_submit(sl_t *self, sl_prompt_queue_mode_t mode);
int sl_backend_edit(sl_t *self, int index);
int sl_backend_promote_newest(sl_t *self);
int sl_backend_wait_edit(const sl_impl_t *impl);
int sl_backend_wait_keys(const sl_impl_t *impl);
int sl_backend_wait_key(const sl_impl_t *impl, int key);
int sl_backend_reading_paste(const sl_impl_t *impl);
int sl_backend_push_key(sl_t *self, int key, const char *bytes, size_t len,
                        int binding_done);
int sl_backend_pop_key(sl_impl_t *impl, int *key, char *bytes, size_t *len,
                       int *binding_done);
int sl_backend_action(sl_t *self, sl_prompt_id_t id,
                      sl_queue_operation_t operation);
int sl_backend_cancel_edit(sl_t *self);
void sl_backend_cancel_waiting(sl_impl_t *impl);
int sl_backend_unsupported(sl_t *self);
int sl_backend_recover_newest(sl_t *self);
int sl_backend_notice(const sl_impl_t *impl, char *text, size_t size);
int sl_queue_backend_request_impl(sl_t *, const sl_queue_request_t *,
                                  sl_queue_request_id_t *);
int sl_queue_backend_result_impl(sl_t *, const sl_queue_result_t *);
int sl_queue_backend_update_impl(sl_t *, const sl_queue_change_t *);
int sl_queue_backend_info_impl(const sl_t *, sl_queue_backend_info_t *);
int sl_queue_backend_entry_impl(const sl_t *, size_t, sl_queue_entry_t *);
int sl_queue_backend_pending_impl(const sl_t *, size_t,
                                  sl_queue_pending_info_t *);
int sl_queue_backend_recover_impl(sl_t *, sl_queue_request_id_t);

#endif
