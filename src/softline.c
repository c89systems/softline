/* Expose the platform's kernel tab-expansion flags alongside POSIX termios. */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "softline_clipboard.h"
#include "softline_internal.h"
#include "softline_unicode.h"
#include "softline_unicode_word.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/stat.h>

typedef struct sl_row {
  char *text;
  size_t len;
  size_t start;
  size_t end;
  int has_cursor;
  int base_col;
  int cols;
  size_t text_first, text_end;
  int leading_tab, trailing_tab;
} sl_row_t;

typedef struct sl_render {
  int width;
  sl_row_t *rows;
  int cap;
  int count;
  int cursor_row;
  int cursor_col;
  int editor_first;
  int editor_offset;
} sl_render_t;

typedef struct sl_rgb {
  unsigned char red;
  unsigned char green;
  unsigned char blue;
} sl_rgb_t;

typedef struct sl_theme_palette {
  sl_rgb_t elements[8];
  sl_rgb_t separator;
  sl_rgb_t prompt;
  sl_rgb_t secondary;
  int prompt_bold;
  sl_rgb_t input;
  int input_bold;
} sl_theme_palette_t;

static const sl_theme_palette_t sl_theme_palettes[] = {{{{0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0}},
                                                        {0, 0, 0},
                                                        {0, 0, 0},
                                                        {0, 0, 0},
                                                        0,
                                                        {0, 0, 0},
                                                        0},
                                                       {{{56, 189, 248},
                                                         {34, 211, 238},
                                                         {167, 139, 250},
                                                         {244, 114, 182},
                                                         {251, 191, 36},
                                                         {52, 211, 153},
                                                         {251, 146, 60},
                                                         {248, 113, 113}},
                                                        {100, 116, 139},
                                                        {6, 182, 212},
                                                        {148, 163, 184},
                                                        1,
                                                        {0, 0, 0},
                                                        0},
                                                       {{{139, 233, 253},
                                                         {241, 250, 140},
                                                         {189, 147, 249},
                                                         {255, 184, 108},
                                                         {80, 250, 123},
                                                         {255, 85, 85},
                                                         {255, 121, 198},
                                                         {195, 183, 201}},
                                                        {98, 114, 164},
                                                        {98, 114, 164},
                                                        {195, 183, 201},
                                                        0,
                                                        {0, 0, 0},
                                                        0},
                                                       {{{250, 189, 47},
                                                         {184, 187, 38},
                                                         {131, 165, 152},
                                                         {211, 134, 155},
                                                         {254, 128, 25},
                                                         {142, 192, 124},
                                                         {251, 73, 52},
                                                         {213, 196, 161}},
                                                        {102, 92, 84},
                                                        {184, 187, 38},
                                                        {213, 196, 161},
                                                        1,
                                                        {0, 0, 0},
                                                        0},
                                                       {{{224, 184, 90},
                                                         {224, 184, 90},
                                                         {224, 184, 90},
                                                         {224, 184, 90},
                                                         {224, 184, 90},
                                                         {224, 184, 90},
                                                         {224, 184, 90},
                                                         {224, 184, 90}},
                                                        {88, 83, 74},
                                                        {168, 118, 40},
                                                        {169, 152, 101},
                                                        1,
                                                        {255, 224, 138},
                                                        0},
                                                       {{{51, 255, 51},
                                                         {51, 255, 51},
                                                         {51, 255, 51},
                                                         {51, 255, 51},
                                                         {51, 255, 51},
                                                         {51, 255, 51},
                                                         {51, 255, 51},
                                                         {51, 255, 51}},
                                                        {79, 91, 79},
                                                        {22, 122, 31},
                                                        {95, 158, 111},
                                                        1,
                                                        {51, 255, 51},
                                                        1},
                                                       {{{0, 229, 255},
                                                         {248, 248, 242},
                                                         {157, 78, 221},
                                                         {255, 42, 109},
                                                         {199, 125, 255},
                                                         {0, 255, 204},
                                                         {184, 169, 201},
                                                         {122, 107, 143}},
                                                        {78, 69, 99},
                                                        {78, 69, 99},
                                                        {184, 169, 201},
                                                        0,
                                                        {0, 0, 0},
                                                        0},
                                                       {{{54, 249, 246},
                                                         {254, 222, 93},
                                                         {185, 103, 255},
                                                         {255, 139, 139},
                                                         {0, 240, 255},
                                                         {255, 0, 204},
                                                         {255, 126, 219},
                                                         {172, 164, 184}},
                                                        {72, 76, 105},
                                                        {255, 255, 255},
                                                        {172, 164, 184},
                                                        1,
                                                        {0, 0, 0},
                                                        0},
                                                       {{{255, 126, 219},
                                                         {248, 248, 242},
                                                         {54, 249, 246},
                                                         {185, 103, 255},
                                                         {54, 249, 246},
                                                         {255, 126, 219},
                                                         {179, 169, 192},
                                                         {130, 120, 156}},
                                                        {95, 89, 117},
                                                        {255, 126, 219},
                                                        {179, 169, 192},
                                                        1,
                                                        {0, 0, 0},
                                                        0},
                                                       {{{0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0},
                                                         {0, 0, 0}},
                                                        {0, 0, 0},
                                                        {0, 0, 0},
                                                        {0, 0, 0},
                                                        0,
                                                        {0, 0, 0},
                                                        0}};

static size_t sl_utf8_clamp_cluster_boundary(const char *buf, size_t len,
                                             size_t pos);
static size_t sl_utf8_decode(const char *buf, size_t len, size_t pos,
                             unsigned long *codepoint);
static size_t sl_utf8_next_cluster_len(const char *buf, size_t len, size_t pos);
static int sl_render_clear_active(sl_t *self);
static int sl_render_apply(sl_t *self, const char *prompt);
static void sl_output_track_tail(sl_impl_t *impl, const char *bytes,
                                 size_t length);
static int sl_write_cursor_pos(int fd, int row, int col);
static int sl_wstr(int fd, const char *s);
static int sl_theme_color_style(sl_prompt_theme_t theme, sl_theme_color_t color,
                                int italic, char *style, size_t size);
static int sl_try_pin_scroll_region(sl_t *self);
static void sl_refresh_scroll_region(sl_t *self);
static int sl_query_cursor_row(sl_t *self);
static int sl_read_cursor_report(sl_t *self, int request);
static int sl_cursor_report_begin(sl_t *self, struct termios *original);
static int sl_cursor_report_end(sl_t *self, const struct termios *original,
                                int temporary);
static int sl_enable_raw(sl_t *self);
static void sl_disable_raw(sl_t *self);
static int sl_output_write_validated(sl_t *self, const char *bytes,
                                     size_t length);

static sl_impl_t *sl_impl(sl_t *self) {
  if (!self)
    return NULL;
  return (sl_impl_t *)self->impl;
}

static const sl_impl_t *sl_impl_const(const sl_t *self) {
  if (!self)
    return NULL;
  return (const sl_impl_t *)self->impl;
}

static void sl_set_error(sl_t *self, const char *message) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl)
    return;
  if (!message)
    message = "";
  strncpy(impl->error, message, sizeof(impl->error) - 1);
  impl->error[sizeof(impl->error) - 1] = '\0';
}

static void sl_set_readline_status(sl_t *self, sl_readline_status_t status) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (impl)
    impl->last_readline_status = status;
}

static char *sl_strdup(const char *s) {
  size_t n;
  char *copy;
  if (!s)
    s = "";
  n = strlen(s) + 1;
  copy = (char *)malloc(n);
  if (copy)
    memcpy(copy, s, n);
  return copy;
}

static int sl_buf_set(sl_t *self, const char *s);

static void sl_prompt_queue_clear_raw(sl_prompt_queue_t *queue) {
  int i;
  if (!queue)
    return;
  for (i = 0; i < queue->len; i++)
    free(queue->items[i].text);
  queue->len = 0;
  queue->editing_index = 0;
  queue->approval_id = 0;
}

static int sl_prompt_queue_reserve(sl_prompt_queue_t *queue, int entries) {
  sl_prompt_queue_entry_t *items;
  int cap;
  if (!queue)
    return -1;
  if (entries <= queue->cap)
    return 0;
  cap = queue->cap > 0 ? queue->cap : 8;
  while (cap < entries) {
    if (cap > INT_MAX / 2) {
      cap = entries;
      break;
    }
    cap *= 2;
  }
  items = (sl_prompt_queue_entry_t *)realloc(queue->items,
                                             (size_t)cap * sizeof(*items));
  if (!items)
    return -1;
  queue->items = items;
  queue->cap = cap;
  return 0;
}

static int sl_prompt_queue_insert_raw(sl_impl_t *impl, int index,
                                      const char *text,
                                      sl_prompt_queue_mode_t mode, int held,
                                      sl_prompt_id_t id) {
  sl_prompt_queue_t *queue;
  char *copy;
  int i, entries;
  if (!impl || !text)
    return -1;
  queue = &impl->prompt_queue;
  if (index < 0 || index > queue->len)
    return -2;
  /* New entries must leave backing storage for allocation-free edit restore.
   * Requeueing the edit itself consumes that reserved slot instead. */
  entries = queue->len + (queue->editing.info.id && !id ? 1 : 0);
  if (entries >= queue->max_entries ||
      (!id && queue->serial == (sl_prompt_id_t)LONG_MAX))
    return 1;
  copy = sl_strdup(text);
  if (!copy)
    return -1;
  if (sl_prompt_queue_reserve(queue, entries + 1) != 0) {
    free(copy);
    return -1;
  }
  for (i = queue->len; i > index; i--)
    queue->items[i] = queue->items[i - 1];
  queue->items[index].text = copy;
  memset(&queue->items[index].info, 0, sizeof(queue->items[index].info));
  queue->items[index].info.id = id ? id : ++queue->serial;
  queue->items[index].info.mode = mode;
  queue->items[index].info.held = held && !queue->auto_approval;
  queue->items[index].info.policy.executable = 1;
  queue->items[index].info.stopped = queue->stopped;
  queue->items[index].history_recorded = 0;
  if (id) {
    queue->items[index].info = queue->editing.info;
    queue->items[index].info.mode = mode;
    queue->items[index].history_recorded = queue->editing.history_recorded;
  }
  if (queue->editing.info.id && index < queue->editing_index)
    queue->editing_index++;
  queue->len++;
  return 0;
}

static int sl_prompt_queue_append_raw(sl_impl_t *impl, const char *text,
                                      sl_prompt_queue_mode_t mode) {
  sl_prompt_queue_t *queue = &impl->prompt_queue;
  if (queue->editing.info.id && mode == SL_PROMPT_QUEUE_MODE_QUEUED)
    mode = queue->editing.info.mode;
  return sl_prompt_queue_insert_raw(impl, queue->len, text, mode,
                                    queue->editing.info.held,
                                    queue->editing.info.id);
}

static void sl_prompt_queue_finish_edit(sl_prompt_queue_t *queue) {
  free(queue->editing.text);
  memset(&queue->editing, 0, sizeof(queue->editing));
  queue->editing_index = 0;
}

/* The edited entry was appended only after its replacement was committed.
 * Restore its reserved position without allocation, keeping failure rollback
 * at the append slot. Queue positions are independent of arrival identities. */
static void sl_prompt_queue_restore_edit_order(sl_prompt_queue_t *queue) {
  sl_prompt_queue_entry_t entry = queue->items[queue->len - 1];
  int index = queue->editing_index;
  memmove(queue->items + index + 1, queue->items + index,
          (size_t)(queue->len - index - 1) * sizeof(*queue->items));
  queue->items[index] = entry;
}

static void sl_prompt_queue_finish_requeue(sl_prompt_queue_t *queue) {
  if (queue->editing.info.id)
    sl_prompt_queue_restore_edit_order(queue);
  if (queue->editing.info.held) {
    queue->approval_id = queue->editing.info.id;
    queue->approval_focused = 0;
  }
  sl_prompt_queue_finish_edit(queue);
}

static void sl_prompt_queue_schedule(sl_impl_t *impl);

/* Alt-E reserves its removed slot, so restoring the original cannot allocate.
 * In-progress edits are discarded on cancellation, EOF or read failure. */
void sl_queue_cached_edit_restore(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  sl_prompt_queue_t *queue = &impl->prompt_queue;
  queue->last_id = 0;
  if (!queue->editing.info.id)
    return;
  queue->items[queue->len++] = queue->editing;
  sl_prompt_queue_restore_edit_order(queue);
  memset(&queue->editing, 0, sizeof(queue->editing));
  queue->editing_index = 0;
  (void)sl_buf_set(self, "");
  sl_prompt_queue_schedule(impl);
}

static void sl_prompt_queue_restore_edit(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  /* EOF or terminal failure detaches presentation; it must not release a
   * backend hold. Deliberate cancellation sends ABORT explicitly below. */
  if (!impl->backend)
    sl_queue_cached_edit_restore(self);
}

static int sl_prompt_queue_replace_raw(sl_impl_t *impl, int index,
                                       const char *text) {
  sl_prompt_queue_t *queue;
  char *copy;
  if (!impl || !text)
    return -1;
  queue = &impl->prompt_queue;
  if (index < 0 || index >= queue->len)
    return -2;
  copy = sl_strdup(text);
  if (!copy)
    return -1;
  free(queue->items[index].text);
  queue->items[index].text = copy;
  return 0;
}

static char *sl_prompt_queue_take_raw(sl_prompt_queue_t *queue, int index) {
  char *item;
  int i;
  if (!queue || index < 0 || index >= queue->len)
    return NULL;
  item = queue->items[index].text;
  for (i = index; i + 1 < queue->len; i++)
    queue->items[i] = queue->items[i + 1];
  if (queue->editing.info.id && index < queue->editing_index)
    queue->editing_index--;
  queue->len--;
  return item;
}

/* One eligibility rule serves automatic dispatch, inspection, promotion and
 * host takes. Explicit fresh admissions may pass the stopped backlog, but
 * cannot resume it. Independent queries do not disturb other entries. */
static int sl_prompt_queue_blocks(const sl_prompt_queue_t *queue,
                                  const sl_prompt_queue_entry_info_t *before,
                                  const sl_prompt_queue_entry_info_t *after) {
  if (before->policy.independent || before->stopped)
    return 0;
  /* Suggestions do not hold ordinary user input. Steering keeps seam priority
   * over ordinary turns, but cannot pass older managed commands or waive its
   * own execution hold. */
  if (!before->policy.enqueue &&
      (before->held || (after->mode == SL_PROMPT_QUEUE_MODE_STEER &&
                        before->mode == SL_PROMPT_QUEUE_MODE_QUEUED)))
    return 0;
  return queue->policy_callback || after->policy.enqueue ||
         before->policy.enqueue || !before->policy.executable;
}

static int
sl_prompt_queue_blocked_before(const sl_prompt_queue_t *queue,
                               const sl_prompt_queue_entry_info_t *info,
                               int index) {
  int i;
  if (info->policy.independent)
    return 0;
  for (i = 0; i < index; i++)
    if (sl_prompt_queue_blocks(queue, &queue->items[i].info, info))
      return 1;
  return 0;
}

static int sl_prompt_queue_eligible(const sl_prompt_queue_t *queue, int index) {
  const sl_prompt_queue_entry_info_t *info = &queue->items[index].info;
  if (queue->remote)
    return 0;
  if (info->held || !info->policy.executable || info->stopped)
    return 0;
  if (info->policy.independent)
    return 1;
  if (queue->editing.info.id && index >= queue->editing_index &&
      sl_prompt_queue_blocks(queue, &queue->editing.info, info))
    return 0;
  return !sl_prompt_queue_blocked_before(queue, info, index);
}

static int sl_prompt_queue_first_queued(const sl_prompt_queue_t *queue) {
  int i;
  if (!queue || queue->remote || queue->host_controls_delivery)
    return -1;
  for (i = 0; i < queue->len; i++) {
    const sl_prompt_queue_entry_info_t *info = &queue->items[i].info;
    if (sl_prompt_queue_eligible(queue, i) &&
        (info->policy.enqueue ||
         (queue->delivery == SL_PROMPT_QUEUE_DELIVERY_AUTO &&
          info->mode == SL_PROMPT_QUEUE_MODE_QUEUED)))
      return i;
  }
  return -1;
}

static void sl_prompt_queue_schedule(sl_impl_t *impl) {
  impl->request_queue_dispatch =
      impl->active_readline &&
      sl_prompt_queue_first_queued(&impl->prompt_queue) >= 0;
}

/* Approval order follows arrival identity, including an entry temporarily
 * removed for editing. Requeueing or inserting cannot bypass that decision. */
static sl_prompt_id_t
sl_prompt_queue_approval_head(const sl_prompt_queue_t *queue) {
  sl_prompt_id_t id = queue->editing.info.held ? queue->editing.info.id : 0;
  int i;
  if (queue->remote) {
    for (i = 0; i < queue->len; ++i) {
      if (id && i >= queue->editing_index)
        return id;
      if (queue->items[i].info.held)
        return queue->items[i].info.id;
    }
    return id;
  }
  for (i = 0; i < queue->len; ++i)
    if (queue->items[i].info.held && (!id || queue->items[i].info.id < id))
      id = queue->items[i].info.id;
  return id;
}

static int
sl_prompt_queue_require_fifo(sl_t *self, const sl_impl_t *impl,
                             const sl_prompt_queue_entry_info_t *info) {
  if (info->held &&
      sl_prompt_queue_approval_head(&impl->prompt_queue) != info->id) {
    sl_set_error(self, "approval decisions must follow FIFO order");
    return SL_ERROR_INVALID;
  }
  return SL_OK;
}

/* An offered question stays visible when typing resumes. Eligibility to
 * acquire input is separate from the lifetime of that displayed entry. */
static int sl_prompt_held_index(const sl_impl_t *impl, sl_prompt_id_t id) {
  sl_prompt_id_t head;
  int i;
  if (!impl || !impl->prompt_queue.enabled || impl->prompt_queue.auto_approval)
    return -1;
  head = sl_prompt_queue_approval_head(&impl->prompt_queue);
  if (!head || (id && id != head))
    return -1;
  for (i = 0; i < impl->prompt_queue.len; i++)
    if (impl->prompt_queue.items[i].info.id == head)
      return i;
  return -1;
}

/* A question may acquire input only at an empty, ordinary editing boundary.
 * Selection happens in the input loop after checking terminal readiness. */
static int sl_prompt_approval_index(const sl_impl_t *impl, sl_prompt_id_t id) {
  if (!impl || !impl->active_readline || impl->len ||
      impl->prompt_queue.editing.info.id || impl->history_search_active ||
      impl->bracketed_paste)
    return -1;
  return sl_prompt_held_index(impl, id);
}

static char *sl_prompt_queue_deliver_raw(sl_prompt_queue_t *queue, int index) {
  sl_prompt_id_t id = queue->items[index].info.id;
  char *text;
  queue->last_mode = queue->items[index].info.mode;
  text = sl_prompt_queue_take_raw(queue, index);
  queue->last_id = text ? id : 0;
  return text;
}

static int sl_prompt_queue_enabled(const sl_impl_t *impl) {
  return impl && impl->prompt_queue.enabled;
}

/* The queued-turns preset uses the status busy signal as its turn lifecycle.
 * Keeping this policy here makes an owner-thread completion callback a single
 * state transition rather than an application-managed dequeue protocol. */
static void sl_prompt_queue_sync_queued_turns_delivery(sl_impl_t *impl) {
  if (!impl || impl->backend || !impl->prompt_queue.enabled ||
      impl->prompt_queue.profile != SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS)
    return;
  impl->prompt_queue.delivery =
      impl->statusline.busy || impl->prompt_queue.stopped ||
              impl->prompt_queue.host_controls_delivery
          ? SL_PROMPT_QUEUE_DELIVERY_MANUAL
          : SL_PROMPT_QUEUE_DELIVERY_AUTO;
  sl_prompt_queue_schedule(impl);
}

static void sl_prompt_queue_stop_queued_turns(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  int i;
  if (!impl || !impl->prompt_queue.enabled)
    return;
  if (impl->backend) {
    sl_queue_request_t request;
    memset(&request, 0, sizeof(request));
    request.operation = SL_QUEUE_STOP;
    (void)sl_queue_backend_request(self, &request, NULL);
    return;
  }
  if (impl->prompt_queue.profile != SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS &&
      !impl->prompt_queue.policy_callback &&
      !impl->prompt_queue.editing.info.policy.enqueue) {
    for (i = 0; i < impl->prompt_queue.len; i++)
      if (impl->prompt_queue.items[i].info.policy.enqueue)
        break;
    if (i == impl->prompt_queue.len)
      return;
  }
  impl->prompt_queue.stopped = 1;
  for (i = 0; i < impl->prompt_queue.len; ++i)
    impl->prompt_queue.items[i].info.stopped = 1;
  if (impl->prompt_queue.editing.info.id)
    impl->prompt_queue.editing.info.stopped = 1;
  if (impl->prompt_queue.profile == SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS)
    impl->prompt_queue.delivery = SL_PROMPT_QUEUE_DELIVERY_MANUAL;
  impl->request_queue_dispatch = 0;
}

static void sl_prompt_queue_resume_backlog(sl_impl_t *impl) {
  int i;
  impl->prompt_queue.stopped = 0;
  for (i = 0; i < impl->prompt_queue.len; ++i)
    impl->prompt_queue.items[i].info.stopped = 0;
  impl->prompt_queue.editing.info.stopped = 0;
  sl_prompt_queue_sync_queued_turns_delivery(impl);
  sl_prompt_queue_schedule(impl);
}

static void sl_prompt_queue_resume_on_user_turn(sl_impl_t *impl,
                                                sl_prompt_source_t source) {
  if (!impl || !impl->prompt_queue.stopped ||
      (source != SL_PROMPT_SOURCE_DIRECT &&
       source != SL_PROMPT_SOURCE_PROMOTED))
    return;
  sl_prompt_queue_resume_backlog(impl);
}

static int sl_prompt_queue_require_enabled(sl_t *self, sl_impl_t *impl) {
  if (!impl || !impl->prompt_queue.enabled) {
    sl_set_error(self, "prompt queue is not enabled");
    return SL_ERROR_INVALID;
  }
  return SL_OK;
}

static int sl_prompt_queue_index_valid(size_t index, int length,
                                       int allow_end) {
  if (index > (size_t)INT_MAX)
    return 0;
  if (allow_end)
    return (int)index <= length;
  return (int)index < length;
}

static void sl_statusline_clear(sl_statusline_t *statusline) {
  size_t i;
  if (!statusline)
    return;
  for (i = 0; i < SL_STATUS_MAX_ELEMENTS; i++) {
    free(statusline->elements[i]);
    statusline->elements[i] = NULL;
  }
  statusline->count = 0;
  statusline->truncated = 0;
}

static int sl_statusline_text_valid(const char *text) {
  size_t len;
  size_t pos;
  if (!text)
    return 1;
  len = strlen(text);
  pos = 0;
  while (pos < len) {
    unsigned long codepoint;
    size_t n;
    n = sl_utf8_decode(text, len, pos, &codepoint);
    if (n == 0 || ((unsigned char)text[pos] >= 0x80 && n == 1) ||
        codepoint < 32 || codepoint == 127 ||
        (codepoint >= 0x80 && codepoint <= 0x9f))
      return 0;
    pos += n;
  }
  return 1;
}

static int sl_quoted_prompt_text_valid(const char *text) {
  size_t len;
  size_t pos;
  if (!text)
    return 0;
  len = strlen(text);
  for (pos = 0; pos < len;) {
    unsigned long codepoint;
    size_t n = sl_utf8_decode(text, len, pos, &codepoint);
    if (n == 0 || ((unsigned char)text[pos] >= 0x80 && n == 1) ||
        (codepoint < 32 && codepoint != '\n' && codepoint != '\t') ||
        codepoint == 127 || (codepoint >= 0x80 && codepoint <= 0x9f))
      return 0;
    pos += n;
  }
  return 1;
}

static const sl_theme_palette_t *sl_theme_palette(sl_prompt_theme_t theme) {
  if (theme < SL_PROMPT_THEME_PLAIN || theme > SL_PROMPT_THEME_DEFAULT)
    return NULL;
  return &sl_theme_palettes[(int)theme];
}

static int sl_rgb_style(char *buf, size_t cap, sl_rgb_t rgb, int bold) {
  int n;
  n = snprintf(buf, cap, bold ? "\033[1;38;2;%u;%u;%um" : "\033[38;2;%u;%u;%um",
               (unsigned int)rgb.red, (unsigned int)rgb.green,
               (unsigned int)rgb.blue);
  return n > 0 && n < (int)cap ? 0 : -1;
}

static int sl_write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t n;
    n = write(fd, buf, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    buf += n;
    len -= (size_t)n;
  }
  return 0;
}

static int sl_wstr(int fd, const char *s) {
  if (!s)
    return 0;
  return sl_write_all(fd, s, strlen(s));
}

static int sl_wchar(int fd, char c) { return sl_write_all(fd, &c, 1); }

/* Let the output terminal's ONLCR setting perform carriage return expansion.
 * Explicit CRLF on a normal terminal becomes CRCRLF and visibly jolts a TUI. */
static int sl_write_line_break(sl_impl_t *impl) {
  struct termios termios_state;
  if (!impl)
    return -1;
  if (!isatty(impl->output_fd))
    return sl_wchar(impl->output_fd, '\n');
  if (isatty(impl->output_fd) &&
      tcgetattr(impl->output_fd, &termios_state) == 0 &&
      (termios_state.c_oflag & OPOST) != 0 &&
      (termios_state.c_oflag & ONLCR) != 0)
    return sl_wchar(impl->output_fd, '\n');
  return sl_wstr(impl->output_fd, "\r\n");
}

static int sl_write_cursor_up(int fd, int rows) {
  char seq[32];
  int n;
  if (rows <= 0)
    return 0;
  n = snprintf(seq, sizeof(seq), "\033[%dA", rows);
  if (n <= 0 || n >= (int)sizeof(seq))
    return -1;
  return sl_write_all(fd, seq, (size_t)n);
}

static int sl_write_cursor_down(int fd, int rows) {
  char seq[32];
  int n;
  if (rows <= 0)
    return 0;
  n = snprintf(seq, sizeof(seq), "\033[%dB", rows);
  if (n <= 0 || n >= (int)sizeof(seq))
    return -1;
  return sl_write_all(fd, seq, (size_t)n);
}

static int sl_write_cursor_forward(int fd, int cols) {
  char seq[32];
  int n;
  if (cols <= 0)
    return 0;
  n = snprintf(seq, sizeof(seq), "\033[%dC", cols);
  if (n <= 0 || n >= (int)sizeof(seq))
    return -1;
  return sl_write_all(fd, seq, (size_t)n);
}

static int sl_write_cursor_pos(int fd, int row, int col) {
  char seq[48];
  int n;
  if (row < 0)
    row = 0;
  if (col < 0)
    col = 0;
  n = snprintf(seq, sizeof(seq), "\033[%d;%dH", row + 1, col + 1);
  if (n <= 0 || n >= (int)sizeof(seq))
    return -1;
  return sl_write_all(fd, seq, (size_t)n);
}

/* A frame uses one geometry snapshot. Position native prompt cells from the
 * physical bottom even if the terminal resizes before its PTY hint arrives. */
static int sl_write_frame_cursor(sl_impl_t *impl, int height, int row,
                                 int col) {
  char seq[64];
  int count, below;
  if (!sl_surface_is_native(impl->output_surface))
    return sl_write_cursor_pos(impl->output_fd, row, col);
  sl_surface_native_release_cursor(impl->output_surface);
  below = height - 1 - row;
  count = snprintf(seq, sizeof(seq), "\033[65535;%dH", col + 1);
  if (below > 0)
    count +=
        snprintf(seq + count, sizeof(seq) - (size_t)count, "\033[%dA", below);
  return count > 0 && count < (int)sizeof(seq)
             ? sl_write_all(impl->output_fd, seq, (size_t)count)
             : -1;
}

static int sl_set_scroll_region(int fd, int top, int bottom) {
  char seq[48];
  int n;
  if (top < 0)
    top = 0;
  if (bottom < top)
    bottom = top;
  n = snprintf(seq, sizeof(seq), "\033[%d;%dr", top + 1, bottom + 1);
  if (n <= 0 || n >= (int)sizeof(seq))
    return -1;
  return sl_write_all(fd, seq, (size_t)n);
}

static int sl_reset_scroll_region(int fd) { return sl_wstr(fd, "\033[r"); }

static void sl_release_auto_scroll_region(sl_impl_t *impl) {
  if (!impl || !impl->auto_scroll_pinned)
    return;
  (void)sl_reset_scroll_region(impl->output_fd);
  impl->auto_scroll_pinned = 0;
}

static int sl_scroll_region_up(sl_impl_t *impl, int top, int bottom, int rows) {
  int i;
  int fd;
  if (!impl)
    return -1;
  fd = impl->output_fd;
  if (rows <= 0 || bottom < top)
    return 0;
  if (sl_set_scroll_region(fd, top, bottom) != 0 ||
      sl_write_cursor_pos(fd, bottom, 0) != 0)
    return -1;
  for (i = 0; i < rows; i++) {
    if (sl_write_line_break(impl) != 0) {
      (void)sl_reset_scroll_region(fd);
      return -1;
    }
  }
  return sl_reset_scroll_region(fd);
}

static int sl_enable_bracketed_paste(sl_impl_t *impl) {
  return sl_wstr(impl->output_fd, "\033[?2004h");
}

static void sl_disable_bracketed_paste(sl_impl_t *impl) {
  (void)sl_wstr(impl->output_fd, "\033[?2004l");
}

static int sl_hide_cursor(sl_impl_t *impl) {
  if (!impl || impl->cursor_hidden)
    return impl ? 0 : -1;
  if (sl_wstr(impl->output_fd, "\033[?25l") != 0)
    return -1;
  impl->cursor_hidden = 1;
  return 0;
}

static int sl_show_cursor(sl_impl_t *impl) {
  if (!impl || !impl->cursor_hidden)
    return impl ? 0 : -1;
  if (sl_wstr(impl->output_fd, "\033[?25h") != 0)
    return -1;
  impl->cursor_hidden = 0;
  return 0;
}

static int sl_terminal_columns(sl_impl_t *impl) {
  struct winsize ws;
  if (ioctl(impl->output_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return (int)ws.ws_col;
  return 80;
}

static int sl_terminal_rows(sl_impl_t *impl) {
  struct winsize ws;
  if (ioctl(impl->output_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    return (int)ws.ws_row;
  return 24;
}

static int sl_terminal_width(sl_impl_t *impl) {
  if (impl->auto_scroll_pinned || sl_surface_is_native(impl->output_surface) ||
      (impl->output_stream_active && isatty(impl->input_fd) &&
       isatty(impl->output_fd)))
    return sl_terminal_columns(impl);
  return impl->screen_width > 0 ? impl->screen_width
                                : sl_terminal_columns(impl);
}

static int sl_terminal_height(sl_impl_t *impl) {
  return sl_terminal_rows(impl);
}

static int sl_prompt_uses_absolute_rows(sl_impl_t *impl) {
  return impl && (impl->auto_scroll_pinned ||
                  sl_surface_is_native(impl->output_surface) ||
                  impl->output_stream_active);
}

/* Erase with the default background so blank cells do not acquire a full-width
 * extent that the terminal can reflow into extra rows on a later shrink. */
static int sl_clear_prompt_tail(sl_impl_t *impl) {
  return sl_wstr(impl->output_fd, "\033[0m\033[0K");
}

/* Callers position the cursor at column zero before clearing a whole row. */
static int sl_clear_prompt_row(sl_impl_t *impl) {
  return sl_clear_prompt_tail(impl);
}

static int sl_terminal_bottom(sl_impl_t *impl) {
  return sl_terminal_height(impl) - 1;
}

static int sl_prompt_top(sl_impl_t *impl, int prompt_rows) {
  int top, below;
  int height;
  height = sl_terminal_height(impl);
  if (prompt_rows < 1)
    prompt_rows = 1;
  if (prompt_rows > height)
    prompt_rows = height;
  top = sl_terminal_bottom(impl) - prompt_rows + 1;
  if (!impl->active_prompt && sl_surface_is_native(impl->output_surface) &&
      impl->rendered_rows > 0) {
    /* A retained, inactive frame follows native reflow. An active editor
     * stays at the current terminal bottom, including after height growth. */
    below =
        impl->rendered_height - impl->rendered_top_row - impl->rendered_rows;
    if (below > 0)
      top -= below;
    if (top < 2)
      top = 2;
  }
  if (top < 0)
    top = 0;
  return top;
}

static int sl_output_prompt_rows(sl_impl_t *impl) {
  int rows;
  int height;
  rows = impl->native_prompt_rows > 0 ? impl->native_prompt_rows : 1;
  height = sl_terminal_height(impl);
  /* DECSTBM requires at least two output rows. */
  if (height > 2 && rows > height - 2)
    rows = height - 2;
  return rows;
}

static int sl_native_resize_pending(sl_impl_t *impl) {
  return sl_surface_native_resize_pending(impl->output_surface);
}

/* Only query a cursor that is still physically owned by the producer. */
static int sl_native_observe_producer(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  int observed;
  if (!impl || impl->rendered_rows > 0 ||
      !sl_surface_native_cursor_live(impl->output_surface))
    return -1;
  observed = sl_query_cursor_row(self);
  if (observed > 0)
    sl_surface_native_observe(impl->output_surface, sl_terminal_columns(impl),
                              sl_terminal_rows(impl), observed - 1,
                              impl->probed_cursor_col);
  return observed;
}

static int sl_output_surface_reconcile(sl_t *self, int prompt_top) {
  sl_impl_t *impl = sl_impl(self);
  int width;
  if (!impl || !isatty(impl->input_fd) || !isatty(impl->output_fd))
    return 0;
  width = sl_terminal_columns(impl);
  /* No editor frame exists yet, so reserve no prompt rows. */
  if (!impl->active_prompt && impl->rendered_rows == 0)
    prompt_top = sl_terminal_rows(impl);
  if (sl_terminal_rows(impl) < 3 || prompt_top < 2 ||
      prompt_top > sl_terminal_rows(impl) ||
      (prompt_top == sl_terminal_rows(impl) &&
       (impl->active_prompt || impl->rendered_rows > 0))) {
    sl_set_error(self, "native output needs two scroll rows above the prompt");
    return -1;
  }
  if (!impl->output_surface) {
    int row, col;
    int was_raw = impl->raw_active;
    if (sl_enable_raw(self) != 0)
      return -1;
    row = sl_query_cursor_row(self);
    col = row > 0 ? impl->probed_cursor_col : 0;
    /* A confirmed line start supplies one boundary before the first quote.
     * Reopened sessions retain their known spacing instead. */
    if (row > 0 && col == 0 && !impl->native_cursor_valid &&
        impl->output_trailing_newlines == 0)
      impl->output_trailing_newlines = 1;
    row = row > 0 ? row - 1
          : impl->native_cursor_valid
              ? sl_terminal_rows(impl) - 1 - impl->native_cursor_below
              : 0;
    if (row < 0)
      row = 0;
    /* Native close leaves column zero. A nonzero startup report therefore
     * identifies an unfinished line written outside Softline between sessions.
     * Restart quote separation instead of trusting the old newline count.
     * This uses the existing startup probe; feed positioning stays unchanged.
     */
    if (impl->native_cursor_valid && col > 0)
      impl->output_trailing_newlines = 0;
    impl->output_surface = sl_surface_create_native(
        impl->output_fd, width, prompt_top, row, col,
        impl->native_cursor_valid ? impl->native_history_rows : -1);
    if (!impl->output_surface && !was_raw)
      sl_disable_raw(self);
    return impl->output_surface ? 0 : -1;
  }
  /* A readline width hint cannot move a live, full-terminal producer cursor. */
  if (sl_surface_native_cursor_live(impl->output_surface) &&
      prompt_top == sl_terminal_rows(impl) &&
      sl_surface_matches(impl->output_surface, width, prompt_top))
    return 0;
  /* A changed output region homes the hardware cursor, even if the prompt
   * text is unchanged. Its cached cell comparison must still restore it. */
  if (!sl_surface_matches(impl->output_surface, width, prompt_top))
    impl->rendered_cursor_valid = 0;
  {
    int observed = sl_native_observe_producer(self);
    int col, row, result;
    if (impl->output_reconcile_active && observed <= 0 &&
        sl_surface_native_cursor_live(impl->output_surface)) {
      sl_set_error(
          self, "terminal did not provide a cursor report for reconciliation");
      return -1;
    }
    sl_surface_native_position(impl->output_surface, NULL, &col);
    result = sl_surface_resize(impl->output_surface, width, prompt_top);
    if (result == 0 && observed > 0) {
      /* Margin changes home the hardware cursor. Retain the observed column
       * and the producer row after any actual prompt growth. */
      sl_surface_native_position(impl->output_surface, &row, NULL);
      sl_surface_native_observe(impl->output_surface, width,
                                sl_terminal_rows(impl), row, col);
    }
    return result;
  }
}

static tcflag_t sl_native_output_flags(tcflag_t flags) {
  /* The kernel counts ANSI bytes rather than terminal cursor movements.
   * Leave tabs to the terminal while retaining OPOST/ONLCR and other flags. */
#if defined(OXTABS)
  flags &= ~OXTABS;
#elif defined(TABDLY) && defined(TAB3)
  if ((flags & TABDLY) == TAB3)
    flags &= ~TABDLY;
#endif
  return flags;
}

static int sl_enable_raw(sl_t *self) {
  sl_impl_t *impl;
  struct termios raw, output;
  impl = sl_impl(self);
  if (!impl)
    return -1;
  if (impl->raw_active)
    return 0;
  if (!isatty(impl->input_fd)) {
    sl_set_error(self, "input file descriptor is not a terminal");
    errno = ENOTTY;
    return -1;
  }
  if (tcgetattr(impl->input_fd, &impl->original_termios) != 0) {
    sl_set_error(self, "failed to read terminal attributes");
    return -1;
  }
  raw = impl->original_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  /* Preserve output processing: native renderer newlines rely on ONLCR. */
  raw.c_oflag = sl_native_output_flags(raw.c_oflag);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(impl->input_fd, TCSAFLUSH, &raw) != 0) {
    sl_set_error(self, "failed to enable terminal raw mode");
    return -1;
  }
  impl->raw_active = 1;
  impl->output_oflag_changed = 0;
  /* stdin and stdout often share a TTY through different descriptors. Query
   * after setting input raw mode, so that shared TTY needs no second change.
   * A separate output TTY only lends us its output flags, not its input mode.
   */
  if (isatty(impl->output_fd)) {
    if (tcgetattr(impl->output_fd, &output) != 0) {
      sl_set_error(self, "failed to read output terminal attributes");
      sl_disable_raw(self);
      return -1;
    }
    impl->original_output_oflag = output.c_oflag;
    output.c_oflag = sl_native_output_flags(output.c_oflag);
    if (output.c_oflag != impl->original_output_oflag) {
      impl->output_oflag_changed = 1;
      if (tcsetattr(impl->output_fd, TCSANOW, &output) != 0) {
        sl_set_error(self, "failed to disable output kernel tab expansion");
        sl_disable_raw(self);
        return -1;
      }
    }
  }
  return 0;
}

static void sl_disable_raw(sl_t *self) {
  sl_impl_t *impl;
  struct termios output;
  impl = sl_impl(self);
  if (!impl || !impl->raw_active)
    return;
  (void)tcsetattr(impl->input_fd, TCSAFLUSH, &impl->original_termios);
  if (impl->output_oflag_changed && tcgetattr(impl->output_fd, &output) == 0) {
    output.c_oflag = impl->original_output_oflag;
    (void)tcsetattr(impl->output_fd, TCSANOW, &output);
  }
  impl->output_oflag_changed = 0;
  impl->raw_active = 0;
}

static int sl_buf_reserve(sl_t *self, size_t need) {
  sl_impl_t *impl;
  size_t cap;
  char *next;
  impl = sl_impl(self);
  if (!impl)
    return -1;
  if (need > impl->line_max_len + 1) {
    sl_set_error(self, "line exceeds configured maximum length");
    return -1;
  }
  if (need <= impl->cap)
    return 0;
  cap = impl->cap ? impl->cap : SL_BUF_INITIAL;
  while (cap < need) {
    if (cap > ((size_t)-1) / 2) {
      sl_set_error(self, "line buffer is too large");
      return -1;
    }
    cap *= 2;
  }
  next = (char *)realloc(impl->buf, cap);
  if (!next) {
    sl_set_error(self, "out of memory while growing line buffer");
    return -1;
  }
  impl->buf = next;
  impl->cap = cap;
  return 0;
}

static int sl_buf_replace(sl_t *self, size_t at, size_t removed,
                          const char *text, size_t inserted) {
  sl_impl_t *impl = sl_impl(self);
  size_t len;
  if (!impl || at > impl->len || removed > impl->len - at ||
      inserted > impl->line_max_len - (impl->len - removed)) {
    sl_set_error(self, "edit exceeds the line limit");
    return -1;
  }
  if (!removed && !inserted)
    return 0;
  if (removed == inserted && !memcmp(impl->buf + at, text, inserted))
    return 0;
  len = impl->len - removed + inserted;
  if (sl_buf_reserve(self, len + 1) != 0)
    return -1;
  if (!impl->history_search_active &&
      sl_edit_record(&impl->edit, at, impl->buf + at, removed, inserted,
                     impl->cursor) != 0) {
    sl_set_error(self, "out of memory while recording draft undo");
    return -1;
  }
  memmove(impl->buf + at + inserted, impl->buf + at + removed,
          impl->len - at - removed);
  if (inserted)
    memmove(impl->buf + at, text, inserted);
  impl->len = len;
  impl->buf[len] = '\0';
  impl->edit.changed = 1;
  impl->preferred_column_valid = 0;
  return 0;
}

static int sl_buf_set(sl_t *self, const char *s) {
  sl_impl_t *impl;
  size_t n;
  impl = sl_impl(self);
  if (!impl)
    return -1;
  n = s ? strlen(s) : 0;
  if (sl_buf_reserve(self, n + 1) != 0)
    return -1;
  /* A recalled/queued draft is a new editing context, not an undoable
   * mutation of the previous entry's identity. Public set_buffer is journaled.
   */
  if (!impl->history_search_active)
    sl_edit_clear_undo(&impl->edit);
  if (n > 0)
    memcpy(impl->buf, s, n);
  impl->buf[n] = '\0';
  impl->len = n;
  impl->cursor = n;
  impl->edit.changed = 1;
  impl->preferred_column_valid = 0;
  return 0;
}

static int sl_buf_insert(sl_t *self, size_t at, const char *s, size_t n) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || !s)
    return -1;
  if (at > impl->len)
    at = impl->len;
  return sl_buf_replace(self, at, 0, s, n);
}

static int sl_buf_delete(sl_t *self, size_t at, size_t n) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || at >= impl->len || n == 0)
    return 0;
  if (n > impl->len - at)
    n = impl->len - at;
  return sl_buf_replace(self, at, n, NULL, 0);
}

static int sl_buf_insert_cstr(sl_t *self, const char *text) {
  sl_impl_t *impl;
  size_t n;
  impl = sl_impl(self);
  if (!impl || !text)
    return SL_ERROR_INVALID;
  n = strlen(text);
  if (sl_buf_insert(self, impl->cursor, text, n) != 0)
    return SL_ERROR;
  impl->cursor += n;
  return SL_OK;
}

static int sl_buf_set_public(sl_t *self, const char *text) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl || !text)
    return SL_ERROR_INVALID;
  if (sl_buf_replace(self, 0, impl->len, text, strlen(text)) != 0)
    return SL_ERROR;
  if (impl->cursor != impl->len) {
    if (impl->edit.open && (impl->edit.command == SL_EDIT_INSERT ||
                            impl->edit.command == SL_EDIT_PASTE))
      impl->edit.group++;
    impl->edit.changed = 1;
    impl->edit.last_command = SL_EDIT_OTHER;
  }
  impl->cursor = impl->len;
  impl->preferred_column_valid = 0;
  impl->edit.yank_valid = 0;
  return SL_OK;
}

static const char *sl_buffer_method(const sl_t *self) {
  const sl_impl_t *impl;
  impl = sl_impl_const(self);
  if (!impl)
    return NULL;
  return impl->buf ? impl->buf : "";
}

static size_t sl_cursor_method(const sl_t *self) {
  const sl_impl_t *impl;
  impl = sl_impl_const(self);
  if (!impl)
    return 0;
  return impl->cursor;
}

static int sl_set_cursor_method(sl_t *self, size_t cursor) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl)
    return SL_ERROR_INVALID;
  if (cursor > impl->len)
    cursor = impl->len;
  impl->cursor = sl_utf8_clamp_cluster_boundary(impl->buf, impl->len, cursor);
  /* A watch may navigate while a typed UTF-8 key awaits continuation. That
   * key must start a new run without splitting a key callback's own group. */
  if (impl->edit.open && (impl->edit.command == SL_EDIT_INSERT ||
                          impl->edit.command == SL_EDIT_PASTE))
    impl->edit.group++;
  impl->edit.last_command = SL_EDIT_OTHER;
  impl->edit.changed = 1;
  impl->preferred_column_valid = 0;
  impl->edit.yank_valid = 0;
  return SL_OK;
}

static int sl_submit_method(sl_t *self) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || !impl->active_readline)
    return SL_ERROR_INVALID;
  impl->request_submit = 1;
  return SL_OK;
}

static int sl_cancel_method(sl_t *self) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || !impl->active_readline)
    return SL_ERROR_INVALID;
  impl->request_cancel = 1;
  return SL_OK;
}

static int sl_suspend_method(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl || !impl->active_readline)
    return SL_ERROR_INVALID;
  impl->request_suspend = 1;
  return SL_OK;
}

static size_t sl_line_start(const char *buf, size_t pos) {
  while (pos > 0 && buf[pos - 1] != '\n')
    pos--;
  return pos;
}

static size_t sl_line_end(const char *buf, size_t len, size_t pos) {
  while (pos < len && buf[pos] != '\n')
    pos++;
  return pos;
}

static int sl_word_at(const char *buf, size_t len, size_t pos) {
  unsigned long cp;
  (void)sl_utf8_decode(buf, len, pos, &cp);
  return sl_codepoint_is_word(cp);
}

static size_t sl_word_forward(const char *buf, size_t len, size_t pos) {
  while (pos < len && !sl_word_at(buf, len, pos))
    pos += sl_utf8_next_cluster_len(buf, len, pos);
  while (pos < len && sl_word_at(buf, len, pos))
    pos += sl_utf8_next_cluster_len(buf, len, pos);
  return pos;
}

static size_t sl_word_backward(const char *buf, size_t pos) {
  size_t at = 0, start = 0, last = 0;
  int in_word = 0;
  /* A forward cluster walk keeps this linear even for a long Unicode word. */
  while (at < pos) {
    int word = sl_word_at(buf, pos, at);
    if (word && !in_word)
      start = at;
    if (word)
      last = start;
    in_word = word;
    at += sl_utf8_next_cluster_len(buf, pos, at);
  }
  return last;
}

static size_t sl_whitespace_word_backward(const char *buf, size_t pos) {
  if (pos == 0)
    return 0;
  pos--;
  while (pos > 0 && isspace((unsigned char)buf[pos]))
    pos--;
  while (pos > 0 && !isspace((unsigned char)buf[pos - 1]))
    pos--;
  return pos;
}

static size_t sl_utf8_prev_len(const char *buf, size_t pos) {
  size_t start;
  if (pos == 0)
    return 0;
  start = pos - 1;
  while (start > 0 && (((unsigned char)buf[start] & 0xc0) == 0x80))
    start--;
  return pos - start;
}

static size_t sl_utf8_decode(const char *buf, size_t len, size_t pos,
                             unsigned long *codepoint) {
  unsigned char c;
  size_t n;
  size_t i;
  unsigned long cp;
  if (codepoint)
    *codepoint = 0xfffdUL;
  if (pos >= len)
    return 0;
  c = (unsigned char)buf[pos];
  if ((c & 0x80) == 0) {
    if (codepoint)
      *codepoint = c;
    return 1;
  }
  if ((c & 0xe0) == 0xc0) {
    n = 2;
    cp = (unsigned long)(c & 0x1f);
  } else if ((c & 0xf0) == 0xe0) {
    n = 3;
    cp = (unsigned long)(c & 0x0f);
  } else if ((c & 0xf8) == 0xf0) {
    n = 4;
    cp = (unsigned long)(c & 0x07);
  } else {
    return 1;
  }
  if (pos + n > len)
    return 1;
  for (i = 1; i < n; i++) {
    c = (unsigned char)buf[pos + i];
    if ((c & 0xc0) != 0x80)
      return 1;
    cp = (cp << 6) | (unsigned long)(c & 0x3f);
  }
  if ((n == 2 && cp < 0x80UL) || (n == 3 && cp < 0x800UL) ||
      (n == 4 && cp < 0x10000UL) || cp > 0x10ffffUL ||
      (cp >= 0xd800UL && cp <= 0xdfffUL))
    return 1;
  if (codepoint)
    *codepoint = cp;
  return n;
}

static int sl_codepoint_is_variation(unsigned long cp) {
  return (cp >= 0xfe00UL && cp <= 0xfe0fUL) ||
         (cp >= 0xe0100UL && cp <= 0xe01efUL);
}

static int sl_codepoint_accepts_emoji_modifier(unsigned long cp) {
  return (cp >= 0x1f000UL && cp <= 0x1faffUL &&
          !sl_codepoint_is_regional_indicator(cp)) ||
         cp == 0x261dUL || cp == 0x26f9UL || (cp >= 0x270aUL && cp <= 0x270dUL);
}

static size_t sl_utf8_cluster_len_width(const char *buf, size_t len, size_t pos,
                                        int *width) {
  size_t n;
  size_t step;
  unsigned long cp;
  int w;
  int emoji_sequence;
  int modifier_eligible;
  n = sl_utf8_decode(buf, len, pos, &cp);
  if (n == 0) {
    if (width)
      *width = 0;
    return 0;
  }
  w = sl_codepoint_width(cp);
  emoji_sequence = sl_codepoint_is_wide(cp);
  modifier_eligible = sl_codepoint_accepts_emoji_modifier(cp);
  if (sl_codepoint_is_regional_indicator(cp)) {
    step = sl_utf8_decode(buf, len, pos + n, &cp);
    if (step > 0 && sl_codepoint_is_regional_indicator(cp)) {
      n += step;
      w = 2;
    }
  }
  for (;;) {
    if (pos + n >= len)
      break;
    step = sl_utf8_decode(buf, len, pos + n, &cp);
    if (step == 0)
      break;
    if (cp < 0x300UL && cp != 0x200dUL && cp != 0x20e3UL)
      break;
    if (sl_codepoint_is_combining(cp) || sl_codepoint_is_variation(cp)) {
      if (cp == 0xfe0fUL)
        emoji_sequence = 1;
      n += step;
      continue;
    }
    if (cp >= 0x1f3fbUL && cp <= 0x1f3ffUL && modifier_eligible) {
      n += step;
      emoji_sequence = 1;
      modifier_eligible = 0;
      continue;
    }
    if (cp == 0x200dUL) {
      n += step;
      if (pos + n >= len)
        break;
      step = sl_utf8_decode(buf, len, pos + n, &cp);
      if (step == 0)
        break;
      n += step;
      emoji_sequence = 1;
      modifier_eligible = sl_codepoint_accepts_emoji_modifier(cp);
      continue;
    }
    if (cp == 0x20e3UL) {
      n += step;
      emoji_sequence = 1;
      continue;
    }
    break;
  }
  if (emoji_sequence && w < 2)
    w = 2;
  if (width)
    *width = w;
  return n;
}

static size_t sl_utf8_next_cluster_len(const char *buf, size_t len,
                                       size_t pos) {
  return sl_utf8_cluster_len_width(buf, len, pos, NULL);
}

static size_t sl_utf8_prev_cluster_len(const char *buf, size_t pos) {
  size_t scan;
  size_t last;
  size_t n;
  if (pos == 0)
    return 0;
  scan = 0;
  last = 0;
  while (scan < pos) {
    last = scan;
    n = sl_utf8_next_cluster_len(buf, pos, scan);
    if (n == 0)
      break;
    scan += n;
  }
  if (scan == pos)
    return pos - last;
  return sl_utf8_prev_len(buf, pos);
}

static size_t sl_utf8_clamp_cluster_boundary(const char *buf, size_t len,
                                             size_t pos) {
  size_t scan;
  size_t n;
  if (!buf || pos == 0)
    return 0;
  if (pos >= len)
    return len;
  scan = 0;
  while (scan < len) {
    if (scan == pos)
      return pos;
    n = sl_utf8_next_cluster_len(buf, len, scan);
    if (n == 0)
      break;
    if (pos < scan + n)
      return scan;
    scan += n;
  }
  return pos;
}

static void sl_history_clear(sl_history_t *history) {
  int i;
  if (!history)
    return;
  for (i = 0; i < history->len; i++)
    free(history->items[i]);
  free(history->items);
  history->items = NULL;
  history->len = 0;
}

static int sl_history_set_max(sl_t *self, int max_len) {
  sl_impl_t *impl;
  sl_history_t *history;
  char **items;
  int keep;
  int start;
  int i;
  impl = sl_impl(self);
  if (!impl || impl->history_busy || max_len < 0) {
    sl_set_error(self, "invalid history length");
    return SL_ERROR_INVALID;
  }
  history = &impl->history;
  if (max_len == history->max_len)
    return SL_OK;
  if (max_len == 0) {
    sl_history_clear(history);
    history->max_len = 0;
    return SL_OK;
  }
  keep = history->len < max_len ? history->len : max_len;
  start = history->len - keep;
  items = (char **)calloc((size_t)max_len, sizeof(*items));
  if (!items) {
    sl_set_error(self, "out of memory while resizing history");
    return SL_ERROR_NOMEM;
  }
  for (i = 0; i < keep; i++)
    items[i] = history->items[start + i];
  for (i = 0; i < start; i++)
    free(history->items[i]);
  free(history->items);
  history->items = items;
  history->len = keep;
  history->max_len = max_len;
  return SL_OK;
}

static int sl_history_add_memory(sl_t *self, const char *line) {
  sl_impl_t *impl;
  sl_history_t *history;
  char *copy;
  size_t len;
  int i;
  impl = sl_impl(self);
  if (!impl || !line)
    return SL_ERROR_INVALID;
  history = &impl->history;
  if (history->max_len == 0 || line[0] == '\0')
    return SL_OK;
  len = strlen(line);
  if (len > impl->line_max_len) {
    sl_set_error(self, "history entry exceeds configured maximum length");
    return SL_ERROR_INVALID;
  }
  if (!history->items) {
    history->items =
        (char **)calloc((size_t)history->max_len, sizeof(*history->items));
    if (!history->items) {
      sl_set_error(self, "out of memory while allocating history");
      return SL_ERROR_NOMEM;
    }
  }
  if (history->len > 0 && strcmp(history->items[history->len - 1], line) == 0)
    return SL_OK;
  copy = sl_strdup(line);
  if (!copy) {
    sl_set_error(self, "out of memory while adding history entry");
    return SL_ERROR_NOMEM;
  }
  if (impl->history_append && !impl->history_busy) {
    int status;
    impl->history_busy = 1;
    status =
        impl->history_append(impl->history_key, line, impl->history_userdata);
    impl->history_busy = 0;
    if (status != SL_OK) {
      free(copy);
      if (impl->history_store && status == SL_ERROR_IO)
        snprintf(impl->error, sizeof(impl->error),
                 "native history append failed: %s", strerror(errno));
      else
        sl_set_error(self, "history backend append failed");
      return status < 0 ? status : SL_ERROR;
    }
  }
  if (history->len == history->max_len) {
    free(history->items[0]);
    for (i = 1; i < history->len; i++)
      history->items[i - 1] = history->items[i];
    history->len--;
  }
  history->items[history->len] = copy;
  history->len++;
  return SL_OK;
}

/* Loading uses a staged receiver with no append hook. Public mutations are
 * rejected during callbacks, including reentrant calls from custom backends. */
static int sl_history_add_impl(sl_t *self, const char *line) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl || impl->history_busy) {
    sl_set_error(self, "history callback cannot reenter history methods");
    return SL_ERROR_INVALID;
  }
  return sl_history_add_memory(self, line);
}

static int sl_history_close_impl(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl || impl->history_busy) {
    sl_set_error(self, "cannot detach history during its callback");
    return SL_ERROR_INVALID;
  }
  free(impl->history_key);
  impl->history_key = NULL;
  impl->history_append = NULL;
  impl->history_userdata = NULL;
  sl_history_store_destroy(impl->history_store);
  impl->history_store = NULL;
  return SL_OK;
}

struct sl_history_import {
  sl_t receiver;
  sl_impl_t impl;
  int status;
};

static int sl_history_emit(void *context, const char *prompt) {
  struct sl_history_import *import = (struct sl_history_import *)context;
  if (import->status == SL_OK)
    import->status = sl_history_add_memory(&import->receiver, prompt);
  return import->status;
}

static int sl_history_set_backend_impl(sl_t *self, const char *key,
                                       sl_history_load_callback_t load,
                                       sl_history_append_callback_t append,
                                       void *userdata) {
  sl_impl_t *impl = sl_impl(self);
  struct sl_history_import import;
  char *copy;
  int i, status;
  if (!impl || impl->history_busy || !key || !*key || !append) {
    sl_set_error(self,
                 "history backend requires a nonempty key and append hook");
    return SL_ERROR_INVALID;
  }
  copy = sl_strdup(key);
  if (!copy)
    return SL_ERROR_NOMEM;
  memset(&import, 0, sizeof(import));
  import.receiver.impl = &import.impl;
  import.impl.line_max_len = impl->line_max_len;
  import.impl.history.max_len = impl->history.max_len;
  status = SL_OK;
  for (i = 0; i < impl->history.len && status == SL_OK; i++)
    status = sl_history_emit(&import, impl->history.items[i]);
  if (status == SL_OK && load) {
    impl->history_busy = 1;
    status = load(copy, sl_history_emit, &import, userdata);
    impl->history_busy = 0;
    if (status == SL_OK)
      status = import.status;
  }
  if (status != SL_OK) {
    free(copy);
    sl_history_clear(&import.impl.history);
    if (load == sl_history_store_load && status == SL_ERROR_IO)
      snprintf(impl->error, sizeof(impl->error),
               "native history load failed: %s", strerror(errno));
    else if (load == sl_history_store_load && status == SL_ERROR_INVALID)
      sl_set_error(self, "invalid or oversized native history record");
    else
      sl_set_error(self, "history backend load failed");
    return status < 0 ? status : SL_ERROR;
  }
  (void)sl_history_close_impl(self);
  sl_history_clear(&impl->history);
  impl->history = import.impl.history;
  impl->history_key = copy;
  impl->history_append = append;
  impl->history_userdata = userdata;
  return SL_OK;
}

static int sl_history_open_impl(sl_t *self, const char *key,
                                const char *directory) {
  sl_impl_t *impl = sl_impl(self);
  sl_history_store_t *store;
  int status;
  if (!impl || impl->history_busy)
    return SL_ERROR_INVALID;
  status = sl_history_store_create(key, directory, impl->line_max_len, &store);
  if (status != SL_OK) {
    if (status == SL_ERROR_INVALID)
      sl_set_error(self,
                   "history store requires a nonempty key and an absolute "
                   "directory or XDG_STATE_HOME/HOME");
    else if (status == SL_ERROR_IO)
      snprintf(impl->error, sizeof(impl->error),
               "cannot create private history directory: %s", strerror(errno));
    else
      sl_set_error(self, "out of memory setting up history store");
    return status;
  }
  status = sl_history_set_backend_impl(self, key, sl_history_store_load,
                                       sl_history_store_append, store);
  if (status == SL_OK)
    impl->history_store = store;
  else
    sl_history_store_destroy(store);
  return status;
}

static int sl_history_compact_impl(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  int status;
  if (!impl || impl->history_busy || !impl->history_store) {
    sl_set_error(self, "history compaction requires an attached native store");
    return SL_ERROR_INVALID;
  }
  status = sl_history_store_compact(impl->history_store, impl->history.max_len);
  if (status != SL_OK)
    sl_set_error(self, "native history compaction failed");
  return status;
}

static int sl_history_set_auto_add_impl(sl_t *self, int enabled) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl || impl->history_busy)
    return SL_ERROR_INVALID;
  impl->history_auto_add = enabled != 0;
  return SL_OK;
}

static int sl_history_search_active_impl(const sl_t *self) {
  const sl_impl_t *impl = sl_impl_const(self);
  return impl ? impl->history_search_active : 0;
}

static sl_key_binding_t *sl_find_key_binding(sl_impl_t *impl, sl_key_t key) {
  int i;
  if (!impl)
    return NULL;
  for (i = 0; i < SL_MAX_KEY_BINDINGS; i++) {
    if (impl->key_bindings[i].callback && impl->key_bindings[i].key == key)
      return &impl->key_bindings[i];
  }
  return NULL;
}

static int sl_bind_key_method(sl_t *self, sl_key_t key,
                              sl_key_callback_t callback, void *userdata) {
  sl_impl_t *impl;
  sl_key_binding_t *empty;
  int i;
  impl = sl_impl(self);
  if (!impl || key == SL_KEY_NONE)
    return SL_ERROR_INVALID;
  empty = NULL;
  for (i = 0; i < SL_MAX_KEY_BINDINGS; i++) {
    if (impl->key_bindings[i].callback && impl->key_bindings[i].key == key) {
      if (!callback) {
        impl->key_bindings[i].callback = NULL;
        impl->key_bindings[i].userdata = NULL;
      } else {
        impl->key_bindings[i].callback = callback;
        impl->key_bindings[i].userdata = userdata;
      }
      return SL_OK;
    }
    if (!empty && !impl->key_bindings[i].callback)
      empty = &impl->key_bindings[i];
  }
  if (!callback)
    return SL_OK;
  if (!empty) {
    sl_set_error(self, "too many key bindings");
    return SL_ERROR_NOMEM;
  }
  empty->key = key;
  empty->callback = callback;
  empty->userdata = userdata;
  return SL_OK;
}

static int sl_history_save_impl(sl_t *self, const char *filename) {
  sl_impl_t *impl;
  FILE *fp;
  int fd;
  int i;
  impl = sl_impl(self);
  if (!impl || impl->history_busy || !filename)
    return SL_ERROR_INVALID;
  fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    sl_set_error(self, "failed to open history file for writing");
    return SL_ERROR_IO;
  }
  if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
    close(fd);
    sl_set_error(self, "failed to set history file permissions");
    return SL_ERROR_IO;
  }
  fp = fdopen(fd, "w");
  if (!fp) {
    close(fd);
    sl_set_error(self, "failed to open history file for writing");
    return SL_ERROR_IO;
  }
  for (i = 0; i < impl->history.len; i++) {
    if (sl_history_write_record(fp, impl->history.items[i]) != SL_OK) {
      fclose(fp);
      sl_set_error(self, "failed to write history file");
      return SL_ERROR_IO;
    }
  }
  if (fclose(fp) != 0) {
    sl_set_error(self, "failed to close history file");
    return SL_ERROR_IO;
  }
  return SL_OK;
}

static int sl_history_load_impl(sl_t *self, const char *filename) {
  sl_impl_t *impl;
  FILE *fp;
  char *line;
  int had_too_long;
  int record;
  if (!self || !filename)
    return SL_ERROR_INVALID;
  impl = sl_impl(self);
  if (!impl || impl->history_busy)
    return SL_ERROR_INVALID;
  fp = fopen(filename, "r");
  if (!fp) {
    sl_set_error(self, "failed to open history file for reading");
    return SL_ERROR_IO;
  }
  line = (char *)malloc(impl->line_max_len + 1);
  if (!line) {
    fclose(fp);
    sl_set_error(self, "out of memory while loading history");
    return SL_ERROR_NOMEM;
  }
  had_too_long = 0;
  while ((record = sl_history_read_record(fp, line, impl->line_max_len + 1,
                                          0)) != 0) {
    if (record == SL_ERROR_IO) {
      free(line);
      fclose(fp);
      sl_set_error(self, "failed to read history file");
      return SL_ERROR_IO;
    }
    if (record == SL_ERROR_INVALID) {
      had_too_long = 1;
      sl_set_error(self, "invalid or oversized history record");
      continue;
    }
    {
      int status;
      impl->history_busy = 1;
      status = sl_history_add_memory(self, line);
      impl->history_busy = 0;
      if (status != SL_OK) {
        free(line);
        fclose(fp);
        return status;
      }
    }
  }
  free(line);
  if (ferror(fp)) {
    fclose(fp);
    sl_set_error(self, "failed to read history file");
    return SL_ERROR_IO;
  }
  if (fclose(fp) != 0) {
    sl_set_error(self, "failed to close history file");
    return SL_ERROR_IO;
  }
  if (had_too_long)
    return SL_ERROR_INVALID;
  return SL_OK;
}

static int sl_row_append(sl_row_t *row, const char *s, size_t n) {
  char *next;
  if (n == 0)
    return 0;
  next = (char *)realloc(row->text, row->len + n + 1);
  if (!next)
    return -1;
  row->text = next;
  memcpy(row->text + row->len, s, n);
  row->len += n;
  row->text[row->len] = '\0';
  return 0;
}

static int sl_row_append_cells(sl_row_t *row, const char *s, size_t n,
                               int cells) {
  if (sl_row_append(row, s, n) != 0)
    return -1;
  row->cols += cells;
  return 0;
}

static int sl_render_init(sl_t *self, sl_render_t *render) {
  if (!sl_impl(self))
    return -1;
  memset(render, 0, sizeof(*render));
  return 0;
}

static int sl_render_reserve_rows(sl_render_t *render, int rows) {
  sl_row_t *next;
  int cap;
  if (rows <= render->cap)
    return 0;
  cap = render->cap > 0 ? render->cap : 8;
  while (cap < rows) {
    if (cap > INT_MAX / 2) {
      cap = rows;
      break;
    }
    cap *= 2;
  }
  next = (sl_row_t *)realloc(render->rows, (size_t)cap * sizeof(*render->rows));
  if (!next)
    return -1;
  memset(next + render->cap, 0,
         (size_t)(cap - render->cap) * sizeof(*render->rows));
  render->rows = next;
  render->cap = cap;
  return 0;
}

static int sl_render_new_row_at(sl_render_t *render, size_t start,
                                int base_col) {
  if (sl_render_reserve_rows(render, render->count + 1) != 0)
    return -1;
  render->rows[render->count].text = NULL;
  render->rows[render->count].len = 0;
  render->rows[render->count].start = start;
  render->rows[render->count].end = start;
  render->rows[render->count].has_cursor = 0;
  render->rows[render->count].base_col = base_col;
  render->rows[render->count].cols = base_col;
  render->rows[render->count].text_first = start;
  render->rows[render->count].text_end = start;
  render->rows[render->count].leading_tab = 0;
  render->rows[render->count].trailing_tab = 0;
  render->count++;
  return 0;
}

static int sl_render_new_indented_row(sl_render_t *render, int indent,
                                      size_t start) {
  if (sl_render_new_row_at(render, start, indent) != 0)
    return -1;
  render->rows[render->count - 1].cols = 0;
  while (indent-- > 0) {
    if (sl_row_append_cells(&render->rows[render->count - 1], " ", 1, 1) != 0)
      return -1;
  }
  return 0;
}

static void sl_render_free(sl_render_t *render) {
  int i;
  for (i = 0; i < render->count; i++)
    free(render->rows[i].text);
  free(render->rows);
  render->rows = NULL;
  render->cap = 0;
  render->count = 0;
}

static void sl_reflow_frame_clear(sl_reflow_frame_t *frame) {
  int i;
  for (i = 0; i < frame->rows; i++)
    free(frame->lines[i]);
  free(frame->lines);
  free(frame->lens);
  free(frame->cols);
  memset(frame, 0, sizeof(*frame));
}

static void sl_render_store_clear(sl_impl_t *impl) {
  int i;
  if (!impl)
    return;
  for (i = 0; i < impl->rendered_rows; i++)
    free(impl->rendered_lines[i]);
  free(impl->rendered_lines);
  free(impl->rendered_lens);
  free(impl->rendered_cols);
  sl_reflow_frame_clear(&impl->reflow_frame);
  impl->rendered_lines = NULL;
  impl->rendered_lens = NULL;
  impl->rendered_cols = NULL;
  impl->rendered_cap = 0;
  impl->rendered_rows = 0;
  impl->rendered_editor_first = 0;
  impl->rendered_cursor_row = 0;
  impl->rendered_cursor_col = 0;
  impl->rendered_cursor_valid = 0;
  impl->rendered_width = 0;
  impl->rendered_height = 0;
}

static int sl_render_store_reserve(sl_impl_t *impl, int rows) {
  char **lines;
  size_t *lens;
  int *cols;
  int i;
  if (rows <= impl->rendered_cap)
    return 0;
  lines = (char **)realloc(impl->rendered_lines,
                           (size_t)rows * sizeof(*impl->rendered_lines));
  if (!lines)
    return -1;
  impl->rendered_lines = lines;
  lens = (size_t *)realloc(impl->rendered_lens,
                           (size_t)rows * sizeof(*impl->rendered_lens));
  if (!lens)
    return -1;
  impl->rendered_lens = lens;
  cols = (int *)realloc(impl->rendered_cols,
                        (size_t)rows * sizeof(*impl->rendered_cols));
  if (!cols)
    return -1;
  impl->rendered_cols = cols;
  for (i = impl->rendered_cap; i < rows; i++) {
    impl->rendered_lines[i] = NULL;
    impl->rendered_lens[i] = 0;
    impl->rendered_cols[i] = 0;
  }
  impl->rendered_cap = rows;
  return 0;
}

static int sl_render_store_update(sl_impl_t *impl, sl_render_t *render,
                                  int first_row, int row_count, int cursor_row,
                                  int cursor_col, int top_row) {
  int i;
  if (sl_render_store_reserve(impl, row_count) != 0)
    return -1;
  for (i = 0; i < row_count; i++) {
    char *copy;
    sl_row_t *row;
    row = &render->rows[first_row + i];
    copy = (char *)malloc(row->len + 1);
    if (!copy)
      return -1;
    if (row->len > 0)
      memcpy(copy, row->text, row->len);
    copy[row->len] = '\0';
    free(impl->rendered_lines[i]);
    impl->rendered_lines[i] = copy;
    impl->rendered_lens[i] = row->len;
    impl->rendered_cols[i] = row->cols;
  }
  for (i = row_count; i < impl->rendered_rows; i++) {
    free(impl->rendered_lines[i]);
    impl->rendered_lines[i] = NULL;
    impl->rendered_lens[i] = 0;
    impl->rendered_cols[i] = 0;
  }
  impl->rendered_rows = row_count;
  impl->rendered_editor_first = render->editor_first - first_row;
  if (impl->rendered_editor_first < 0)
    impl->rendered_editor_first = 0;
  if (impl->rendered_editor_first > row_count)
    impl->rendered_editor_first = row_count;
  impl->rendered_top_row = top_row;
  impl->rendered_cursor_row = cursor_row;
  impl->rendered_cursor_col = cursor_col;
  impl->rendered_cursor_valid = 1;
  sl_reflow_frame_clear(&impl->reflow_frame);
  return 0;
}

/* Map the retained prompt and its cursor into the terminal's new width. */
static size_t sl_ansi_sequence_len(const char *text, size_t len, size_t pos);

/* Count physical rows from the rendered clusters: a wide cluster moves as a
 * whole when it meets the new right margin. */
static int sl_render_reflow_row(const char *text, size_t len, int width,
                                int cursor_col, int *cursor_row,
                                int *cursor_column) {
  size_t pos = 0;
  int rows = 1, col = 0, cells_seen = 0;
  if (cursor_row)
    *cursor_row = 0;
  if (cursor_column)
    *cursor_column = 0;
  while (pos < len) {
    size_t step = sl_ansi_sequence_len(text, len, pos);
    int cells;
    if (step > 0) {
      pos += step;
      continue;
    }
    step = sl_utf8_cluster_len_width(text, len, pos, &cells);
    if (step == 0)
      break;
    if (cells > 0 && col + cells > width) {
      rows++;
      col = 0;
    }
    col += cells;
    cells_seen += cells;
    if (cursor_row && cells_seen == cursor_col) {
      *cursor_row = rows - 1 + (col == width);
      if (cursor_column)
        *cursor_column = col == width ? 0 : col;
    }
    pos += step;
  }
  return rows;
}

static void sl_render_reflow_geometry(sl_impl_t *impl, int width, int *span,
                                      int *offset, int *column,
                                      int *editor_offset) {
  const sl_reflow_frame_t *source = &impl->reflow_frame;
  int source_rows = source->rows ? source->rows : impl->rendered_rows;
  int source_cursor_row =
      source->rows ? source->cursor_row : impl->rendered_cursor_row;
  int source_cursor_col =
      source->rows ? source->cursor_col : impl->rendered_cursor_col;
  int source_editor_first =
      source->rows ? source->editor_first : impl->rendered_editor_first;
  int i;
  *span = *offset = 0;
  *column = *editor_offset = 0;
  for (i = 0; i < source_rows; i++) {
    const char *line =
        source->rows ? source->lines[i] : impl->rendered_lines[i];
    size_t len = source->rows ? source->lens[i] : impl->rendered_lens[i];
    int cols = source->rows ? source->cols[i] : impl->rendered_cols[i];
    int cursor_row = 0;
    int rows =
        cols == INT_MAX
            ? 1
            : sl_render_reflow_row(line, len, width, source_cursor_col,
                                   i == source_cursor_row ? &cursor_row : NULL,
                                   i == source_cursor_row ? column : NULL);
    *span += rows;
    if (i < source_cursor_row)
      *offset += rows;
    if (i == source_cursor_row)
      *offset += cursor_row;
    if (i < source_editor_first)
      *editor_offset += rows;
  }
}

/* Retain unchanged physical prompt rows after native width reflow. Only
 * rows that crossed the new right margin become unknown; fitted rows keep
 * their cached cells so MVU can leave them untouched. */
static int sl_render_store_reflow(sl_impl_t *impl, int width, int height,
                                  int top, int span, int cursor_offset,
                                  int cursor_column, int editor_offset) {
  char **lines;
  size_t *lens;
  int *cols;
  int start = top < 0 ? -top : 0;
  int rows = span - start;
  int offset = 0, i;
  sl_reflow_frame_t *source = &impl->reflow_frame;
  if (rows > height - (top > 0 ? top : 0))
    rows = height - (top > 0 ? top : 0);
  if (rows < 0)
    rows = 0;
  lines = (char **)calloc((size_t)(rows > 0 ? rows : 1), sizeof(*lines));
  lens = (size_t *)calloc((size_t)(rows > 0 ? rows : 1), sizeof(*lens));
  cols = (int *)malloc((size_t)(rows > 0 ? rows : 1) * sizeof(*cols));
  if (!lines || !lens || !cols) {
    free(lines);
    free(lens);
    free(cols);
    return -1;
  }
  for (i = 0; i < rows; i++)
    cols[i] = INT_MAX; /* Unknown cells inside an owned prompt row. */
  if (source->rows == 0) {
    source->lines = impl->rendered_lines;
    source->lens = impl->rendered_lens;
    source->cols = impl->rendered_cols;
    source->rows = impl->rendered_rows;
    source->editor_first = impl->rendered_editor_first;
    source->cursor_row = impl->rendered_cursor_row;
    source->cursor_col = impl->rendered_cursor_col;
  } else {
    for (i = 0; i < impl->rendered_rows; i++)
      free(impl->rendered_lines[i]);
    free(impl->rendered_lines);
    free(impl->rendered_lens);
    free(impl->rendered_cols);
  }
  impl->rendered_lines = lines;
  impl->rendered_lens = lens;
  impl->rendered_cols = cols;
  impl->rendered_cap = impl->rendered_rows = rows;
  for (i = 0; i < source->rows; i++) {
    int count = source->cols[i] == INT_MAX
                    ? 1
                    : sl_render_reflow_row(source->lines[i], source->lens[i],
                                           width, 0, NULL, NULL);
    int target = offset - start;
    if (count < 1)
      count = 1;
    if (count == 1 && target >= 0 && target < rows && source->lines[i]) {
      lines[target] = (char *)malloc(source->lens[i] + 1);
      if (!lines[target])
        return -1;
      memcpy(lines[target], source->lines[i], source->lens[i] + 1);
      lens[target] = source->lens[i];
      cols[target] = source->cols[i];
    }
    offset += count;
  }
  impl->rendered_top_row = top > 0 ? top : 0;
  impl->rendered_editor_first =
      editor_offset > start ? editor_offset - start : 0;
  impl->rendered_cursor_row = cursor_offset - start;
  impl->rendered_cursor_col = cursor_column;
  return 0;
}

static int sl_row_equal(sl_impl_t *impl, sl_render_t *render, int row,
                        int render_row) {
  if (impl->render_reset)
    return 0;
  if (row >= impl->rendered_rows || render_row >= render->count)
    return 0;
  if (impl->rendered_cols[row] != render->rows[render_row].cols ||
      impl->rendered_lens[row] != render->rows[render_row].len)
    return 0;
  if (impl->rendered_lens[row] == 0)
    return 1;
  return memcmp(impl->rendered_lines[row], render->rows[render_row].text,
                impl->rendered_lens[row]) == 0;
}

static size_t sl_ansi_sequence_len(const char *text, size_t len, size_t pos) {
  size_t i;
  if (!text || pos + 2 > len || text[pos] != '\033' || text[pos + 1] != '[')
    return 0;
  i = pos + 2;
  while (i < len && (text[i] < '@' || text[i] > '~'))
    i++;
  return i < len ? i - pos + 1 : 0;
}

static int sl_row_prefix_cols(const char *text, size_t len) {
  int cols;
  size_t pos;
  cols = 0;
  pos = 0;
  while (pos < len) {
    int cells;
    size_t ansi_len;
    size_t char_len;
    ansi_len = sl_ansi_sequence_len(text, len, pos);
    if (ansi_len > 0) {
      pos += ansi_len;
      continue;
    }
    char_len = sl_utf8_cluster_len_width(text, len, pos, &cells);
    if (char_len == 0)
      break;
    cols += cells;
    pos += char_len;
  }
  return cols;
}

/* Return a cluster-safe byte prefix shared by two retained view rows. */
static size_t sl_row_shared_prefix(const char *old_text, size_t old_len,
                                   const char *new_text, size_t new_len) {
  size_t pos;
  size_t limit;
  pos = 0;
  limit = old_len < new_len ? old_len : new_len;
  while (pos < limit && old_text[pos] == new_text[pos]) {
    size_t ansi_len;
    int cells;
    size_t char_len;
    /* Parse the complete new sequence even when the old row is shorter.
     * Otherwise a longer colour escape can look like shared text bytes. */
    ansi_len = sl_ansi_sequence_len(new_text, new_len, pos);
    if (ansi_len > 0) {
      if (ansi_len > limit - pos ||
          memcmp(old_text + pos, new_text + pos, ansi_len) != 0)
        break;
      pos += ansi_len;
      continue;
    }
    char_len = sl_utf8_cluster_len_width(new_text, limit, pos, &cells);
    if (char_len == 0 || pos + char_len > limit ||
        memcmp(old_text + pos, new_text + pos, char_len) != 0)
      break;
    pos += char_len;
  }
  return pos;
}

static int sl_render_visible_equal(sl_impl_t *impl, sl_render_t *render,
                                   int first_row, int row_count, int cursor_row,
                                   int cursor_col, int top_row) {
  int i;
  int editor_first = render->editor_first - first_row;
  if (editor_first < 0)
    editor_first = 0;
  if (editor_first > row_count)
    editor_first = row_count;
  if (impl->rendered_editor_first != editor_first)
    return 0;
  if (impl->rendered_rows != row_count)
    return 0;
  if (impl->rendered_top_row != top_row)
    return 0;
  if (impl->rendered_cursor_row != cursor_row ||
      impl->rendered_cursor_col != cursor_col)
    return 0;
  for (i = 0; i < row_count; i++) {
    if (!sl_row_equal(impl, render, i, first_row + i))
      return 0;
  }
  return 1;
}

static int sl_word_byte(char ch) {
  return ch != '\0' && ch != '\n' && ch != '\t' && !isspace((unsigned char)ch);
}

static size_t sl_next_word_len(const char *buf, size_t len, size_t pos) {
  size_t n;
  n = 0;
  while (pos + n < len && sl_word_byte(buf[pos + n]))
    n++;
  return n;
}

static int sl_text_width(const char *buf, size_t len) {
  size_t pos;
  int width;
  pos = 0;
  width = 0;
  while (pos < len) {
    int cells;
    size_t n;
    if (buf[pos] == '\t') {
      cells = 8 - (width % 8);
      pos++;
    } else {
      n = sl_utf8_cluster_len_width(buf, len, pos, &cells);
      if (n == 0)
        break;
      pos += n;
    }
    width += cells;
  }
  return width;
}

static int sl_word_width(const char *buf, size_t len, size_t pos,
                         size_t byte_len) {
  int width;
  size_t end;
  width = 0;
  end = pos + byte_len;
  while (pos < end && pos < len) {
    int cells;
    size_t n;
    n = sl_utf8_cluster_len_width(buf, len, pos, &cells);
    if (n == 0)
      break;
    width += cells;
    pos += n;
  }
  return width;
}

static int sl_queue_row_append_span(sl_row_t *row, const char *text, size_t len,
                                    int available, const char *style,
                                    size_t *consumed) {
  size_t pos;
  int cols;
  int screen_cols;
  int clipped;
  size_t break_pos = 0;
  size_t break_len = 0;
  int break_cols = 0;
  const char *reset;
  if (!row || !text || available < 1)
    return 0;
  reset = style && style[0] != '\0' ? "\033[0m" : "";
  if (sl_row_append(row, style ? style : "", style ? strlen(style) : 0) != 0)
    return -1;
  pos = 0;
  cols = 0;
  screen_cols = row->cols;
  clipped = 0;
  while (pos < len && text[pos] != '\n') {
    unsigned char ch;
    unsigned long codepoint;
    int cells;
    int escaped_control;
    size_t n;
    char visible[9];
    ch = (unsigned char)text[pos];
    /* Wrapped rows drop the separating whitespace, never part of a word.
     * Keep the rendered boundary too: tabs and escaped controls use cells
     * differently from their source bytes. Single-line previews stay clipped.
     */
    if (consumed && cols > 0 && (ch == ' ' || ch == '\t') &&
        (pos == 0 || (text[pos - 1] != ' ' && text[pos - 1] != '\t'))) {
      break_pos = pos;
      while (break_pos < len &&
             (text[break_pos] == ' ' || text[break_pos] == '\t'))
        break_pos++;
      break_len = row->len;
      break_cols = row->cols;
    }
    n = 1;
    escaped_control = 0;
    if (ch == '\t') {
      cells = 8 - (screen_cols % 8);
      memset(visible, ' ', (size_t)cells);
      visible[cells] = '\0';
    } else if (ch < 0x20 || ch == 0x7f) {
      cells = 2;
      visible[0] = '^';
      visible[1] = ch == 0x7f ? '?' : (char)(ch + '@');
      visible[2] = '\0';
    } else if (ch >= 0x80 && ch <= 0x9f) {
      cells = 4;
      (void)snprintf(visible, sizeof(visible), "\\x%02X", ch);
      escaped_control = 1;
    } else {
      n = sl_utf8_decode(text, len, pos, &codepoint);
      if (n == 0) {
        n = 1;
        cells = 1;
      } else if (codepoint >= 0x80 && codepoint <= 0x9f) {
        cells = 4;
        (void)snprintf(visible, sizeof(visible), "\\x%02lX", codepoint);
        escaped_control = 1;
      } else {
        n = sl_utf8_cluster_len_width(text, len, pos, &cells);
        if (n == 0) {
          n = 1;
          cells = 1;
        }
      }
    }
    if (cells > 0 && cols + cells > available) {
      if (break_pos > 0) {
        row->len = break_len;
        row->text[row->len] = '\0';
        row->cols = break_cols;
        pos = break_pos;
      } else if (consumed && cols == 0) {
        if (sl_row_append_cells(row, "?", 1, 1) != 0)
          return -1;
        pos += n;
      }
      clipped = 1;
      break;
    }
    if (ch == '\t' || ch < 0x20 || ch == 0x7f || escaped_control) {
      if (sl_row_append_cells(row, visible, strlen(visible), cells) != 0)
        return -1;
    } else if (sl_row_append_cells(row, text + pos, n, cells) != 0) {
      return -1;
    }
    cols += cells;
    screen_cols += cells;
    pos += n;
  }
  if (pos < len)
    clipped = 1;
  if (consumed)
    *consumed = pos;
  if (!consumed && clipped && available - cols >= 3 &&
      sl_row_append_cells(row, "...", 3, 3) != 0)
    return -1;
  return sl_row_append(row, reset, strlen(reset));
}

static int sl_queue_row_append_preview(sl_row_t *row, const char *text,
                                       int available, const char *style) {
  return sl_queue_row_append_span(row, text, strlen(text), available, style,
                                  NULL);
}

static int sl_render_queue_text(sl_render_t *render, const char *text,
                                int width, const char *prefix,
                                const char *style, int max_rows) {
  size_t remaining = strlen(text);
  int indent = width > 2 ? 2 : 0;
  int rows = 0;
  do {
    size_t consumed = 0;
    const char *next;
    if (sl_render_new_row_at(render, 0, 0) != 0 ||
        sl_row_append(&render->rows[render->count - 1], style, strlen(style)) !=
            0 ||
        sl_row_append_cells(&render->rows[render->count - 1], prefix,
                            (size_t)indent, indent) != 0 ||
        sl_queue_row_append_span(&render->rows[render->count - 1], text,
                                 remaining, width - indent, style,
                                 &consumed) != 0)
      return -1;
    next = text + consumed;
    if (*next == '\n')
      ++next;
    if (++rows == max_rows && *next) {
      sl_row_t *row = &render->rows[render->count - 1];
      int dots = width - indent < 3 ? width - indent : 3;
      free(row->text);
      memset(row, 0, sizeof(*row));
      if (sl_row_append(row, style, strlen(style)) != 0 ||
          sl_row_append_cells(row, prefix, (size_t)indent, indent) != 0 ||
          sl_queue_row_append_span(row, text, remaining, width - indent - dots,
                                   style, &consumed) != 0 ||
          sl_queue_row_append_span(row, "...", (size_t)dots, dots, style,
                                   NULL) != 0)
        return -1;
      return 0;
    }
    prefix = "  ";
    text += consumed;
    remaining -= consumed;
    if (*text == '\n') {
      ++text;
      --remaining;
    }
  } while (*text);
  return 0;
}

static const char *sl_prompt_theme_style(sl_prompt_theme_t theme) {
  if (theme == SL_PROMPT_THEME_DEFAULT)
    return "\033[1;97m";
  if (theme == SL_PROMPT_THEME_ACCENT)
    return "\033[1;36m";
  if (theme == SL_PROMPT_THEME_DRACULA)
    return "\033[38;2;98;114;164m";
  if (theme == SL_PROMPT_THEME_GRUVBOX)
    return "\033[1;38;2;184;187;38m";
  if (theme == SL_PROMPT_THEME_MONOCHROME)
    return "\033[1;38;2;168;118;40m";
  if (theme == SL_PROMPT_THEME_MONOGREEN)
    return "\033[1;38;2;22;122;31m";
  if (theme == SL_PROMPT_THEME_OUTRUN)
    return "\033[38;2;78;69;99m";
  if (theme == SL_PROMPT_THEME_RICED)
    return "\033[1;38;2;255;255;255m";
  if (theme == SL_PROMPT_THEME_SYNTHWAVE)
    return "\033[1;38;2;255;126;219m";
  return "";
}

static int sl_prompt_input_style(const sl_impl_t *impl, char *style,
                                 size_t style_cap) {
  const sl_theme_palette_t *palette;
  if (!impl || !style || style_cap == 0)
    return -1;
  palette = sl_theme_palette(impl->prompt_theme);
  if (!palette || (palette->input.red == 0 && palette->input.green == 0 &&
                   palette->input.blue == 0)) {
    style[0] = '\0';
    return 0;
  }
  return sl_rgb_style(style, style_cap, palette->input, palette->input_bold);
}

static int sl_render_new_input_row(sl_render_t *render, int indent,
                                   size_t start) {
  return sl_render_new_row_at(render, start, indent);
}

static int sl_row_append_styled(sl_row_t *row, const char *text,
                                const char *style) {
  const char *reset;
  if (!row || !text)
    return -1;
  reset = style && style[0] != '\0' ? "\033[0m" : "";
  return sl_row_append(row, style ? style : "", style ? strlen(style) : 0) ||
                 sl_row_append_cells(row, text, strlen(text),
                                     sl_text_width(text, strlen(text))) ||
                 sl_row_append(row, reset, strlen(reset))
             ? -1
             : 0;
}

static int sl_statusline_spinner_advance(sl_statusline_t *statusline) {
  struct timeval now;
  long elapsed_ms;
  if (!statusline || !statusline->spinner || !statusline->busy)
    return 0;
  if (gettimeofday(&now, NULL) != 0)
    return 0;
  if (!statusline->spinner_time_valid) {
    statusline->spinner_time = now;
    statusline->spinner_time_valid = 1;
    return 1;
  }
  elapsed_ms = (long)(now.tv_sec - statusline->spinner_time.tv_sec) * 1000L +
               (long)(now.tv_usec - statusline->spinner_time.tv_usec) / 1000L;
  if (elapsed_ms < 500L)
    return 0;
  statusline->spinner_frame = (statusline->spinner_frame + 1) % 4;
  statusline->spinner_time = now;
  return 1;
}

static int sl_statusline_append_wrapped(sl_render_t *render, int width,
                                        int *col, const char *text,
                                        const char *style, int indent) {
  size_t pos;
  size_t len;
  const char *reset;
  if (!render || !col || !text || width < 1)
    return -1;
  len = strlen(text);
  if (len == 0)
    return 0;
  reset = style && style[0] != '\0' ? "\033[0m" : "";
  if (sl_row_append(&render->rows[render->count - 1], style ? style : "",
                    style ? strlen(style) : 0) != 0)
    return -1;
  pos = 0;
  while (pos < len) {
    int cells;
    size_t n;
    n = sl_utf8_cluster_len_width(text, len, pos, &cells);
    if (n == 0) {
      n = 1;
      cells = 1;
    }
    if (cells > width - *col && *col > 0) {
      if (sl_row_append(&render->rows[render->count - 1], reset,
                        strlen(reset)) != 0 ||
          sl_render_new_row_at(render, 0, 0) != 0)
        return -1;
      *col = 0;
      if (indent > 0 && cells <= width - indent) {
        if (sl_row_append_cells(&render->rows[render->count - 1], "  ",
                                (size_t)indent, indent) != 0)
          return -1;
        *col = indent;
      }
      if (sl_row_append(&render->rows[render->count - 1], style ? style : "",
                        style ? strlen(style) : 0) != 0)
        return -1;
    }
    if (cells > width - *col) {
      pos += n;
      continue;
    }
    if (sl_row_append_cells(&render->rows[render->count - 1], text + pos, n,
                            cells) != 0)
      return -1;
    *col += cells;
    pos += n;
  }
  return sl_row_append(&render->rows[render->count - 1], reset, strlen(reset));
}

static int sl_render_append_statusline(sl_t *self, sl_render_t *render,
                                       int width) {
  static const char spinner_frames[] = "/-\\|";
  static const sl_rgb_t busy_colour = {255, 51, 51};
  static const sl_rgb_t idle_colour = {57, 255, 20};
  sl_impl_t *impl;
  sl_statusline_t *statusline;
  const sl_theme_palette_t *palette;
  const char *default_style;
  char marker[3];
  char style[32];
  int col;
  int marker_width;
  int have_element;
  size_t i;
  impl = sl_impl(self);
  if (!impl || !render || !impl->statusline.enabled)
    return 0;
  statusline = &impl->statusline;
  palette = sl_theme_palette(impl->prompt_theme);
  if (sl_render_new_row_at(render, 0, 0) != 0)
    return -1;
  if (statusline->spinner && statusline->busy) {
    (void)sl_statusline_spinner_advance(statusline);
    marker[0] = spinner_frames[statusline->spinner_frame];
    marker[1] = ' ';
    marker[2] = '\0';
    marker_width = 2;
  } else if (statusline->busy) {
    marker[0] = 'x';
    marker[1] = ' ';
    marker[2] = '\0';
    marker_width = 2;
  } else {
    marker[0] = statusline->idle_marker ? statusline->idle_marker : ' ';
    marker[1] = ' ';
    marker[2] = '\0';
    marker_width = 2;
  }
  if (marker_width > width)
    marker_width = width;
  marker[marker_width] = '\0';
  default_style = NULL;
  if (impl->prompt_theme == SL_PROMPT_THEME_DEFAULT &&
      (statusline->busy || statusline->idle_marker != '\0')) {
    default_style = statusline->busy ? "\033[31m" : "\033[32m";
  }
  if (default_style) {
    if (sl_row_append_styled(&render->rows[render->count - 1], marker,
                             default_style) != 0)
      return -1;
  } else if ((statusline->busy || statusline->idle_marker != '\0') && palette &&
             impl->prompt_theme != SL_PROMPT_THEME_PLAIN) {
    sl_rgb_t colour;
    colour = statusline->busy ? busy_colour : idle_colour;
    if (sl_rgb_style(style, sizeof(style), colour, 0) != 0 ||
        sl_row_append_styled(&render->rows[render->count - 1], marker, style) !=
            0)
      return -1;
  } else if (marker_width > 0 &&
             sl_row_append_cells(&render->rows[render->count - 1], marker,
                                 (size_t)marker_width, marker_width) != 0) {
    return -1;
  }
  col = marker_width;
  have_element = 0;
  for (i = 0; i < statusline->count; i++) {
    const char *element;
    int element_width;
    int indent;
    element = statusline->elements[i];
    if (!element || element[0] == '\0')
      continue;
    element_width = sl_text_width(element, strlen(element));
    indent = marker_width > 0 && width > 2 ? 2 : 0;
    if ((have_element && element_width + 3 > width - col) ||
        (!have_element && element_width > width - col)) {
      if (sl_render_new_row_at(render, 0, 0) != 0 ||
          (indent > 0 &&
           sl_row_append_cells(&render->rows[render->count - 1], "  ",
                               (size_t)indent, indent) != 0))
        return -1;
      col = indent;
      have_element = 0;
    }
    if (have_element) {
      if (impl->prompt_theme == SL_PROMPT_THEME_DEFAULT) {
        if (sl_row_append_styled(&render->rows[render->count - 1], " : ",
                                 "\033[90m") != 0)
          return -1;
      } else if (palette && impl->prompt_theme != SL_PROMPT_THEME_PLAIN) {
        if (sl_rgb_style(style, sizeof(style), palette->separator, 0) != 0 ||
            sl_row_append_styled(&render->rows[render->count - 1], " : ",
                                 style) != 0)
          return -1;
      } else if (sl_row_append_cells(&render->rows[render->count - 1], " : ", 3,
                                     3) != 0) {
        return -1;
      }
      col += 3;
    }
    if (impl->prompt_theme != SL_PROMPT_THEME_PLAIN) {
      sl_theme_color_t color =
          (sl_theme_color_t)(SL_THEME_COLOR_ELEMENT_0 +
                             (statusline->start_element + i) % 8);
      if (sl_theme_color_style(impl->prompt_theme, color, 0, style,
                               sizeof(style)) != 0 ||
          sl_statusline_append_wrapped(render, width, &col, element, style,
                                       indent) != 0)
        return -1;
    } else if (sl_statusline_append_wrapped(render, width, &col, element, "",
                                            indent) != 0) {
      return -1;
    }
    have_element = 1;
  }
  return 0;
}

static int sl_theme_color_style(sl_prompt_theme_t theme, sl_theme_color_t color,
                                int italic, char *style, size_t size) {
  static const int ansi_elements[] = {36, 33, 35, 34, 32, 31, 37, 97};
  const sl_theme_palette_t *palette;
  const sl_rgb_t *rgb;
  int ansi;
  int dim;
  int n;
  palette = sl_theme_palette(theme);
  if (color == SL_THEME_COLOR_QUEUE)
    color = SL_THEME_COLOR_ELEMENT_3;
  if (theme == SL_PROMPT_THEME_DEFAULT || theme == SL_PROMPT_THEME_PLAIN) {
    dim = color == SL_THEME_COLOR_MUTED;
    if (color == SL_THEME_COLOR_MUTED || color == SL_THEME_COLOR_SECONDARY)
      ansi = 90;
    else if (color == SL_THEME_COLOR_PROMPT || color == SL_THEME_COLOR_INPUT)
      ansi = 97;
    else
      ansi = ansi_elements[color - SL_THEME_COLOR_ELEMENT_0];
    n = snprintf(style, size, "\033[%s%s%dm", italic ? "3;" : "",
                 dim ? "2;" : "", ansi);
  } else {
    if (color == SL_THEME_COLOR_MUTED)
      rgb = &palette->separator;
    else if (color == SL_THEME_COLOR_SECONDARY)
      rgb = &palette->secondary;
    else if (color == SL_THEME_COLOR_PROMPT)
      rgb = &palette->prompt;
    else if (color == SL_THEME_COLOR_INPUT)
      rgb = &palette->input;
    else
      rgb = &palette->elements[color - SL_THEME_COLOR_ELEMENT_0];
    n = snprintf(style, size, "\033[%s38;2;%u;%u;%um", italic ? "3;" : "",
                 (unsigned int)rgb->red, (unsigned int)rgb->green,
                 (unsigned int)rgb->blue);
  }
  return n > 0 && (size_t)n < size ? 0 : -1;
}

static int sl_status_message_append_part(sl_row_t *row, const char *text,
                                         int width, const char *style) {
  size_t pos;
  size_t len;
  if (sl_row_append(row, style, strlen(style)) != 0)
    return -1;
  len = strlen(text);
  pos = 0;
  while (pos < len) {
    int cells;
    size_t n;
    n = sl_utf8_cluster_len_width(text, len, pos, &cells);
    if (n == 0 || (cells > 0 && row->cols + cells > width))
      break;
    if (sl_row_append_cells(row, text + pos, n, cells) != 0)
      return -1;
    pos += n;
  }
  return sl_row_append(row, "\033[0m", 4);
}

static int sl_status_message_new_row(sl_render_t *render, int indent,
                                     const char *style) {
  if (sl_row_append(&render->rows[render->count - 1], "\033[0m", 4) != 0 ||
      sl_render_new_indented_row(render, indent, 0) != 0)
    return -1;
  return sl_row_append(&render->rows[render->count - 1], style, strlen(style));
}

static int sl_status_message_append_wrapped(sl_render_t *render,
                                            const char *text, int width,
                                            int indent, const char *style) {
  size_t pos;
  size_t len;
  size_t word_end;
  sl_row_t *row;
  len = strlen(text);
  pos = 0;
  word_end = 0;
  row = &render->rows[render->count - 1];
  if (row->cols >= width && len > 0) {
    if (sl_render_new_indented_row(render, indent, 0) != 0)
      return -1;
    row = &render->rows[render->count - 1];
  }
  if (sl_row_append(row, style, strlen(style)) != 0)
    return -1;
  while (pos < len) {
    size_t n;
    int cells;
    row = &render->rows[render->count - 1];
    if (text[pos] == ' ') {
      size_t spaces;
      size_t next_word;
      int next_width;
      spaces = 0;
      while (pos + spaces < len && text[pos + spaces] == ' ')
        spaces++;
      next_word = sl_next_word_len(text, len, pos + spaces);
      next_width = sl_word_width(text, len, pos + spaces, next_word);
      if ((next_word > 0 && next_width <= width - indent &&
           spaces + (size_t)next_width > (size_t)(width - row->cols)) ||
          spaces > (size_t)(width - row->cols)) {
        pos += spaces;
        if (pos < len && sl_status_message_new_row(render, indent, style) != 0)
          return -1;
        continue;
      }
      if (sl_row_append_cells(row, text + pos, spaces, (int)spaces) != 0)
        return -1;
      pos += spaces;
      continue;
    }
    if (sl_word_byte(text[pos]) && pos >= word_end) {
      size_t word_len;
      int word_width;
      word_len = sl_next_word_len(text, len, pos);
      word_end = pos + word_len;
      word_width = sl_word_width(text, len, pos, word_len);
      if (row->cols > indent && word_width <= width - indent &&
          row->cols + word_width > width) {
        if (sl_status_message_new_row(render, indent, style) != 0)
          return -1;
        row = &render->rows[render->count - 1];
      }
    }
    n = sl_utf8_cluster_len_width(text, len, pos, &cells);
    if (n == 0)
      return -1;
    if (cells > width) {
      if (row->cols >= width) {
        if (sl_status_message_new_row(render, indent, style) != 0)
          return -1;
        row = &render->rows[render->count - 1];
      }
      if (sl_row_append_cells(row, "?", 1, 1) != 0)
        return -1;
      pos += n;
      continue;
    }
    if (row->cols + cells > width) {
      if (sl_status_message_new_row(render, indent, style) != 0)
        return -1;
      row = &render->rows[render->count - 1];
    }
    if (cells > width - row->cols)
      return -1;
    if (sl_row_append_cells(row, text + pos, n, cells) != 0)
      return -1;
    pos += n;
  }
  return sl_row_append(&render->rows[render->count - 1], "\033[0m", 4);
}

static int sl_render_append_status_message(sl_t *self, sl_render_t *render,
                                           int width) {
  sl_impl_t *impl;
  sl_row_t *row;
  char prefix_style[48];
  char text_style[48];
  const char *prefix;
  int indent;
  impl = sl_impl(self);
  if (!impl || !impl->status_message || !impl->status_message[0])
    return 0;
  if (sl_render_new_row_at(render, 0, 0) != 0)
    return -1;
  if (sl_theme_color_style(impl->prompt_theme,
                           impl->status_message_prefix_color, 0, prefix_style,
                           sizeof(prefix_style)) != 0 ||
      sl_theme_color_style(impl->prompt_theme, impl->status_message_text_color,
                           1, text_style, sizeof(text_style)) != 0)
    return -1;
  row = &render->rows[render->count - 1];
  prefix = impl->status_message_prefix ? impl->status_message_prefix : "! ";
  if (sl_status_message_append_part(row, prefix, width, prefix_style) != 0)
    return -1;
  indent = sl_text_width(prefix, strlen(prefix));
  if (indent > width - 2)
    indent = width > 1 ? width - 2 : 0;
  return sl_status_message_append_wrapped(render, impl->status_message, width,
                                          indent, text_style);
}

static int sl_render_append_queue_panel(sl_t *self, sl_render_t *render,
                                        int width) {
  sl_impl_t *impl;
  sl_prompt_queue_t *queue;
  const char *style;
  char queue_style[32];
  int i;
  int shown;
  int remaining;
  int held;
  int approval;
  int focused;
  char summary[64];
  char backend_notice[192];
  impl = sl_impl(self);
  if (!sl_prompt_queue_enabled(impl) || !render)
    return 0;
  queue = &impl->prompt_queue;
  style = "";
  if (impl->prompt_theme != SL_PROMPT_THEME_PLAIN) {
    if (sl_theme_color_style(impl->prompt_theme, SL_THEME_COLOR_QUEUE, 0,
                             queue_style, sizeof(queue_style)) != 0)
      return -1;
    style = queue_style;
  }
  if (sl_backend_notice(impl, backend_notice, sizeof(backend_notice))) {
    if (sl_render_new_row_at(render, 0, 0) != 0 ||
        sl_queue_row_append_preview(&render->rows[render->count - 1],
                                    backend_notice, width, style) != 0)
      return -1;
  }
  if (queue->len == 0 && !queue->editing.info.id)
    return 0;
  held = 0;
  for (i = 0; i < queue->len; i++)
    held += queue->items[i].info.held != 0;
  (void)snprintf(summary, sizeof(summary), "%d %s", held,
                 held == 1 ? "approval" : "awaiting approval");
  approval =
      queue->approval_id ? sl_prompt_held_index(impl, queue->approval_id) : -1;
  focused = queue->approval_focused &&
            sl_prompt_approval_index(impl, queue->approval_id) >= 0;
  if (queue->editing.info.id) {
    const char *editing =
        queue->editing.info.held
            ? "H editing: approval required / Alt-N discard"
        : queue->editing.info.mode == SL_PROMPT_QUEUE_MODE_STEER
            ? "S editing / Alt-N discard"
            : "Q editing / Alt-N discard";
    if (sl_render_new_row_at(render, 0, 0) != 0 ||
        sl_queue_row_append_preview(&render->rows[render->count - 1], editing,
                                    width, style) != 0)
      return -1;
  }
  if (held && approval < 0) {
    if (sl_render_queue_text(render, summary, width, "  ", style, 0) != 0)
      return -1;
  }
  shown = 0;
  remaining = 0;
  for (i = 0; i < queue->len; i++) {
    char prefix[32];
    char number[24];
    const char *entry_prefix;
    int prefix_width;
    sl_row_t *row;
    if (queue->items[i].info.held)
      continue;
    if (shown >= queue->preview_entries) {
      remaining++;
      continue;
    }
    shown++;
    if (sl_render_new_row_at(render, 0, 0) != 0)
      return -1;
    row = &render->rows[render->count - 1];
    (void)snprintf(number, sizeof(number), "%d. ", i + 1);
    entry_prefix =
        queue->items[i].info.mode == SL_PROMPT_QUEUE_MODE_STEER ? "S " : "Q ";
    (void)snprintf(prefix, sizeof(prefix), "%s%s", entry_prefix, number);
    if (sl_queue_row_append_preview(row, prefix, width, style) != 0)
      return -1;
    prefix_width = row->cols;
    if (sl_queue_row_append_preview(row, queue->items[i].text,
                                    width - prefix_width, style) != 0)
      return -1;
  }
  if (remaining) {
    char more[64];
    (void)snprintf(more, sizeof(more), "  ... %d more", remaining);
    if (sl_render_new_row_at(render, 0, 0) != 0 ||
        sl_queue_row_append_preview(&render->rows[render->count - 1], more,
                                    width, style) != 0)
      return -1;
  }
  if (approval >= 0) {
    char heading[192];
    (void)snprintf(
        heading, sizeof(heading), "%s / %s / Alt-E edit / %s:", summary,
        focused ? "Alt-Y approve / Alt-N reject" : "Alt-A focus",
        queue->items[approval].info.mode == SL_PROMPT_QUEUE_MODE_STEER
            ? "Steering"
            : "Queued");
    if (sl_render_queue_text(render, heading, width, "A ",
                             focused ? "\033[1;7m" : style, 0) != 0)
      return -1;
    if (sl_render_queue_text(render, queue->items[approval].text, width, "  ",
                             style, 3) != 0)
      return -1;
  }
  return 0;
}

/* Bounds describe cursor positions owned by this visual row, rather than
 * the content endpoint that can belong to the next soft-wrapped row. A row
 * filled only by a tab continuation may have no addressable position. */
static void sl_render_cursor(sl_render_t *render, size_t offset, int col,
                             size_t cursor) {
  sl_row_t *row = &render->rows[render->count - 1];
  if (!row->has_cursor) {
    row->start = offset;
    row->base_col = col;
    row->has_cursor = 1;
  }
  row->end = offset;
  if (offset == cursor) {
    render->cursor_row = render->count - 1;
    render->cursor_col = col;
  }
}

/* Input layout records source spans and cursor positions only. Formatting
 * text is allocated later for visible rows, including partial tab cells. */
static void sl_render_input_span(sl_render_t *render, size_t at, size_t len,
                                 int cells, int tab) {
  sl_row_t *row = &render->rows[render->count - 1];
  if (row->text_first == row->text_end)
    row->text_first = at;
  if (tab) {
    if (at == row->text_first)
      row->leading_tab += cells;
    if (row->text_end != at + len)
      row->trailing_tab = 0;
    row->trailing_tab += cells;
  } else
    row->trailing_tab = 0;
  row->text_end = at + len;
  row->cols += cells;
}

static int sl_render_build(sl_t *self, const char *prompt,
                           sl_render_t *render) {
  sl_impl_t *impl;
  int width;
  int col;
  int prompt_width;
  int indent;
  size_t i;
  size_t space_end = 0;
  int space_word_width = 0;
  int space_has_word = 0;
  memset(render, 0, sizeof(*render));
  impl = sl_impl(self);
  if (!impl)
    return -1;
  if (sl_render_init(self, render) != 0)
    return -1;
  width = sl_terminal_width(impl);
  if (width < 1)
    width = 1;
  render->width = width;
  if (sl_render_append_queue_panel(self, render, width) != 0)
    return -1;
  if (sl_render_append_status_message(self, render, width) != 0)
    return -1;
  if (sl_render_append_statusline(self, render, width) != 0)
    return -1;
  /* A search query is persistent chrome, not part of the scrollable result.
   * Keeping it on one bounded row also handles a query as wide as the PTY. */
  if (impl->history_search_active && prompt) {
    if (sl_render_new_row_at(render, 0, 0) != 0 ||
        sl_queue_row_append_preview(
            &render->rows[render->count - 1], prompt, width,
            sl_prompt_theme_style(impl->prompt_theme)) != 0)
      return -1;
    prompt = "";
  }
  render->editor_first = render->count;
  prompt_width = prompt ? sl_text_width(prompt, strlen(prompt)) : 0;
  if (sl_render_new_row_at(render, 0, prompt_width) != 0)
    return -1;
  indent = prompt_width;
  if (indent >= width)
    indent = width > 1 ? width - 1 : 0;
  col = prompt_width;
  i = 0;
  while (i < impl->len) {
    char ch;
    size_t char_len;
    int char_width;
    ch = impl->buf[i];
    char_len = sl_utf8_cluster_len_width(impl->buf, impl->len, i, &char_width);
    if (char_len == 0)
      char_len = 1;
    if (ch == '\n') {
      sl_render_cursor(render, i, col, impl->cursor);
      if (sl_render_new_input_row(render, indent, i + 1) != 0)
        return -1;
      col = indent;
      i++;
      continue;
    }
    if (ch == ' ' && col > indent) {
      size_t spaces;
      int word_width;
      int avail;
      if (i >= space_end) {
        size_t word_len;
        space_end = i;
        while (space_end < impl->len && impl->buf[space_end] == ' ')
          ++space_end;
        word_len = sl_next_word_len(impl->buf, impl->len, space_end);
        space_has_word = word_len != 0;
        space_word_width =
            word_len ? sl_word_width(impl->buf, impl->len, space_end, word_len)
                     : 0;
      }
      spaces = space_end - i;
      word_width = space_word_width;
      avail = width - indent;
      if (avail < 1)
        avail = 1;
      if (space_has_word && word_width <= avail &&
          col + (int)spaces + word_width > width) {
        if (sl_render_new_input_row(render, indent, i + spaces) != 0)
          return -1;
        col = indent;
        if (impl->cursor >= i && impl->cursor <= i + spaces) {
          render->cursor_row = render->count - 1;
          render->cursor_col = col;
        }
        i += spaces;
        continue;
      }
    }
    if (sl_word_byte(ch) && (i == 0 || !sl_word_byte(impl->buf[i - 1])) &&
        col > indent) {
      size_t word_len;
      int word_width;
      int avail;
      word_len = sl_next_word_len(impl->buf, impl->len, i);
      word_width = sl_word_width(impl->buf, impl->len, i, word_len);
      avail = width - indent;
      if (avail < 1)
        avail = 1;
      if (word_width <= avail && col + word_width > width) {
        if (sl_render_new_input_row(render, indent, i) != 0)
          return -1;
        col = indent;
      }
    }
    if (col >= width || (char_width > 0 && col + char_width > width)) {
      if (sl_render_new_input_row(render, indent, i) != 0)
        return -1;
      col = indent;
    }
    sl_render_cursor(render, i, col, impl->cursor);
    if (ch == '\t') {
      int spaces;
      spaces = 8 - (col % 8);
      while (spaces-- > 0) {
        if (col >= width) {
          if (sl_render_new_input_row(render, indent, i) != 0)
            return -1;
          col = indent;
        }
        sl_render_input_span(render, i, 1, 1, 1);
        col++;
      }
    } else {
      sl_render_input_span(render, i, char_len, char_width, 0);
      col += char_width;
    }
    i += char_len;
  }
  if (col < width || impl->cursor == impl->len) {
    if (col >= width) {
      if (sl_render_new_input_row(render, indent, impl->len) != 0)
        return -1;
      col = indent;
    }
    sl_render_cursor(render, impl->len, col, impl->cursor);
  }
  return 0;
}

static size_t sl_cursor_for_render_col(sl_render_t *render, int row_index,
                                       int target_col, const char *buf) {
  sl_row_t *row;
  size_t pos;
  int col;
  if (row_index < 0 || row_index >= render->count)
    return 0;
  row = &render->rows[row_index];
  if (target_col <= row->base_col)
    return row->start;
  col = row->base_col;
  pos = row->start;
  while (pos < row->end) {
    int width;
    size_t char_len;
    if (buf[pos] == '\t') {
      width = 8 - (col % 8);
      char_len = 1;
    } else {
      char_len = sl_utf8_cluster_len_width(buf, row->end, pos, &width);
    }
    if (col + width > target_col)
      return pos;
    col += width;
    if (char_len == 0)
      char_len = 1;
    pos += char_len;
  }
  return row->end;
}

static int sl_move_visual(sl_t *self, const char *prompt, int direction) {
  sl_impl_t *impl;
  sl_render_t render;
  int target_row;
  int eof_row;
  size_t target;
  impl = sl_impl(self);
  if (!impl)
    return 0;
  if (sl_render_build(self, prompt, &render) != 0) {
    sl_render_free(&render);
    return 0;
  }
  target_row = render.cursor_row + direction;
  while (target_row >= render.editor_first && target_row < render.count &&
         !render.rows[target_row].has_cursor)
    target_row += direction;
  /* A full final row paints its EOF continuation only while the cursor is
   * there. Navigation may reach it without reserving an extra painted row. */
  eof_row = direction > 0 && target_row == render.count &&
            render.rows[render.count - 1].cols >= render.width &&
            render.rows[render.count - 1].end < impl->len;
  if (!eof_row &&
      (target_row < render.editor_first || target_row >= render.count)) {
    sl_render_free(&render);
    return 0;
  }
  if (!impl->preferred_column_valid ||
      impl->preferred_column_width != render.width) {
    impl->preferred_column = render.cursor_col;
    impl->preferred_column_width = render.width;
    impl->preferred_column_valid = 1;
  }
  target = eof_row
               ? impl->len
               : sl_cursor_for_render_col(&render, target_row,
                                          impl->preferred_column, impl->buf);
  sl_render_free(&render);
  if (target == impl->cursor)
    return 0;
  impl->cursor = target;
  return 1;
}

/* Reserve only the rows in the current frame, leaving two output rows. */
static int sl_native_prompt_height(sl_impl_t *impl, const sl_render_t *render) {
  int rows = render->count;
  int limit = sl_terminal_height(impl) - 2;
  if (rows > limit)
    rows = limit;
  return rows > 0 ? rows : 1;
}

/* Page the editor within its space while retaining the queue/status panel. */
static void sl_render_page_editor(sl_impl_t *impl, sl_render_t *render) {
  int rows, first, i, available;
  int height = sl_terminal_height(impl);
  int count = render->count - render->editor_first;
  rows = height / 2;
  available = height - render->editor_first;
  if (impl->output_stream_active || sl_surface_is_native(impl->output_surface))
    available -= 2;
  if (rows > available)
    rows = available;
  if (rows < 1)
    rows = 1;
  if (rows > count)
    rows = count;
  first = impl->editor_view_first;
  if (first > count - rows)
    first = count - rows;
  if (first > render->cursor_row - render->editor_first)
    first = render->cursor_row - render->editor_first;
  if (first + rows <= render->cursor_row - render->editor_first)
    first = render->cursor_row - render->editor_first - rows + 1;
  impl->editor_view_first = first;
  render->editor_offset = first;
  first += render->editor_first;
  for (i = render->editor_first; i < first; i++)
    free(render->rows[i].text);
  for (i = first + rows; i < render->count; i++)
    free(render->rows[i].text);
  memmove(render->rows + render->editor_first, render->rows + first,
          (size_t)rows * sizeof(*render->rows));
  render->cursor_row -= first - render->editor_first;
  render->count = render->editor_first + rows;
}

/* Materialize only the selected input viewport. Layout/navigation retain byte
 * positions, never hidden display strings or any producer transcript. */
static int sl_render_paint_editor(sl_t *self, const char *prompt,
                                  sl_render_t *render) {
  sl_impl_t *impl = sl_impl(self);
  char input_style[32];
  const char *prompt_style = sl_prompt_theme_style(impl->prompt_theme);
  int i;
  if (impl->history_search_active)
    prompt = "";
  if (sl_prompt_input_style(impl, input_style, sizeof(input_style)) != 0)
    return -1;
  for (i = render->editor_first; i < render->count; ++i) {
    sl_row_t *row = &render->rows[i];
    int indent = row->cols;
    size_t pos = row->text_first;
    /* The layout's initial column is independent of cursor ownership (a
     * continuation of a tab may have no addressable cursor position). */
    int base = prompt ? sl_text_width(prompt, strlen(prompt)) : 0;
    int initial = i == render->editor_first &&
                  (!render->editor_offset || base < render->width);
    if (!initial && base >= render->width)
      base = render->width > 1 ? render->width - 1 : 0;
    row->cols = 0;
    if (initial) {
      if (sl_row_append_styled(row, prompt ? prompt : "", prompt_style) != 0)
        return -1;
    } else {
      while (base-- > 0)
        if (sl_row_append_cells(row, " ", 1, 1) != 0)
          return -1;
    }
    if (sl_row_append(row, input_style, strlen(input_style)) != 0)
      return -1;
    while (pos < row->text_end) {
      int cells;
      size_t n;
      if (impl->buf[pos] == '\t') {
        cells = pos == row->text_first     ? row->leading_tab
                : pos + 1 == row->text_end ? row->trailing_tab
                                           : 8 - row->cols % 8;
        while (cells-- > 0)
          if (sl_row_append_cells(row, " ", 1, 1) != 0)
            return -1;
        ++pos;
      } else {
        n = sl_utf8_cluster_len_width(impl->buf, row->text_end, pos, &cells);
        if (!n)
          n = 1;
        if (sl_row_append_cells(row, impl->buf + pos, n, cells) != 0)
          return -1;
        pos += n;
      }
    }
    if (input_style[0] && sl_row_append(row, "\033[0m", 4) != 0)
      return -1;
    if (row->cols != indent)
      return -1;
  }
  return 0;
}

/* A retained editor frame still has physical cells after readline returns.
 * Reconcile those cells before either repainting or closing the session. */
static int sl_native_reconcile_rendered_frame(sl_t *self, int width,
                                              int height) {
  sl_impl_t *impl = sl_impl(self);
  struct winsize size;
  int span, height_only, old_offset, offset, observed;
  int column, editor_offset;
  int old_below;
  if (!sl_surface_is_native(impl->output_surface) || impl->rendered_rows == 0 ||
      (!sl_native_resize_pending(impl) && impl->rendered_width == width &&
       impl->rendered_height == height))
    return 0;
  span = impl->rendered_rows;
  height_only = impl->rendered_width == width;
  offset = old_offset = impl->rendered_cursor_row;
  observed = impl->cursor_position_probe == 1 || impl->output_reconcile_active
                 ? sl_query_cursor_row(self)
                 : 0;
  if (impl->output_reconcile_active && observed <= 0) {
    sl_set_error(self,
                 "terminal did not provide a cursor report for reconciliation");
    return -1;
  }
  /* A retained frame can remain above the new bottom after terminal growth.
   * Its cached top and height identify the physical cursor we last painted. */
  old_below = impl->rendered_height - 1 - (impl->rendered_top_row + old_offset);
  if (ioctl(impl->output_fd, TIOCGWINSZ, &size) == 0 &&
      (size.ws_col != width || size.ws_row != height))
    return 1;
  if (!height_only)
    sl_render_reflow_geometry(impl, width, &span, &offset, &column,
                              &editor_offset);
  /* A terminal may clamp a cursor at the old right edge to the last cell.
   * Honor the observed position rather than the predicted reflow row. */
  if (!height_only && observed > 0 && impl->rendered_cursor_col > 0 &&
      impl->rendered_cursor_col % width == 0 &&
      impl->probed_cursor_col == width - 1)
    offset--;
  if (observed > 0) {
    int observed_below = height - observed;
    if (height_only)
      sl_surface_native_history_shift(
          impl->output_surface, observed - 1 - offset - impl->rendered_top_row);
    sl_surface_native_prompt_reflow(
        impl->output_surface, observed_below - old_below + offset - old_offset);
    impl->rendered_top_row = observed - 1 - offset;
  } else {
    /* No input-owned cursor observation is available in a feed. This is an
     * endpoint estimate, not a model of the preceding transcript: earlier
     * hard-ended lines may have reflowed as well. Do not retain those lines
     * or wait here. See "Review exception: unobserved retained-frame resize"
     * in docs/softline-mdf-stream-design.md. */
    int line_rows = sl_surface_native_reflow_rows(impl->output_surface, width);
    int top = impl->rendered_top_row + line_rows;
    int space =
        height - impl->rendered_height - line_rows - (offset - old_offset);
    int available = height - 1 - (top + offset);
    /* Native reflow adds rows before the prompt; viewport growth or row
     * contraction can restore only available terminal history. Existing
     * blank rows below the cursor are preserved. No cells are replayed. */
    if (height < impl->rendered_height) {
      /* Height shrink retains the old physical cursor row, clamped to the
       * new screen, even when column reflow moves its logical row. */
      top = impl->rendered_top_row + old_offset;
      if (top >= height)
        top = height - 1;
      top -= offset;
      space = 0;
    }
    if (space > available)
      space = available;
    if (space > 0)
      top += sl_surface_native_history_growth(impl->output_surface, space);
    if (top + offset >= height)
      top -= top + offset - height + 1;
    sl_surface_native_history_shift(impl->output_surface,
                                    top - impl->rendered_top_row - line_rows);
    sl_surface_native_prompt_reflow(impl->output_surface,
                                    height - impl->rendered_height +
                                        impl->rendered_top_row - top);
    impl->rendered_top_row = top;
  }
  if (!height_only &&
      sl_render_store_reflow(
          impl, width, height, impl->rendered_top_row, span, offset,
          observed > 0 ? impl->probed_cursor_col : column, editor_offset) != 0)
    return -1;
  impl->rendered_height = height;
  impl->rendered_cursor_valid =
      observed > 0 && impl->probed_cursor_col == impl->rendered_cursor_col;
  if (sl_output_surface_reconcile(
          self, sl_prompt_top(impl, sl_output_prompt_rows(impl))) != 0)
    return -1;
  impl->rendered_width = width;
  if (ioctl(impl->output_fd, TIOCGWINSZ, &size) == 0 &&
      (size.ws_col != width || size.ws_row != height))
    return 1;
  return 0;
}

static int sl_render_apply_absolute(sl_t *self, sl_render_t *render) {
  sl_impl_t *impl;
  int visible;
  int first;
  int cursor_row;
  int top;
  int old_rows;
  int old_top;
  int clear_top;
  int clear_bottom;
  int current_row;
  int current_col;
  int i;
  int rc;
  int height;
  int width;
  int transcript_top;
  char input_style[32];
  int input_styled;
  struct winsize size;
  impl = sl_impl(self);
  if (!impl)
    return -1;
  if (sl_prompt_input_style(impl, input_style, sizeof(input_style)) != 0)
    return -1;
  input_styled = input_style[0] != '\0';
  if (ioctl(impl->output_fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 &&
      size.ws_row > 0) {
    height = size.ws_row;
    width = size.ws_col;
  } else {
    height = sl_terminal_height(impl);
    width = sl_terminal_width(impl);
  }
  if (render->width != width)
    return 1;
  /* Stream creation must page the frame before its surface exists. */
  if (impl->output_stream_active ||
      sl_surface_is_native(impl->output_surface)) {
    impl->native_prompt_rows = sl_native_prompt_height(impl, render);
  }
  if (sl_surface_is_native(impl->output_surface) && impl->rendered_rows > 0 &&
      (sl_native_resize_pending(impl) || impl->rendered_width != width ||
       impl->rendered_height != height)) {
    /* A retry must see acknowledged dimensions, so it cannot apply a resize
     * twice. Height-only changes preserve the terminal's prompt pixels. */
    int result = sl_native_reconcile_rendered_frame(self, width, height);
    if (result != 0)
      return result;
  }
  if (!impl->output_stream_active && !impl->output_surface &&
      impl->rendered_rows > 0 &&
      (impl->rendered_width != width || impl->rendered_height != height)) {
    clear_top = 0;
    clear_bottom = sl_terminal_bottom(impl);
    if (impl->auto_scroll_pinned) {
      clear_top = impl->rendered_top_row;
      clear_bottom = clear_top + impl->rendered_rows - 1;
      if (clear_top < 0)
        clear_top = 0;
      if (clear_bottom > sl_terminal_bottom(impl))
        clear_bottom = sl_terminal_bottom(impl);
    }
    for (i = clear_top; i <= clear_bottom; i++) {
      if (sl_write_frame_cursor(impl, height, i, 0) != 0 ||
          sl_clear_prompt_row(impl) != 0)
        return -1;
    }
    sl_render_store_clear(impl);
  }
  visible = render->count;
  if ((impl->output_stream_active ||
       sl_surface_is_native(impl->output_surface)) &&
      visible > sl_output_prompt_rows(impl))
    visible = sl_output_prompt_rows(impl);
  if (visible > height)
    visible = height;
  if (impl->output_stream_active && height > 1 && visible >= height)
    visible = height - 1;
  if (visible < 1)
    visible = 1;
  first = render->cursor_row - visible + 1;
  if (first < 0)
    first = 0;
  if (first > render->count - visible)
    first = render->count - visible;
  cursor_row = render->cursor_row - first;
  if (cursor_row < 0)
    cursor_row = 0;
  if (cursor_row >= visible)
    cursor_row = visible - 1;
  top = sl_prompt_top(impl, visible);
  transcript_top =
      (impl->output_stream_active || sl_surface_is_native(impl->output_surface))
          ? sl_prompt_top(impl, sl_output_prompt_rows(impl))
          : top;
  if ((impl->output_stream_active || impl->output_surface) &&
      (!impl->output_surface ||
       !sl_surface_matches(impl->output_surface, width, transcript_top))) {
    /* Retry resized frames after observing their new position. If no frame
     * exists, reconcile the producer before drawing the first prompt. */
    if (sl_native_resize_pending(impl) && impl->rendered_rows > 0)
      return 1;
    impl->rendered_cursor_valid = 0;
    if (sl_output_surface_reconcile(self, transcript_top) != 0) {
      sl_set_error(self, "failed to resize live output surface");
      return -1;
    }
  }
  if (impl->rendered_rows == 0)
    (void)sl_native_observe_producer(self);
  if (impl->render_reset) {
    if (sl_surface_is_native(impl->output_surface)) {
      if (sl_surface_native_restore_region(impl->output_surface) != 0)
        return -1;
    } else if (impl->auto_scroll_pinned &&
               sl_set_scroll_region(impl->output_fd, 0, top - 1) != 0) {
      return -1;
    }
    impl->rendered_cursor_valid = 0;
  }
  if (sl_render_visible_equal(impl, render, first, visible, cursor_row,
                              render->cursor_col, top)) {
    if (!impl->rendered_cursor_valid) {
      if (sl_write_frame_cursor(impl, height, top + cursor_row,
                                render->cursor_col) != 0 ||
          sl_show_cursor(impl) != 0)
        return -1;
      impl->rendered_cursor_valid = 1;
    }
    impl->rendered_width = width;
    impl->rendered_height = height;
    return 0;
  }
  old_rows = impl->rendered_rows;
  old_top = impl->rendered_top_row;
  current_row = impl->rendered_cursor_valid && old_rows > 0 && old_top == top
                    ? impl->rendered_cursor_row
                    : -1;
  current_col = impl->rendered_cursor_valid && old_rows > 0 && old_top == top
                    ? impl->rendered_cursor_col
                    : -1;
  rc = 0;

  if (!sl_surface_is_native(impl->output_surface) && old_rows > 0 &&
      old_top != top && top < old_top &&
      sl_scroll_region_up(impl, 0, old_top - 1, old_top - top) != 0)
    return -1;

  for (i = 0; rc == 0 && i < visible; i++) {
    int render_row;
    int old_row;
    int patch_row;
    int prefix_col;
    size_t prefix_len;
    render_row = first + i;
    old_row = top + i - old_top;
    if (old_row < 0 || old_row >= old_rows ||
        !sl_row_equal(impl, render, old_row, render_row)) {
      patch_row = 0;
      prefix_len = 0;
      prefix_col = 0;
      if (!impl->render_reset && !input_styled && old_row >= 0 &&
          old_row < old_rows &&
          impl->rendered_cols[old_row] <= render->rows[render_row].cols &&
          render_row >= render->editor_first) {
        prefix_len = sl_row_shared_prefix(
            impl->rendered_lines[old_row], impl->rendered_lens[old_row],
            render->rows[render_row].text, render->rows[render_row].len);
        if (prefix_len > 0) {
          prefix_col =
              sl_row_prefix_cols(render->rows[render_row].text, prefix_len);
          patch_row = 1;
        }
      }
      if (!patch_row) {
        if (sl_write_frame_cursor(impl, height, top + i, 0) != 0)
          rc = -1;
        if (rc == 0 &&
            (impl->render_reset || old_row < 0 || old_row >= old_rows ||
             impl->rendered_cols[old_row] > render->rows[render_row].cols) &&
            sl_clear_prompt_row(impl) != 0)
          rc = -1;
        if (rc == 0 && render->rows[render_row].len > 0 &&
            sl_write_all(impl->output_fd, render->rows[render_row].text,
                         render->rows[render_row].len) != 0)
          rc = -1;
        current_row = i;
        current_col = render->rows[render_row].cols;
      } else {
        if (current_row != i || current_col != prefix_col) {
          if (sl_write_frame_cursor(impl, height, top + i, prefix_col) != 0)
            rc = -1;
        }
        if (rc == 0 && prefix_len < render->rows[render_row].len &&
            sl_write_all(impl->output_fd,
                         render->rows[render_row].text + prefix_len,
                         render->rows[render_row].len - prefix_len) != 0)
          rc = -1;
        current_row = i;
        current_col = render->rows[render_row].cols;
      }
      if (rc == 0 &&
          (impl->render_reset || old_row < 0 || old_row >= old_rows ||
           impl->rendered_cols[old_row] > render->rows[render_row].cols) &&
          sl_clear_prompt_tail(impl) != 0)
        rc = -1;
    }
  }
  /* Clear only old prompt rows that are outside the new frame. */
  for (i = 0; rc == 0 && i < old_rows; i++) {
    int row = old_top + i;
    if (row < 0 || row >= height || (row >= top && row < top + visible))
      continue;
    if (sl_write_frame_cursor(impl, height, row, 0) != 0 ||
        sl_clear_prompt_row(impl) != 0)
      rc = -1;
    current_row = current_col = -1;
  }
  if (rc == 0 &&
      (current_row != cursor_row || current_col != render->cursor_col) &&
      sl_write_frame_cursor(impl, height, top + cursor_row,
                            render->cursor_col) != 0)
    rc = -1;
  if (rc == 0 && sl_show_cursor(impl) != 0)
    rc = -1;
  if (rc == 0 &&
      sl_render_store_update(impl, render, first, visible, cursor_row,
                             render->cursor_col, top) != 0) {
    sl_set_error(self, "out of memory while storing terminal render state");
    rc = -1;
  }
  if (rc == 0) {
    impl->rendered_width = width;
    impl->rendered_height = height;
  }
  if (rc != 0)
    (void)sl_show_cursor(impl);
  return rc;
}

static int sl_render_apply(sl_t *self, const char *prompt) {
  sl_impl_t *impl;
  sl_render_t render;
  int old_rows;
  int max_rows;
  int i;
  int fd;
  int rc;
  int resizing_width;
  int width;
  impl = sl_impl(self);
  if (!impl)
    return -1;
  if (!impl->retained_prompt || strcmp(impl->retained_prompt, prompt) != 0) {
    char *copy = sl_strdup(prompt);
    if (!copy) {
      sl_set_error(self, "failed to retain prompt text");
      return -1;
    }
    free(impl->retained_prompt);
    impl->retained_prompt = copy;
  }
retry:
  if (sl_render_build(self, prompt, &render) != 0) {
    sl_render_free(&render);
    sl_set_error(self, "failed to render editor state");
    return -1;
  }
  if (impl->preferred_column_valid &&
      impl->preferred_column_width != render.width)
    impl->preferred_column_valid = 0;
  sl_render_page_editor(impl, &render);
  if (sl_render_paint_editor(self, prompt, &render) != 0) {
    sl_render_free(&render);
    sl_set_error(self, "failed to format prompt viewport");
    return -1;
  }
  if (sl_prompt_uses_absolute_rows(impl)) {
    rc = sl_render_apply_absolute(self, &render);
    sl_render_free(&render);
    if (rc == 1)
      goto retry;
    if (rc != 0)
      sl_set_error(self, "failed to write prompt terminal output");
    return rc;
  }
  width = sl_terminal_width(impl);
  resizing_width = impl->rendered_rows > 0 && impl->rendered_width != width;
  if (impl->rendered_rows > 0 &&
      (resizing_width || impl->rendered_height != sl_terminal_height(impl))) {
    int columns = sl_terminal_columns(impl);
    int height = sl_terminal_height(impl);
    int span, offset, column, editor_offset, observed, top;
    /* Locate the old frame after terminal reflow. The new layout's row count
     * says nothing about which old row contains the hardware cursor. */
    sl_render_reflow_geometry(impl, columns, &span, &offset, &column,
                              &editor_offset);
    observed = sl_query_cursor_row(self);
    /* A cursor just beyond the text can be clamped to the final cell rather
     * than reflowed onto another row. Use the physical report in that case. */
    if (observed > 0 && impl->rendered_cursor_col > 0 &&
        impl->rendered_cursor_col % columns == 0 &&
        impl->probed_cursor_col == columns - 1)
      offset--;
    top = observed > 0 ? observed - 1 - offset : 0;
    if (sl_render_store_reflow(impl, columns, height, top, span, offset,
                               observed > 0 ? impl->probed_cursor_col : column,
                               editor_offset) != 0) {
      sl_render_free(&render);
      sl_set_error(self, "failed to reconcile readline prompt after resize");
      return -1;
    }
    impl->rendered_top_row = -1;
  }
  if (sl_render_visible_equal(impl, &render, 0, render.count, render.cursor_row,
                              render.cursor_col, -1)) {
    impl->rendered_width = width;
    impl->rendered_height = sl_terminal_height(impl);
    sl_render_free(&render);
    return 0;
  }
  fd = impl->output_fd;
  rc = 0;
  old_rows = impl->rendered_rows;
  /* Return to the first owned prompt row, then let terminal scrolling make
   * room only when the replacement frame reaches the bottom. */
  if (old_rows > 0 && (sl_wchar(fd, '\r') != 0 ||
                       sl_write_cursor_up(fd, impl->rendered_cursor_row) != 0))
    rc = -1;
  max_rows = old_rows > render.count ? old_rows : render.count;
  for (i = 0; rc == 0 && i < max_rows; i++) {
    if (i >= render.count || !sl_row_equal(impl, &render, i, i)) {
      if (sl_wchar(fd, '\r') != 0)
        rc = -1;
      if (rc == 0 && i < render.count && render.rows[i].len > 0 &&
          sl_write_all(fd, render.rows[i].text, render.rows[i].len) != 0)
        rc = -1;
      if (rc == 0 &&
          (impl->render_reset || i >= render.count ||
           (resizing_width && i < render.count &&
            render.rows[i].cols < width) ||
           (i < old_rows && impl->rendered_cols[i] > render.rows[i].cols)) &&
          sl_wstr(fd, "\033[0K") != 0)
        rc = -1;
    }
    if (rc == 0 && i + 1 < max_rows) {
      if (sl_write_line_break(impl) != 0)
        rc = -1;
    }
  }
  if (rc == 0) {
    if (sl_write_cursor_up(fd, max_rows - 1 - render.cursor_row) != 0 ||
        sl_wchar(fd, '\r') != 0 ||
        sl_write_cursor_forward(fd, render.cursor_col) != 0)
      rc = -1;
  }
  if (rc == 0 &&
      sl_render_store_update(impl, &render, 0, render.count, render.cursor_row,
                             render.cursor_col, -1) != 0) {
    sl_set_error(self, "out of memory while storing terminal render state");
    rc = -1;
  }
  if (rc == 0) {
    impl->rendered_width = width;
    impl->rendered_height = sl_terminal_height(impl);
  }
  sl_render_free(&render);
  if (rc != 0)
    sl_set_error(self, "failed to write terminal output");
  return rc;
}

static int sl_native_session_close(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  int prompt_row = -1;
  int i, row;
  if (impl->rendered_rows > 0) {
    int result = 1;
    for (i = 0; i < 3 && result == 1; i++)
      result = sl_native_reconcile_rendered_frame(
          self, sl_terminal_columns(impl), sl_terminal_rows(impl));
    if (result != 0)
      return -1;
  }
  if (impl->rendered_rows == 0) {
    if (sl_native_resize_pending(impl)) {
      if (sl_output_surface_reconcile(self, sl_terminal_rows(impl)) != 0)
        return -1;
    } else {
      (void)sl_native_observe_producer(self);
    }
  }
  if (!impl->clear_prompt_on_exit && impl->rendered_rows > 0) {
    prompt_row = impl->rendered_top_row + impl->rendered_cursor_row;
    /* The cached frame separates editor cells from queue and status rows.
     * Clear only visible input rows and leave the rest of the screen intact. */
    for (i = impl->rendered_editor_first; i < impl->rendered_rows; i++) {
      int row = impl->rendered_top_row + i;
      if (row < 0 || row >= sl_terminal_rows(impl))
        continue;
      if (sl_write_cursor_pos(impl->output_fd, row, 0) != 0 ||
          sl_clear_prompt_row(impl) != 0)
        return -1;
    }
  }
  if (impl->clear_prompt_on_exit && sl_render_clear_active(self) != 0)
    return -1;
  if (prompt_row >= sl_terminal_rows(impl))
    prompt_row = sl_terminal_rows(impl) - 1;
  sl_surface_native_position(impl->output_surface, &row, NULL);
  if (prompt_row >= 0)
    row = prompt_row;
  else if (sl_surface_native_exit_advances(impl->output_surface))
    row++;
  if (row >= sl_terminal_rows(impl))
    row = sl_terminal_rows(impl) - 1;
  if (row < 0)
    row = 0;
  if (sl_surface_native_finish(impl->output_surface, prompt_row) != 0)
    return -1;
  impl->native_cursor_below = sl_terminal_rows(impl) - 1 - row;
  impl->native_cursor_valid = 1;
  impl->native_history_rows =
      sl_surface_native_history_rows(impl->output_surface);
  impl->cursor_hidden = 0;
  sl_surface_destroy(impl->output_surface);
  impl->output_surface = NULL;
  sl_render_store_clear(impl);
  return sl_show_cursor(impl);
}

static int sl_render_finish(sl_t *self, int queue_dispatch) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl)
    return -1;
  if (impl->prompt_theme != SL_PROMPT_THEME_PLAIN &&
      sl_wstr(impl->output_fd, "\033[0m") != 0)
    return -1;
  if (sl_surface_is_native(impl->output_surface)) {
    if (impl->clear_prompt_on_exit && sl_render_clear_active(self) != 0)
      return -1;
    if (!impl->output_stream_active)
      return sl_native_session_close(self);
    return 0;
  }
  if (impl->auto_scroll_pinned) {
    if (queue_dispatch && sl_prompt_queue_enabled(impl)) {
      int top;
      top = impl->rendered_top_row;
      if (sl_hide_cursor(impl) != 0 || sl_render_clear_active(self) != 0) {
        (void)sl_show_cursor(impl);
        return -1;
      }
      sl_release_auto_scroll_region(impl);
      if (sl_write_cursor_pos(impl->output_fd, top, 0) != 0 ||
          sl_show_cursor(impl) != 0)
        return -1;
      return 0;
    }
    sl_release_auto_scroll_region(impl);
    if (sl_write_line_break(impl) != 0) {
      sl_set_error(self, "failed to write final newline");
      return -1;
    }
    sl_render_store_clear(impl);
    return 0;
  }
  if (sl_prompt_uses_absolute_rows(impl)) {
    if (impl->output_stream_active ||
        (queue_dispatch && sl_prompt_queue_enabled(impl))) {
      if (sl_hide_cursor(impl) != 0 || sl_render_clear_active(self) != 0) {
        (void)sl_show_cursor(impl);
        return -1;
      }
    }
    if (sl_write_cursor_pos(impl->output_fd, sl_terminal_bottom(impl) + 1, 0) !=
        0)
      return -1;
    sl_render_store_clear(impl);
    return 0;
  }
  if (queue_dispatch && sl_prompt_queue_enabled(impl)) {
    if (sl_render_clear_active(self) != 0) {
      sl_set_error(self, "failed to clear queued prompt before dispatch");
      return -1;
    }
    return 0;
  }
  if (sl_write_line_break(impl) != 0) {
    sl_set_error(self, "failed to write final newline");
    return -1;
  }
  /* Ordinary readline advances the terminal outside the output region.
   * Its retained cells can no longer be safely replayed by a later stream. */
  sl_surface_destroy(impl->output_surface);
  impl->output_surface = NULL;
  sl_render_store_clear(impl);
  return 0;
}

static int sl_render_clear_active(sl_t *self) {
  sl_impl_t *impl;
  int i;
  int rows;
  impl = sl_impl(self);
  if (!impl || impl->rendered_rows <= 0)
    return 0;
  if (sl_native_resize_pending(impl)) {
    /* A physical resize invalidates cached prompt coordinates as well as
     * transcript coordinates. Never erase the screen using the old ones. */
    sl_render_store_clear(impl);
    return 0;
  }
  if (sl_prompt_uses_absolute_rows(impl)) {
    if (sl_hide_cursor(impl) != 0)
      return -1;
    for (i = 0; i < impl->rendered_rows; i++) {
      if (impl->rendered_top_row + i < 0 ||
          impl->rendered_top_row + i >= sl_terminal_rows(impl))
        continue;
      if (sl_write_cursor_pos(impl->output_fd, impl->rendered_top_row + i, 0) !=
              0 ||
          sl_clear_prompt_row(impl) != 0)
        return -1;
    }
    sl_render_store_clear(impl);
    if (impl->prompt_theme != SL_PROMPT_THEME_PLAIN &&
        sl_wstr(impl->output_fd, "\033[0m") != 0)
      return -1;
    return 0;
  }
  rows = impl->rendered_rows;
  if (sl_wchar(impl->output_fd, '\r') != 0 ||
      sl_write_cursor_up(impl->output_fd, impl->rendered_cursor_row) != 0)
    return -1;
  for (i = 0; i < rows; i++) {
    if (sl_wstr(impl->output_fd, "\r\033[0K") != 0)
      return -1;
    if (i + 1 < rows && (sl_write_cursor_down(impl->output_fd, 1) != 0 ||
                         sl_wchar(impl->output_fd, '\r') != 0))
      return -1;
  }
  if (sl_write_cursor_up(impl->output_fd, rows - 1) != 0 ||
      sl_wchar(impl->output_fd, '\r') != 0)
    return -1;
  sl_render_store_clear(impl);
  if (impl->prompt_theme != SL_PROMPT_THEME_PLAIN &&
      sl_wstr(impl->output_fd, "\033[0m") != 0)
    return -1;
  return 0;
}

static int sl_write_stream_chunk(sl_impl_t *impl, const char *chunk,
                                 size_t len) {
  const char *p;
  const char *start;
  size_t n;
  int fd;
  if (!impl)
    return -1;
  fd = impl->output_fd;
  p = chunk;
  start = chunk;
  while ((size_t)(p - chunk) < len) {
    if (*p == '\n') {
      n = (size_t)(p - start);
      if (n > 0 && sl_write_all(fd, start, n) != 0)
        return -1;
      if (sl_write_line_break(impl) != 0)
        return -1;
      p++;
      start = p;
    } else {
      p++;
    }
  }
  n = (size_t)(p - start);
  if (n > 0 && sl_write_all(fd, start, n) != 0)
    return -1;
  return 0;
}

static int sl_write_stream(sl_t *self, sl_stream_callback_t callback,
                           void *userdata) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || !callback)
    return SL_ERROR_INVALID;
  for (;;) {
    const char *chunk;
    size_t len;
    int rc;
    chunk = NULL;
    len = 0;
    rc = callback(self, userdata, &chunk, &len);
    if (rc != SL_OK)
      return rc;
    if (len == 0)
      return SL_OK;
    if (!chunk)
      return SL_ERROR_INVALID;
    if (sl_write_stream_chunk(impl, chunk, len) != 0)
      return SL_ERROR_IO;
    if (!isatty(impl->input_fd) || !isatty(impl->output_fd))
      sl_output_track_tail(impl, chunk, len);
  }
}

/* A live transcript owns the row immediately above the fixed prompt. Scroll
 * before its first byte and defer a trailing newline, so a complete message
 * remains on that bottom transcript row instead of being scrolled one row too
 * far upward. */
static int sl_write_scroll_stream_chunk(sl_impl_t *impl, const char *chunk,
                                        size_t len, int *pending_newline) {
  const char *p;
  const char *start;
  size_t n;
  if (!impl || !chunk || !pending_newline)
    return -1;
  p = chunk;
  start = chunk;
  while ((size_t)(p - chunk) < len) {
    if (*p == '\n') {
      n = (size_t)(p - start);
      if (*pending_newline && sl_write_line_break(impl) != 0)
        return -1;
      *pending_newline = 1;
      if (n > 0 && sl_write_all(impl->output_fd, start, n) != 0)
        return -1;
      p++;
      start = p;
    } else {
      if (*pending_newline) {
        if (sl_write_line_break(impl) != 0)
          return -1;
        *pending_newline = 0;
      }
      p++;
    }
  }
  n = (size_t)(p - start);
  if (*pending_newline && n > 0) {
    if (sl_write_line_break(impl) != 0)
      return -1;
    *pending_newline = 0;
  }
  if (n > 0 && sl_write_all(impl->output_fd, start, n) != 0)
    return -1;
  return 0;
}

static int sl_write_scroll_stream(sl_t *self, sl_stream_callback_t callback,
                                  void *userdata) {
  sl_impl_t *impl;
  int started;
  int pending_newline;
  impl = sl_impl(self);
  if (!impl || !callback)
    return SL_ERROR_INVALID;
  started = 0;
  pending_newline = 0;
  for (;;) {
    const char *chunk;
    size_t len;
    int rc;
    chunk = NULL;
    len = 0;
    rc = callback(self, userdata, &chunk, &len);
    if (rc != SL_OK)
      return rc;
    if (len == 0)
      return SL_OK;
    if (!chunk)
      return SL_ERROR_INVALID;
    if (!started) {
      if (sl_write_line_break(impl) != 0)
        return SL_ERROR_IO;
      started = 1;
    }
    if (sl_write_scroll_stream_chunk(impl, chunk, len, &pending_newline) != 0)
      return SL_ERROR_IO;
  }
}

/* Finite callbacks and live writes share native geometry reconciliation. */
static int sl_prepare_native_output(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl)
    return SL_ERROR_INVALID;
  if (impl->active_prompt && sl_render_apply(self, impl->active_prompt) != 0)
    return SL_ERROR_IO;
  if (!impl->output_surface) {
    if (sl_output_surface_reconcile(
            self, sl_prompt_top(impl, sl_output_prompt_rows(impl))) != 0) {
      sl_set_error(self, "failed to resize native output surface");
      return SL_ERROR_IO;
    }
  } else if (!impl->active_prompt) {
    int old_width;
    int old_terminal_rows;
    sl_surface_geometry(impl->output_surface, &old_width, NULL,
                        &old_terminal_rows);
    if (old_width != sl_terminal_width(impl) ||
        old_terminal_rows != sl_terminal_rows(impl)) {
      if (impl->rendered_rows > 0) {
        int attempt, result = 1;
        for (attempt = 0; attempt < 3 && result == 1; attempt++)
          result = sl_native_reconcile_rendered_frame(
              self, sl_terminal_columns(impl), sl_terminal_rows(impl));
        if (result != 0) {
          sl_set_error(self,
                       "failed to reconcile retained prompt after resize");
          return SL_ERROR_IO;
        }
      } else {
        /* No editor cells exist to reserve below the output cursor. */
        if (sl_output_surface_reconcile(self, sl_terminal_rows(impl)) != 0) {
          sl_set_error(self, "failed to resize native output surface");
          return SL_ERROR_IO;
        }
      }
    }
  }
  /* Margin changes home the cursor. A partial or rejected fragment may emit
   * no producer bytes, so restore the retained input cursor here rather than
   * depending on the suffix of a later native write. */
  if (!impl->active_prompt && impl->rendered_rows > 0 &&
      !impl->rendered_cursor_valid) {
    if (sl_write_frame_cursor(impl, impl->rendered_height,
                              impl->rendered_top_row +
                                  impl->rendered_cursor_row,
                              impl->rendered_cursor_col) != 0) {
      sl_set_error(self, "failed to restore retained prompt cursor");
      return SL_ERROR_IO;
    }
    impl->rendered_cursor_valid = 1;
  }
  return SL_OK;
}

/* Finite output reuses the native cursor when a session is retained. */
static int sl_print_above_surface(sl_t *self, sl_stream_callback_t callback,
                                  void *userdata) {
  sl_impl_t *impl;
  int result;
  int prompt_top;
  impl = sl_impl(self);
  if (!impl || !callback)
    return SL_ERROR_INVALID;
  result = sl_prepare_native_output(self);
  if (result != SL_OK)
    return result;
  prompt_top = sl_prompt_top(impl, sl_output_prompt_rows(impl));
  result = SL_OK;
  for (;;) {
    const char *bytes;
    size_t length;
    int status;
    bytes = NULL;
    length = 0;
    status = callback(self, userdata, &bytes, &length);
    /* A producer callback may span another physical resize. */
    if (sl_prepare_native_output(self) != SL_OK) {
      result = SL_ERROR_IO;
      break;
    }
    prompt_top = sl_prompt_top(impl, sl_output_prompt_rows(impl));
    if (status != SL_OK) {
      result = status;
      break;
    }
    if (length == 0)
      break;
    if (!bytes) {
      sl_set_error(self, "output callback returned a NULL nonempty chunk");
      result = SL_ERROR_INVALID;
      break;
    }
    status = sl_output_write_validated(self, bytes, length);
    if (status != 0) {
      sl_set_error(self, status == -2 ? "invalid output byte sequence"
                                      : "failed to render output bytes");
      result = status == -2 ? SL_ERROR_INVALID : SL_ERROR_IO;
      break;
    }
  }
  if (result == SL_OK && !sl_surface_complete(impl->output_surface)) {
    sl_set_error(self, "output ends in an incomplete ANSI or UTF-8 sequence");
    result = SL_ERROR_INVALID;
  }
  if (result != SL_OK) {
    sl_surface_reset_partial(impl->output_surface);
    impl->output_pending_len = 0;
  }
  if (impl->active_prompt && impl->rendered_rows > 0 &&
      sl_write_frame_cursor(impl, impl->rendered_height,
                            impl->rendered_top_row + impl->rendered_cursor_row,
                            impl->rendered_cursor_col) != 0)
    result = SL_ERROR_IO;
  if (!impl->active_prompt)
    (void)sl_native_observe_producer(self);
  if (!impl->active_prompt &&
      sl_write_frame_cursor(impl, sl_terminal_rows(impl), prompt_top, 0) != 0) {
    sl_set_error(self, "failed to position cursor after finite output");
    result = SL_ERROR_IO;
  }
  if (sl_show_cursor(impl) != 0)
    result = SL_ERROR_IO;
  return result;
}

static int sl_print_above_method_impl(sl_t *self, sl_stream_callback_t callback,
                                      void *userdata) {
  sl_impl_t *impl;
  int prompt_rows;
  int prompt_top;
  int content_top;
  int content_bottom;
  int rc;
  impl = sl_impl(self);
  if (!impl)
    return SL_ERROR_INVALID;
  if (impl->output_stream_active) {
    sl_set_error(self, "print_above cannot overlap a live output stream");
    return SL_ERROR_INVALID;
  }
  if (!impl->active_prompt && !impl->output_surface &&
      impl->native_cursor_valid && isatty(impl->input_fd) &&
      isatty(impl->output_fd)) {
    prompt_top = sl_prompt_top(impl, sl_output_prompt_rows(impl));
    if (sl_output_surface_reconcile(self, prompt_top) != 0)
      return SL_ERROR_IO;
    rc = sl_print_above_surface(self, callback, userdata);
    if (sl_native_session_close(self) != 0)
      rc = SL_ERROR_IO;
    sl_disable_raw(self);
    return rc;
  }
  if (!sl_prompt_uses_absolute_rows(impl) && impl->live_scroll_region &&
      impl->active_prompt)
    (void)sl_try_pin_scroll_region(self);
  if (sl_prompt_uses_absolute_rows(impl)) {
    prompt_rows = sl_surface_is_native(impl->output_surface)
                      ? sl_output_prompt_rows(impl)
                  : impl->rendered_rows > 0 ? impl->rendered_rows
                                            : 1;
    prompt_top = sl_prompt_top(impl, prompt_rows);
    content_top = 0;
    content_bottom = prompt_top - 1;
    if (content_bottom >= content_top) {
      if (!isatty(impl->input_fd) || !isatty(impl->output_fd))
        return sl_write_stream(self, callback, userdata);
      if (impl->output_surface)
        return sl_print_above_surface(self, callback, userdata);
      if (impl->auto_scroll_pinned && impl->active_prompt &&
          impl->rendered_rows > 0 && impl->rendered_top_row < 0) {
        impl->rendered_top_row = prompt_top;
        impl->rendered_width = sl_terminal_width(impl);
        impl->rendered_height = sl_terminal_height(impl);
      }
      if ((impl->active_prompt || impl->cursor_hidden) &&
          sl_hide_cursor(impl) != 0)
        return SL_ERROR_IO;
      if (sl_set_scroll_region(impl->output_fd, content_top, content_bottom) !=
              0 ||
          sl_write_cursor_pos(impl->output_fd, content_bottom, 0) != 0) {
        (void)sl_show_cursor(impl);
        return SL_ERROR_IO;
      }
      rc = impl->auto_scroll_pinned
               ? sl_write_scroll_stream(self, callback, userdata)
               : sl_write_stream(self, callback, userdata);
      if (sl_reset_scroll_region(impl->output_fd) != 0) {
        (void)sl_show_cursor(impl);
        return SL_ERROR_IO;
      }
      if (rc != SL_OK) {
        if (impl->active_prompt && impl->rendered_rows > 0 &&
            sl_write_cursor_pos(impl->output_fd,
                                impl->rendered_top_row +
                                    impl->rendered_cursor_row,
                                impl->rendered_cursor_col) != 0)
          sl_render_store_clear(impl);
        (void)sl_show_cursor(impl);
        return rc;
      }
      if (impl->active_prompt && impl->rendered_rows > 0) {
        if (impl->rendered_top_row < 0) {
          impl->rendered_top_row = prompt_top;
          impl->rendered_width = sl_terminal_width(impl);
          impl->rendered_height = sl_terminal_height(impl);
        }
        if (sl_write_cursor_pos(impl->output_fd,
                                impl->rendered_top_row +
                                    impl->rendered_cursor_row,
                                impl->rendered_cursor_col) != 0 ||
            sl_show_cursor(impl) != 0)
          return SL_ERROR_IO;
      } else if (impl->active_prompt &&
                 sl_render_apply(self, impl->active_prompt) != 0) {
        (void)sl_show_cursor(impl);
        return SL_ERROR_IO;
      }
      return SL_OK;
    }
    sl_set_error(self, "prompt has no space above it");
    (void)sl_show_cursor(impl);
    return SL_ERROR_INVALID;
  }
  if (impl->active_prompt) {
    if (sl_render_clear_active(self) != 0) {
      sl_set_error(self, "failed to clear active prompt before printing");
      return SL_ERROR_IO;
    }
  }
  rc = sl_write_stream(self, callback, userdata);
  if (rc != SL_OK)
    return rc;
  if (impl->active_prompt && sl_render_apply(self, impl->active_prompt) != 0)
    return SL_ERROR_IO;
  return SL_OK;
}

/* Finite producer callbacks share the zero-wait feed contract, including
 * surface setup, retained-frame resize and session teardown. */
static int sl_print_above_method(sl_t *self, sl_stream_callback_t callback,
                                 void *userdata) {
  sl_impl_t *impl = sl_impl(self);
  int previous, result;
  if (!impl)
    return SL_ERROR_INVALID;
  previous = impl->output_write_active;
  impl->output_write_active = 1;
  result = sl_print_above_method_impl(self, callback, userdata);
  impl->output_write_active = previous;
  if (result == SL_OK && impl->active_prompt && impl->live_scroll_region &&
      !sl_prompt_uses_absolute_rows(impl))
    impl->scroll_region_probe_pending = 1;
  return result;
}

static int sl_output_stream_begin_method(sl_t *self) {
  sl_impl_t *impl;
  sl_surface_t *previous_surface;
  impl = sl_impl(self);
  if (!impl || impl->output_stream_active) {
    sl_set_error(self, "live output stream is already open");
    return SL_ERROR_INVALID;
  }
  previous_surface = impl->output_surface;
  if (!sl_surface_is_native(impl->output_surface) && impl->active_prompt &&
      impl->rendered_rows > 0 && sl_render_clear_active(self) != 0) {
    sl_set_error(self, "failed to clear prompt for live output");
    return SL_ERROR_IO;
  }
  sl_release_auto_scroll_region(impl);
  impl->output_stream_active = 1;
  impl->output_ansi_state = 0;
  impl->output_pending_len = 0;
  if (isatty(impl->input_fd) && isatty(impl->output_fd) &&
      impl->native_prompt_rows == 0) {
    sl_render_t initial;
    if (sl_render_build(self, "> ", &initial) != 0) {
      sl_render_free(&initial);
      goto failed;
    }
    sl_render_page_editor(impl, &initial);
    impl->native_prompt_rows = sl_native_prompt_height(impl, &initial);
    sl_render_free(&initial);
  }

  if (!isatty(impl->input_fd) || !isatty(impl->output_fd)) {
    sl_surface_destroy(previous_surface);
    impl->output_surface = NULL;
    previous_surface = NULL;
    impl->output_surface = sl_surface_create_validator();
    if (!impl->output_surface) {
      sl_set_error(self, "failed to create live output validator");
      goto failed;
    }
  } else if (impl->active_prompt) {
    if (sl_render_apply(self, impl->active_prompt) != 0)
      goto failed;
  } else if (sl_output_surface_reconcile(
                 self, sl_prompt_top(impl, sl_output_prompt_rows(impl))) != 0) {
    sl_set_error(self, "failed to create live output surface");
    goto failed;
  }
  return SL_OK;

failed:
  if (!previous_surface) {
    sl_surface_destroy(impl->output_surface);
    impl->output_surface = NULL;
  }
  impl->output_stream_active = 0;
  return SL_ERROR_IO;
}

static int sl_output_stream_reconcile_method(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  int result;
  if (!impl || !impl->output_stream_active || impl->output_write_active ||
      impl->output_reconcile_active) {
    sl_set_error(self, "reconciliation requires an open stream outside a feed");
    return SL_ERROR_INVALID;
  }
  if (!isatty(impl->input_fd) || !isatty(impl->output_fd))
    return SL_OK;
  /* This explicit owner operation may observe input. Never enter it from a
   * producer feed, and leave its incremental UTF-8/SGR validator untouched. */
  impl->output_reconcile_active = 1;
  if (impl->cursor_position_probe < 0)
    impl->cursor_position_probe = 0;
  if (impl->rendered_rows > 0)
    result = sl_render_apply(self, impl->active_prompt ? impl->active_prompt
                                                       : impl->retained_prompt);
  else
    result = sl_prepare_native_output(self) == SL_OK ? 0 : -1;
  impl->output_reconcile_active = 0;
  return result == 0 ? SL_OK : SL_ERROR_IO;
}

static void sl_output_track_tail(sl_impl_t *impl, const char *bytes,
                                 size_t length) {
  size_t i;
  for (i = 0; i < length; i++) {
    unsigned char byte = (unsigned char)bytes[i];
    if (impl->output_ansi_state == 1) {
      impl->output_ansi_state = byte == '[' ? 2 : 0;
      continue;
    }
    if (impl->output_ansi_state == 2) {
      if (byte >= 0x40 && byte <= 0x7e)
        impl->output_ansi_state = 0;
      continue;
    }
    if (byte == 0x1b)
      impl->output_ansi_state = 1;
    else if (byte == '\n') {
      if (impl->output_trailing_newlines < 2)
        impl->output_trailing_newlines++;
    } else if (byte != '\r')
      impl->output_trailing_newlines = 0;
  }
}

/* SIGINT/EOF can restore raw mode while retaining a native stream. Borrow
 * only its output flags for this write, including when input and output share
 * a TTY temporarily acquired for a cursor report. Never flush queued input. */
static int sl_output_write_native(sl_t *self, const char *bytes, size_t length,
                                  int row) {
  sl_impl_t *impl = sl_impl(self);
  struct termios output;
  tcflag_t original_flags = 0;
  int changed = 0, result;
  if (!impl->raw_active) {
    if (tcgetattr(impl->output_fd, &output) != 0) {
      sl_set_error(self, "failed to read native output terminal attributes");
      return -1;
    }
    original_flags = output.c_oflag;
    output.c_oflag = sl_native_output_flags(output.c_oflag);
    changed = output.c_oflag != original_flags;
    if (changed && tcsetattr(impl->output_fd, TCSANOW, &output) != 0) {
      sl_set_error(self, "failed to disable output kernel tab expansion");
      return -1;
    }
  }
  result = sl_surface_native_write(impl->output_surface, bytes, length, row,
                                   impl->rendered_cursor_col);
  if (changed) {
    output.c_oflag = original_flags;
    if (tcsetattr(impl->output_fd, TCSANOW, &output) != 0) {
      sl_set_error(self, "failed to restore native output terminal attributes");
      return -1;
    }
  }
  if (result != 0)
    sl_set_error(self, "failed to write native output");
  return result;
}

static int sl_output_flush_redirected(sl_t *self, const char *bytes,
                                      size_t length) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (length == 0)
    return SL_OK;
  if (sl_surface_is_native(impl->output_surface)) {
    int row;
    int written;
    /* Validation may span a resize after the session was prepared. Use the
     * current native geometry before addressing either output or input.
     * A physical resize can still race the subsequent VT write; this is an
     * accepted limitation, including ASCII. See the concurrent physical
     * resize exception in docs/softline-mdf-stream-design.md. */
    if (sl_native_resize_pending(impl) &&
        sl_prepare_native_output(self) != SL_OK)
      return SL_ERROR_IO;
    row = impl->rendered_rows > 0
              ? impl->rendered_top_row + impl->rendered_cursor_row
              : -1;
    /* Feed writes have no terminal round trip, including Unicode chunks. */
    written = sl_output_write_native(self, bytes, length, row);
    if (written != 0)
      return SL_ERROR_IO;
    impl->rendered_cursor_valid = row >= 0;
  } else if (sl_write_all(impl->output_fd, bytes, length) != 0) {
    sl_set_error(self, "failed to write live output");
    return SL_ERROR_IO;
  }
  sl_output_track_tail(impl, bytes, length);
  return SL_OK;
}

static int sl_output_write_validated(sl_t *self, const char *bytes,
                                     size_t length) {
  sl_impl_t *impl;
  int result;
  char ready[4096];
  size_t ready_len;
  size_t i;
  impl = sl_impl(self);
  ready_len = 0;
  for (i = 0; i < length; i++) {
    size_t accepted;
    if (ready_len > sizeof(ready) - sizeof(impl->output_pending)) {
      if (sl_output_flush_redirected(self, ready, ready_len) != SL_OK)
        return SL_ERROR_IO;
      ready_len = 0;
    }

    result = sl_surface_validate(impl->output_surface, bytes + i, 1, &accepted);
    if (result != 0) {
      impl->output_pending_len = 0;
      if (sl_output_flush_redirected(self, ready, ready_len) != SL_OK)
        return SL_ERROR_IO;
      sl_set_error(
          self, "live output contains an unsupported or invalid byte sequence");
      return SL_ERROR_INVALID;
    }
    if (impl->output_pending_len > 0 ||
        !sl_surface_complete(impl->output_surface)) {
      if (impl->output_pending_len >= sizeof(impl->output_pending)) {
        impl->output_pending_len = 0;
        sl_surface_reset_partial(impl->output_surface);
        if (sl_output_flush_redirected(self, ready, ready_len) != SL_OK)
          return SL_ERROR_IO;
        sl_set_error(self, "live output escape sequence is too long");
        return SL_ERROR_INVALID;
      }
      impl->output_pending[impl->output_pending_len++] = bytes[i];
      if (!sl_surface_complete(impl->output_surface))
        continue;
      if (impl->output_pending_len > sizeof(ready) - ready_len) {
        if (sl_output_flush_redirected(self, ready, ready_len) != SL_OK)
          return SL_ERROR_IO;
        ready_len = 0;
      }
      memcpy(ready + ready_len, impl->output_pending, impl->output_pending_len);
      ready_len += impl->output_pending_len;
      impl->output_pending_len = 0;
    } else {
      if (ready_len == sizeof(ready)) {
        if (sl_output_flush_redirected(self, ready, ready_len) != SL_OK)
          return SL_ERROR_IO;
        ready_len = 0;
      }
      ready[ready_len++] = bytes[i];
    }
  }
  return sl_output_flush_redirected(self, ready, ready_len);
}

static int sl_output_stream_write_method(sl_t *self, const char *bytes,
                                         size_t length) {
  sl_impl_t *impl;
  int result, previous;
  impl = sl_impl(self);
  if (!impl || !impl->output_stream_active || (!bytes && length > 0)) {
    sl_set_error(self, "live output write requires an open stream and bytes");
    return SL_ERROR_INVALID;
  }
  if (length == 0)
    return SL_OK;
  previous = impl->output_write_active;
  impl->output_write_active = 1;
  result = isatty(impl->input_fd) && isatty(impl->output_fd)
               ? sl_prepare_native_output(self)
               : SL_OK;
  if (result == SL_OK)
    result = sl_output_write_validated(self, bytes, length);
  impl->output_write_active = previous;
  return result;
}

static int sl_quoted_prompt_styles(sl_impl_t *impl, char *prefix_style,
                                   size_t prefix_cap, char *text_style,
                                   size_t text_cap) {
  const sl_theme_palette_t *palette;
  sl_rgb_t prefix_color;
  sl_rgb_t text_color;
  char prefix_rgb[32];
  char text_rgb[32];
  int n;
  if (!isatty(impl->input_fd) || !isatty(impl->output_fd)) {
    prefix_style[0] = '\0';
    text_style[0] = '\0';
    return 0;
  }
  if (!impl->quoted_prompt_style_custom &&
      impl->prompt_theme == SL_PROMPT_THEME_PLAIN) {
    strcpy(prefix_style, "\033[2;90m");
    strcpy(text_style, "\033[3;97m");
    return 0;
  }
  if (!impl->quoted_prompt_style_custom &&
      impl->prompt_theme == SL_PROMPT_THEME_DEFAULT) {
    strcpy(prefix_style, "\033[2;90m");
    strcpy(text_style, "\033[3;96m");
    return 0;
  }
  palette = sl_theme_palette(impl->prompt_theme);
  if (!palette)
    return -1;
  if (impl->quoted_prompt_style_custom) {
    prefix_color.red = impl->quoted_prompt_style.prefix.red;
    prefix_color.green = impl->quoted_prompt_style.prefix.green;
    prefix_color.blue = impl->quoted_prompt_style.prefix.blue;
    text_color.red = impl->quoted_prompt_style.text.red;
    text_color.green = impl->quoted_prompt_style.text.green;
    text_color.blue = impl->quoted_prompt_style.text.blue;
  } else {
    prefix_color = palette->separator;
    text_color = palette->elements[0];
  }
  if (sl_rgb_style(prefix_rgb, sizeof(prefix_rgb), prefix_color, 0) != 0 ||
      sl_rgb_style(text_rgb, sizeof(text_rgb), text_color, 0) != 0)
    return -1;
  n = snprintf(prefix_style, prefix_cap, "\033[2m%s", prefix_rgb);
  if (n < 0 || (size_t)n >= prefix_cap)
    return -1;
  n = snprintf(text_style, text_cap, "\033[3m%s", text_rgb);
  return n >= 0 && (size_t)n < text_cap ? 0 : -1;
}

typedef struct sl_quote_output {
  sl_t *owner;
  char bytes[4096];
  size_t len;
} sl_quote_output_t;

static int sl_quote_output_flush(sl_quote_output_t *output) {
  int result;
  if (output->len == 0)
    return SL_OK;
  result =
      sl_output_stream_write_method(output->owner, output->bytes, output->len);
  if (result == SL_OK)
    output->len = 0;
  return result;
}

static int sl_quote_output_append(sl_quote_output_t *output, const char *bytes,
                                  size_t len) {
  while (len > 0) {
    size_t available = sizeof(output->bytes) - output->len;
    size_t count = len < available ? len : available;
    int result;
    memcpy(output->bytes + output->len, bytes, count);
    output->len += count;
    bytes += count;
    len -= count;
    if (output->len == sizeof(output->bytes)) {
      result = sl_quote_output_flush(output);
      if (result != SL_OK)
        return result;
    }
  }
  return SL_OK;
}

static int sl_quoted_prompt_begin_row(sl_quote_output_t *output,
                                      const char *prefix,
                                      const char *prefix_style,
                                      const char *text_style) {
  const char *reset;
  int result;
  reset = prefix_style[0] ? "\033[0m" : "";
  result = sl_quote_output_append(output, reset, strlen(reset));
  if (result != SL_OK)
    return result;
  result = sl_quote_output_append(output, prefix_style, strlen(prefix_style));
  if (result != SL_OK)
    return result;
  result = sl_quote_output_append(output, prefix, strlen(prefix));
  if (result != SL_OK)
    return result;
  result = sl_quote_output_append(output, reset, strlen(reset));
  if (result != SL_OK)
    return result;
  return sl_quote_output_append(output, text_style, strlen(text_style));
}

static int sl_quoted_prompt_end_row(sl_quote_output_t *output,
                                    const char *text_style) {
  return sl_quote_output_append(output, text_style[0] ? "\033[0m\n" : "\n",
                                text_style[0] ? 5 : 1);
}

static int sl_quoted_prompt_next_row(sl_quote_output_t *output,
                                     const char *prefix,
                                     const char *prefix_style,
                                     const char *text_style) {
  int result = sl_quoted_prompt_end_row(output, text_style);
  if (result != SL_OK)
    return result;
  return sl_quoted_prompt_begin_row(output, prefix, prefix_style, text_style);
}

static int sl_output_stream_write_quoted_prompt_impl(sl_t *self,
                                                     const char *text) {
  sl_impl_t *impl = sl_impl(self);
  const char *prefix;
  char prefix_style[64];
  char text_style[64];
  sl_quote_output_t output;
  size_t len;
  size_t pos;
  size_t space_run_end;
  size_t space_word_len;
  int space_word_width;
  int preceding_breaks;
  int width;
  int indent;
  int col;
  int result;
  if (!impl || !impl->output_stream_active ||
      !sl_quoted_prompt_text_valid(text)) {
    sl_set_error(
        self, "quoted prompt requires an open stream and printable UTF-8 text");
    return SL_ERROR_INVALID;
  }
  if (impl->output_surface && !sl_surface_complete(impl->output_surface)) {
    sl_set_error(self,
                 "quoted prompt requires complete preceding output bytes");
    return SL_ERROR_INVALID;
  }
  prefix = impl->quoted_prompt_prefix ? impl->quoted_prompt_prefix : "> ";
  indent = sl_text_width(prefix, strlen(prefix));
  width = sl_terminal_width(impl);
  if (indent >= width) {
    sl_set_error(
        self,
        "quoted prompt prefix leaves no room at the current output width");
    return SL_ERROR_INVALID;
  }
  for (pos = 0, len = strlen(text); pos < len;) {
    int cells;
    size_t n;
    n = sl_utf8_cluster_len_width(text, len, pos, &cells);
    if (n == 0)
      break;
    if (text[pos] != '\n' && text[pos] != '\t' && cells > width - indent) {
      sl_set_error(self,
                   "quoted prompt glyph is wider than space after prefix");
      return SL_ERROR_INVALID;
    }
    pos += n;
  }
  if (sl_quoted_prompt_styles(impl, prefix_style, sizeof(prefix_style),
                              text_style, sizeof(text_style)) != 0) {
    sl_set_error(self, "failed to style quoted prompt");
    return SL_ERROR;
  }
  memset(&output, 0, sizeof(output));
  output.owner = self;
  preceding_breaks = impl->output_trailing_newlines;
  if (preceding_breaks < 2) {
    result =
        sl_quote_output_append(&output, "\n\n", (size_t)(2 - preceding_breaks));
    if (result != SL_OK)
      return result;
  }
  result =
      sl_quoted_prompt_begin_row(&output, prefix, prefix_style, text_style);
  if (result != SL_OK)
    return result;
  len = strlen(text);
  col = indent;
  space_run_end = 0;
  space_word_len = 0;
  space_word_width = 0;
  for (pos = 0; pos < len;) {
    size_t n;
    int cells;
    if (text[pos] == '\n') {
      result =
          sl_quoted_prompt_next_row(&output, prefix, prefix_style, text_style);
      if (result != SL_OK)
        return result;
      col = indent;
      pos++;
      continue;
    }
    if (text[pos] == ' ' && col > indent) {
      size_t spaces;
      if (space_run_end <= pos) {
        space_run_end = pos;
        while (space_run_end < len && text[space_run_end] == ' ')
          space_run_end++;
        space_word_len = sl_next_word_len(text, len, space_run_end);
        space_word_width =
            sl_word_width(text, len, space_run_end, space_word_len);
      }
      spaces = space_run_end - pos;
      if (space_word_len > 0 && space_word_width <= width - indent &&
          (spaces > (size_t)width ||
           col + (int)spaces + space_word_width > width)) {
        result = sl_quoted_prompt_next_row(&output, prefix, prefix_style,
                                           text_style);
        if (result != SL_OK)
          return result;
        col = indent;
        pos += spaces;
        continue;
      }
    }
    if (sl_word_byte(text[pos]) && (pos == 0 || !sl_word_byte(text[pos - 1])) &&
        col > indent) {
      size_t word_len = sl_next_word_len(text, len, pos);
      int word_width = sl_word_width(text, len, pos, word_len);
      if (word_width <= width - indent && col + word_width > width) {
        result = sl_quoted_prompt_next_row(&output, prefix, prefix_style,
                                           text_style);
        if (result != SL_OK)
          return result;
        col = indent;
      }
    }
    n = sl_utf8_cluster_len_width(text, len, pos, &cells);
    if (n == 0)
      break;
    if (col >= width || (cells > 0 && col + cells > width)) {
      result =
          sl_quoted_prompt_next_row(&output, prefix, prefix_style, text_style);
      if (result != SL_OK)
        return result;
      col = indent;
    }
    if (text[pos] == '\t') {
      int spaces = 8 - col % 8;
      while (spaces-- > 0) {
        if (col >= width) {
          result = sl_quoted_prompt_next_row(&output, prefix, prefix_style,
                                             text_style);
          if (result != SL_OK)
            return result;
          col = indent;
        }
        result = sl_quote_output_append(&output, " ", 1);
        if (result != SL_OK)
          return result;
        col++;
      }
    } else {
      result = sl_quote_output_append(&output, text + pos, n);
      if (result != SL_OK)
        return result;
      col += cells;
    }
    pos += n;
  }
  result = sl_quoted_prompt_end_row(&output, text_style);
  if (result != SL_OK)
    return result;
  result = sl_quote_output_append(&output, "\n", 1);
  if (result != SL_OK)
    return result;
  return sl_quote_output_flush(&output);
}

static int sl_output_stream_write_quoted_prompt_method(sl_t *self,
                                                       const char *text) {
  sl_impl_t *impl = sl_impl(self);
  int previous, result;
  if (!impl)
    return SL_ERROR_INVALID;
  previous = impl->output_write_active;
  impl->output_write_active = 1;
  result = sl_output_stream_write_quoted_prompt_impl(self, text);
  impl->output_write_active = previous;
  return result;
}

static int sl_output_stream_end_method(sl_t *self) {
  sl_impl_t *impl;
  int prompt_origin;
  impl = sl_impl(self);
  if (!impl || !impl->output_stream_active) {
    sl_set_error(self, "no live output stream is open");
    return SL_ERROR_INVALID;
  }
  if (impl->output_surface && !sl_surface_complete(impl->output_surface)) {
    sl_set_error(self,
                 "live output ends in an incomplete ANSI or UTF-8 sequence");
    return SL_ERROR_INVALID;
  }
  prompt_origin = impl->active_prompt && impl->rendered_rows > 0
                      ? impl->rendered_top_row
                      : -1;
  if (!sl_surface_is_native(impl->output_surface) && impl->active_prompt &&
      impl->rendered_rows > 0 && sl_render_clear_active(self) != 0) {
    sl_set_error(self, "failed to clear prompt when ending live output");
    return SL_ERROR_IO;
  }
  if (!isatty(impl->input_fd) || !isatty(impl->output_fd)) {
    sl_surface_destroy(impl->output_surface);
    impl->output_surface = NULL;
  }
  impl->output_stream_active = 0;
  if (!impl->active_prompt && sl_surface_is_native(impl->output_surface)) {
    if (sl_surface_native_exit_advances(impl->output_surface) &&
        impl->output_trailing_newlines < 2)
      impl->output_trailing_newlines++;
    if (sl_native_session_close(self) != 0)
      return SL_ERROR_IO;
    sl_disable_raw(self);
    return sl_show_cursor(impl) == 0 ? SL_OK : SL_ERROR_IO;
  }
  if (impl->active_prompt && prompt_origin >= 0 &&
      !sl_prompt_uses_absolute_rows(impl) &&
      sl_write_cursor_pos(impl->output_fd, prompt_origin, 0) != 0) {
    sl_set_error(self, "failed to position cursor before restoring prompt");
    (void)sl_show_cursor(impl);
    return SL_ERROR_IO;
  }
  if (impl->active_prompt && sl_render_apply(self, impl->active_prompt) != 0) {
    (void)sl_show_cursor(impl);
    return SL_ERROR_IO;
  }
  if (!impl->active_prompt && !sl_prompt_uses_absolute_rows(impl) &&
      isatty(impl->input_fd) && isatty(impl->output_fd) &&
      sl_write_cursor_pos(impl->output_fd,
                          sl_prompt_top(impl, sl_output_prompt_rows(impl)),
                          0) != 0) {
    sl_set_error(self, "failed to position cursor after live output");
    (void)sl_show_cursor(impl);
    return SL_ERROR_IO;
  }
  if (sl_show_cursor(impl) != 0) {
    sl_set_error(self, "failed to restore cursor after live output");
    return SL_ERROR_IO;
  }
  return SL_OK;
}

static ssize_t sl_read_terminal_byte(sl_impl_t *impl, char *ch,
                                     int timeout_ms) {
  fd_set readfds;
  struct timeval tv;
  int ready;
  if (!impl || !ch)
    return -1;
  if (timeout_ms < 0)
    return read(impl->input_fd, ch, 1);
  FD_ZERO(&readfds);
  FD_SET(impl->input_fd, &readfds);
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  ready = select(impl->input_fd + 1, &readfds, NULL, NULL, &tv);
  if (ready <= 0)
    return ready;
  return read(impl->input_fd, ch, 1);
}

static unsigned int sl_watch_poll_events(short revents) {
  unsigned int events;
  events = 0;
  if (revents & POLLIN)
    events |= SL_WATCH_READ;
  if (revents & POLLOUT)
    events |= SL_WATCH_WRITE;
  if (revents & (POLLERR | POLLNVAL))
    events |= SL_WATCH_ERROR;
  if (revents & POLLHUP)
    events |= SL_WATCH_HANGUP;
  return events;
}

static short sl_watch_requested_poll_events(unsigned int events) {
  short requested;
  requested = 0;
  if (events & SL_WATCH_READ)
    requested |= POLLIN;
  if (events & SL_WATCH_WRITE)
    requested |= POLLOUT;
  return requested;
}

static sl_watch_t *sl_watch_find(sl_impl_t *impl, sl_watch_id_t id) {
  int i;
  if (!impl || id == 0)
    return NULL;
  for (i = 0; i < SL_MAX_WATCHES; i++) {
    if (impl->watches[i].id == id)
      return &impl->watches[i];
  }
  return NULL;
}

static ssize_t sl_pending_input_take(sl_impl_t *impl, char *ch) {
  if (!impl || !ch || impl->pending_input_len == 0)
    return 0;
  *ch = impl->pending_input[0];
  if (impl->pending_input_len > 1)
    memmove(impl->pending_input, impl->pending_input + 1,
            impl->pending_input_len - 1);
  impl->pending_input_len--;
  return 1;
}

/* Wait for input and application-owned descriptors together. Watch callbacks
 * are always dispatched on this readline owner's thread. */
static ssize_t sl_read_input_byte(sl_t *self, char *ch, int timeout_ms) {
  enum { SL_WATCH_DISPATCH_BUDGET = 8, SL_UI_TICK_CADENCE_MS = 100 };
  struct pollfd fds[SL_MAX_WATCHES + 1];
  sl_watch_id_t ids[SL_MAX_WATCHES + 1];
  unsigned int slots[SL_MAX_WATCHES + 1];
  sl_impl_t *impl;
  int count;
  int i;
  int slot;
  int ready;
  int dispatched;
  int poll_timeout_ms;
  int deadline_valid;
  struct timeval deadline_started;
  if (!self || !ch)
    return -1;
  impl = sl_impl(self);
  if (!impl)
    return -1;
  if (sl_pending_input_take(impl, ch) == 1)
    return 1;
  count = 1;
  fds[0].fd = impl->input_fd;
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  ids[0] = 0;
  slots[0] = 0;
  for (i = 0; i < SL_MAX_WATCHES; i++) {
    sl_watch_t *watch;
    slot =
        (int)((impl->watch_dispatch_cursor + (unsigned int)i) % SL_MAX_WATCHES);
    watch = &impl->watches[slot];
    if (watch->id == 0)
      continue;
    fds[count].fd = watch->fd;
    fds[count].events = sl_watch_requested_poll_events(watch->events);
    fds[count].revents = 0;
    ids[count] = watch->id;
    slots[count] = (unsigned int)slot;
    count++;
  }
  poll_timeout_ms = timeout_ms;
  if (poll_timeout_ms < 0 &&
      ((impl->request_queue_dispatch && !impl->len &&
        !impl->prompt_queue.editing.info.id && !impl->history_search_active &&
        !impl->bracketed_paste &&
        sl_prompt_queue_first_queued(&impl->prompt_queue) >= 0) ||
       (!impl->prompt_queue.approval_id &&
        sl_prompt_approval_index(impl, 0) >= 0)))
    poll_timeout_ms = 0;
  if (poll_timeout_ms < 0 &&
      (impl->idle_callback || sl_surface_is_native(impl->output_surface) ||
       (impl->statusline.enabled && impl->statusline.spinner &&
        impl->statusline.busy)))
    poll_timeout_ms = SL_UI_TICK_CADENCE_MS;
  if (count == 1)
    return sl_read_terminal_byte(impl, ch, poll_timeout_ms);
  deadline_valid = timeout_ms > 0 && gettimeofday(&deadline_started, NULL) == 0;
  ready = poll(fds, (nfds_t)count, poll_timeout_ms);
  if (ready <= 0)
    return ready;
  /* Reconcile an input-owned resize before handing ready data to producers.
   * The feed call then uses that cursor ledger without its own round trip. */
  if (impl->active_prompt && impl->rendered_rows > 0 &&
      sl_native_resize_pending(impl) &&
      sl_render_apply(self, impl->active_prompt) != 0) {
    errno = EIO;
    return -1;
  }
  dispatched = 0;
  for (i = 1; i < count && dispatched < SL_WATCH_DISPATCH_BUDGET; i++) {
    sl_watch_t *watch;
    sl_watch_callback_t callback;
    sl_watch_event_t event;
    void *userdata;
    int rc;
    event.events = sl_watch_poll_events(fds[i].revents);
    if (event.events == 0)
      continue;
    watch = sl_watch_find(impl, ids[i]);
    if (!watch)
      continue;
    sl_refresh_scroll_region(self);
    callback = watch->callback;
    userdata = watch->userdata;
    event.id = watch->id;
    event.fd = watch->fd;
    impl->watch_callback_depth++;
    rc = callback(self, &event, userdata);
    impl->watch_callback_depth--;
    dispatched++;
    impl->watch_dispatch_cursor =
        (slots[i] + 1u) % (unsigned int)SL_MAX_WATCHES;
    if (rc != SL_OK) {
      if (impl->error[0] == '\0')
        sl_set_error(self, "external event callback failed");
      errno = EIO;
      return -1;
    }
  }
  /* A callback that completes this prompt leaves all concurrently received
   * input for the next prompt. This must precede the buffered-input check and
   * fresh probe below because either may consume a byte. */
  if (impl->request_submit || impl->request_cancel || impl->request_suspend) {
    errno = 0;
    return 0;
  }
  /* A callback may probe the terminal and preserve input received during the
   * probe. That buffered byte takes precedence over the stale poll snapshot. */
  if (sl_pending_input_take(impl, ch) == 1)
    return 1;
  /* The terminal may also become ready while a watch callback runs. The poll
   * snapshot predates that callback, so refresh terminal readiness before an
   * idle completion can release a queued turn. */
  if (dispatched > 0) {
    ssize_t input_ready;
    input_ready = sl_read_terminal_byte(impl, ch, 0);
    if (input_ready != 0)
      return input_ready;
  }
  if (fds[0].revents != 0)
    return read(impl->input_fd, ch, 1);
  /* A watch may arrive in the middle of an escape/UTF-8/paste sequence. Keep
   * its original bounded continuation wait instead of treating the watch wake
   * itself as a missing terminal byte. */
  if (timeout_ms > 0) {
    int remaining_ms;
    ssize_t input_ready;
    if (deadline_valid) {
      struct timeval now;
      long elapsed_ms;
      if (gettimeofday(&now, NULL) == 0) {
        elapsed_ms = (long)(now.tv_sec - deadline_started.tv_sec) * 1000L +
                     (long)(now.tv_usec - deadline_started.tv_usec) / 1000L;
        if (elapsed_ms >= (long)timeout_ms) {
          errno = 0;
          return 0;
        }
        remaining_ms = timeout_ms - (int)elapsed_ms;
      } else {
        remaining_ms = timeout_ms;
      }
    } else {
      remaining_ms = timeout_ms;
    }
    input_ready = sl_read_terminal_byte(impl, ch, remaining_ms);
    if (input_ready == 0)
      errno = 0;
    return input_ready;
  }
  errno = 0;
  return 0;
}

static ssize_t sl_read_escape_byte(sl_t *self, char *ch) {
  return sl_read_input_byte(self, ch, 100);
}

static ssize_t sl_read_available_byte(sl_t *self, char *ch) {
  return sl_read_input_byte(self, ch, 0);
}

static int sl_pending_input_append(sl_impl_t *impl, const char *text,
                                   size_t len) {
  size_t needed;
  size_t cap;
  char *next;
  if (!impl || !text)
    return -1;
  if (len == 0)
    return 0;
  if (len > (size_t)-1 - impl->pending_input_len)
    return -1;
  needed = impl->pending_input_len + len;
  if (needed > impl->pending_input_cap) {
    cap = impl->pending_input_cap ? impl->pending_input_cap
                                  : SL_PENDING_INPUT_MAX;
    while (cap < needed) {
      if (cap > (size_t)-1 / 2) {
        cap = needed;
        break;
      }
      cap *= 2;
    }
    next = (char *)realloc(impl->pending_input, cap);
    if (!next)
      return -1;
    impl->pending_input = next;
    impl->pending_input_cap = cap;
  }
  memcpy(impl->pending_input + impl->pending_input_len, text, len);
  impl->pending_input_len += len;
  return 0;
}

static int sl_decimal_append_int(int *value, char ch) {
  int digit;
  if (!value || ch < '0' || ch > '9')
    return -1;
  digit = ch - '0';
  if (*value > (INT_MAX - digit) / 10)
    return -1;
  *value = *value * 10 + digit;
  return 0;
}

/* Read a Device Status Report cursor-position reply while preserving any user
 * input that happens to arrive first. A terminal that does not support DSR is
 * simply left on the ordinary readline rendering path. */
static int sl_read_cursor_report(sl_t *self, int request) {
  sl_impl_t *impl;
  char candidate[SL_PENDING_INPUT_MAX];
  size_t candidate_len;
  struct timeval started;
  int row;
  int col;
  int have_row;
  int have_col;
  impl = sl_impl(self);
  if (!impl || impl->cursor_position_probe < 0 || !isatty(impl->input_fd) ||
      !isatty(impl->output_fd))
    return -1;
  if (request && sl_wstr(impl->output_fd, "\033[6n") != 0) {
    impl->cursor_position_probe = -1;
    return -1;
  }
  if (gettimeofday(&started, NULL) != 0) {
    impl->cursor_position_probe = -1;
    return -1;
  }
  candidate_len = 0;
  for (;;) {
    struct timeval now;
    long elapsed_ms;
    int timeout_ms;
    char ch;
    ssize_t n;
    int valid;
    size_t i;
    size_t restart;
    if (gettimeofday(&now, NULL) != 0)
      break;
    elapsed_ms = (long)(now.tv_sec - started.tv_sec) * 1000L +
                 (long)(now.tv_usec - started.tv_usec) / 1000L;
    if (elapsed_ms >= 100L)
      break;
    timeout_ms = 100 - (int)elapsed_ms;
    n = sl_read_terminal_byte(impl, &ch, timeout_ms);
    if (n != 1)
      break;
    if (candidate_len == 0 && ch != '\033') {
      if (sl_pending_input_append(impl, &ch, 1) != 0)
        goto preserve_failed;
      continue;
    }
    if (candidate_len >= sizeof(candidate)) {
      if (sl_pending_input_append(impl, candidate, candidate_len) != 0 ||
          sl_pending_input_append(impl, &ch, 1) != 0)
        goto preserve_failed;
      candidate_len = 0;
      continue;
    }
    candidate[candidate_len++] = ch;
    valid = 1;
    row = 0;
    col = 0;
    have_row = 0;
    have_col = 0;
    if (candidate[0] != '\033')
      valid = 0;
    if (valid && candidate_len > 1 && candidate[1] != '[')
      valid = 0;
    i = 2;
    while (valid && i < candidate_len && candidate[i] >= '0' &&
           candidate[i] <= '9') {
      if (sl_decimal_append_int(&row, candidate[i]) != 0) {
        valid = 0;
        break;
      }
      have_row = 1;
      i++;
    }
    if (valid && i < candidate_len && candidate[i] != ';')
      valid = 0;
    if (valid && i < candidate_len) {
      i++;
      while (i < candidate_len && candidate[i] >= '0' && candidate[i] <= '9') {
        if (sl_decimal_append_int(&col, candidate[i]) != 0) {
          valid = 0;
          break;
        }
        have_col = 1;
        i++;
      }
      if (i < candidate_len && candidate[i] != 'R')
        valid = 0;
      else if (i < candidate_len)
        i++;
    }
    if (!valid || i < candidate_len) {
      restart = candidate_len;
      for (i = candidate_len; i > 1; i--) {
        if (candidate[i - 1] == '\033') {
          restart = i - 1;
          break;
        }
      }
      if (sl_pending_input_append(impl, candidate, restart) != 0)
        goto preserve_failed;
      if (restart < candidate_len) {
        memmove(candidate, candidate + restart, candidate_len - restart);
        candidate_len -= restart;
      } else {
        candidate_len = 0;
      }
      continue;
    }
    if (have_row && have_col && i == candidate_len &&
        candidate[candidate_len - 1] == 'R') {
      if (row > 0 && col > 0) {
        impl->cursor_position_probe = 1;
        impl->probed_cursor_col = col - 1;
        return row;
      }
      break;
    }
  }
  if (sl_pending_input_append(impl, candidate, candidate_len) != 0)
    goto preserve_failed;
  impl->cursor_position_probe = -1;
  return -1;

preserve_failed:
  sl_set_error(self, "failed to preserve input during cursor probe");
  impl->cursor_position_probe = -1;
  return -1;
}

/* A returning signal handler leaves the session's input restored. Cursor
 * reports must still be read without echo or canonical line buffering. */
static int sl_cursor_report_begin(sl_t *self, struct termios *original) {
  sl_impl_t *impl = sl_impl(self);
  struct termios query;
  if (impl->raw_active)
    return 0;
  if (tcgetattr(impl->input_fd, original) != 0) {
    sl_set_error(self, "failed to read input attributes for cursor report");
    return -1;
  }
  query = *original;
  query.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  query.c_cflag |= CS8;
  query.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  query.c_cc[VMIN] = 0;
  query.c_cc[VTIME] = 1;
  /* Preserve queued user input while changing the report-reading mode. */
  if (tcsetattr(impl->input_fd, TCSANOW, &query) != 0) {
    sl_set_error(self, "failed to acquire input for cursor report");
    return -1;
  }
  return 1;
}

static int sl_cursor_report_end(sl_t *self, const struct termios *original,
                                int temporary) {
  sl_impl_t *impl = sl_impl(self);
  if (temporary && tcsetattr(impl->input_fd, TCSANOW, original) != 0) {
    sl_set_error(self, "failed to restore input after cursor report");
    return -1;
  }
  return 0;
}

static int sl_query_cursor_row(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  struct termios original;
  int temporary, result;
  if (!impl || impl->output_write_active || impl->cursor_position_probe < 0 ||
      !isatty(impl->input_fd) || !isatty(impl->output_fd))
    return -1;
  temporary = sl_cursor_report_begin(self, &original);
  if (temporary < 0)
    return -1;
  result = sl_read_cursor_report(self, 1);
  if (sl_cursor_report_end(self, &original, temporary) != 0)
    return -1;
  return result;
}

static int sl_try_pin_scroll_region(sl_t *self) {
  sl_impl_t *impl;
  int cursor_row;
  int prompt_top;
  int prompt_bottom;
  impl = sl_impl(self);
  if (!impl || !impl->live_scroll_region || impl->auto_scroll_pinned ||
      !impl->active_prompt || impl->rendered_rows < 1)
    return 0;
  cursor_row = sl_query_cursor_row(self);
  if (cursor_row < 1)
    return 0;
  prompt_top = cursor_row - 1 - impl->rendered_cursor_row;
  prompt_bottom = prompt_top + impl->rendered_rows - 1;
  if (prompt_top < 0 || prompt_bottom < sl_terminal_height(impl) - 1)
    return 0;
  impl->auto_scroll_pinned = 1;
  impl->rendered_top_row = prompt_top;
  impl->rendered_height = sl_terminal_rows(impl);
  return 1;
}

/* Late ordinary-readline ownership is acquired between producer callbacks,
 * never within a feed call. Multiple writes in one callback stay immediate. */
static void sl_refresh_scroll_region(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl || impl->output_write_active || !impl->scroll_region_probe_pending)
    return;
  impl->scroll_region_probe_pending = 0;
  if (impl->live_scroll_region && !sl_prompt_uses_absolute_rows(impl))
    (void)sl_try_pin_scroll_region(self);
}

static int sl_read_utf8_input(sl_t *self, char first, char *buf, size_t *len) {
  unsigned char c;
  size_t need;
  size_t i;
  if (!buf || !len)
    return -1;
  buf[0] = first;
  *len = 1;
  c = (unsigned char)first;
  if ((c & 0x80) == 0)
    return 0;
  if ((c & 0xe0) == 0xc0)
    need = 2;
  else if ((c & 0xf0) == 0xe0)
    need = 3;
  else if ((c & 0xf8) == 0xf0)
    need = 4;
  else
    return 0;
  for (i = 1; i < need; i++) {
    char next;
    ssize_t n;
    n = sl_read_escape_byte(self, &next);
    if (n != 1)
      return 0;
    if (((unsigned char)next & 0xc0) != 0x80)
      return 0;
    buf[i] = next;
    *len = i + 1;
  }
  return 0;
}

static int sl_read_key(sl_t *self) {
  sl_impl_t *impl;
  char ch;
  ssize_t n;
  impl = sl_impl(self);
  if (!impl)
    return SL_KEY_NONE;
  errno = 0;
  n = sl_read_input_byte(self, &ch, -1);
  if (n <= 0)
    return SL_KEY_NONE;
  if (ch != '\033')
    return (int)(unsigned char)ch;
  n = sl_read_escape_byte(self, &ch);
  if (n <= 0)
    return SL_KEY_ESCAPE;
  if (ch == 'b')
    return SL_KEY_ALT_B;
  if (ch == 'f')
    return SL_KEY_ALT_F;
  if (ch == '\r')
    return SL_KEY_ALT_ENTER;
  if (ch == 127 || ch == 8)
    return SL_KEY_ALT_BACKSPACE;
  if (ch != '[' && ch != 'O') {
    if (ch >= 'a' && ch <= 'z')
      return SL_KEY_ALT_BASE + (unsigned char)ch;
    if (ch >= 'A' && ch <= 'Z')
      return SL_KEY_ALT_BASE + (unsigned char)ch;
    return SL_KEY_UNKNOWN;
  }
  n = sl_read_escape_byte(self, &ch);
  if (n <= 0)
    return SL_KEY_UNKNOWN;
  switch (ch) {
  case 'A':
    return SL_KEY_UP;
  case 'B':
    return SL_KEY_DOWN;
  case 'C':
    return SL_KEY_RIGHT;
  case 'D':
    return SL_KEY_LEFT;
  case 'H':
    return SL_KEY_HOME;
  case 'F':
    return SL_KEY_END;
  case 'P':
    return SL_KEY_F1;
  case 'Q':
    return SL_KEY_F2;
  case 'R':
    return SL_KEY_F3;
  case 'S':
    return SL_KEY_F4;
  }
  if (ch >= '0' && ch <= '9') {
    int code;
    int modifier;
    int overflow;
    code = ch - '0';
    modifier = 0;
    overflow = 0;
    while (sl_read_escape_byte(self, &ch) == 1) {
      if (ch == '~')
        break;
      if (ch == ';') {
        while (sl_read_escape_byte(self, &ch) == 1) {
          if (ch < '0' || ch > '9')
            break;
          if (!overflow && sl_decimal_append_int(&modifier, ch) != 0)
            overflow = 1;
        }
        break;
      }
      if (ch < '0' || ch > '9')
        break;
      if (!overflow && sl_decimal_append_int(&code, ch) != 0)
        overflow = 1;
    }
    if (overflow)
      return SL_KEY_UNKNOWN;
    if (ch == 'u') {
      if (code == 13 && modifier == 5)
        return SL_KEY_CTRL_ENTER;
      return SL_KEY_UNKNOWN;
    }
    if (ch == '~') {
      if (code == 3)
        return SL_KEY_DELETE;
      if (code == 5)
        return SL_KEY_PAGE_UP;
      if (code == 6)
        return SL_KEY_PAGE_DOWN;
      if (code == 11)
        return SL_KEY_F1;
      if (code == 12)
        return SL_KEY_F2;
      if (code == 13)
        return SL_KEY_F3;
      if (code == 14)
        return SL_KEY_F4;
      if (code == 15)
        return SL_KEY_F5;
      if (code == 17)
        return SL_KEY_F6;
      if (code == 18)
        return SL_KEY_F7;
      if (code == 19)
        return SL_KEY_F8;
      if (code == 20)
        return SL_KEY_F9;
      if (code == 21)
        return SL_KEY_F10;
      if (code == 1 || code == 7)
        return SL_KEY_HOME;
      if (code == 4 || code == 8)
        return SL_KEY_END;
      if (code == 200)
        return SL_KEY_PASTE_BEGIN;
      if (code == 201)
        return SL_KEY_PASTE_END;
    }
  }
  return SL_KEY_UNKNOWN;
}

static int sl_read_paste_input(sl_t *self, char *buf, size_t *len) {
  sl_impl_t *impl;
  static const char paste_end[] = "\033[201~";
  char ch;
  ssize_t n;
  size_t i;
  impl = sl_impl(self);
  if (!impl || !buf || !len)
    return SL_KEY_NONE;
  *len = 0;
  errno = 0;
  n = sl_read_input_byte(self, &ch, -1);
  if (n <= 0)
    return SL_KEY_NONE;
  if (ch == '\r')
    ch = '\n';
  buf[0] = ch;
  *len = 1;
  if (ch != '\033') {
    if (((unsigned char)ch & 0x80) != 0)
      (void)sl_read_utf8_input(self, ch, buf, len);
    return SL_KEY_UNKNOWN;
  }
  for (i = 1; i < sizeof(paste_end) - 1; i++) {
    n = sl_read_escape_byte(self, &ch);
    if (n != 1)
      return SL_KEY_UNKNOWN;
    buf[i] = ch;
    *len = i + 1;
    if (ch != paste_end[i])
      return SL_KEY_UNKNOWN;
  }
  *len = 0;
  return SL_KEY_PASTE_END;
}

static char *sl_readline_plain(sl_t *self, const char *prompt) {
  sl_impl_t *impl;
  char *result;
  char ch;
  int got;
  (void)prompt;
  impl = sl_impl(self);
  if (!impl)
    return NULL;
  sl_set_readline_status(self, SL_READLINE_NONE);
  if (sl_buf_set(self, "") != 0) {
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  got = 0;
  for (;;) {
    ssize_t nread;
    if (impl->plain_pending) {
      ch = impl->plain_pending_ch;
      impl->plain_pending = 0;
      nread = 1;
    } else {
      nread = read(impl->input_fd, &ch, 1);
    }
    if (nread == 0)
      break;
    if (nread < 0) {
      if (errno == EINTR)
        continue;
      sl_set_error(self, "failed to read input");
      sl_set_readline_status(self, SL_READLINE_ERROR);
      return NULL;
    }
    got = 1;
    if (ch == '\r') {
      char next;
      nread = sl_read_available_byte(self, &next);
      if (nread == 1 && next != '\n') {
        impl->plain_pending = 1;
        impl->plain_pending_ch = next;
      }
      break;
    }
    if (ch == '\n')
      break;
    if (ch == 4 && impl->len == 0) {
      sl_set_readline_status(self, SL_READLINE_EOF);
      return NULL;
    }
    if (sl_buf_insert(self, impl->len, &ch, 1) != 0 &&
        impl->len < impl->line_max_len) {
      sl_set_readline_status(self, SL_READLINE_ERROR);
      return NULL;
    }
  }
  if (!got && impl->len == 0) {
    sl_set_readline_status(self, SL_READLINE_EOF);
    return NULL;
  }
  result = sl_strdup(impl->buf);
  if (!result) {
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  if (impl->history_auto_add && sl_history_add_impl(self, result) != SL_OK) {
    free(result);
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  sl_set_readline_status(self, SL_READLINE_SUBMITTED);
  return result;
}

static void sl_history_nav_start(sl_t *self) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl)
    return;
  if (impl->history_index == -1) {
    free(impl->history_edit);
    impl->history_edit = sl_strdup(impl->buf);
    impl->history_index = impl->history.len;
  }
}

static void sl_history_nav_reset(sl_impl_t *impl) {
  if (!impl)
    return;
  free(impl->history_edit);
  impl->history_edit = NULL;
  impl->history_index = -1;
}

static void sl_history_nav(sl_t *self, int dir) {
  sl_impl_t *impl;
  int next;
  impl = sl_impl(self);
  if (!impl || impl->history.len == 0)
    return;
  sl_history_nav_start(self);
  next = impl->history_index + dir;
  if (next < 0)
    next = 0;
  if (next > impl->history.len)
    next = impl->history.len;
  impl->history_index = next;
  if (next == impl->history.len)
    (void)sl_buf_set(self, impl->history_edit ? impl->history_edit : "");
  else
    (void)sl_buf_set(self, impl->history.items[next]);
}

static void sl_history_search_cleanup(sl_t *self, sl_history_search_t *search) {
  sl_impl_t *impl = sl_impl(self);
  if (impl)
    impl->history_search_active = 0;
  if (!search)
    return;
  free(search->saved);
  free(search->query);
  free(search->prompt);
  memset(search, 0, sizeof(*search));
  search->match_index = -1;
}

static int sl_history_search_reserve(sl_history_search_t *search, size_t need) {
  size_t cap;
  char *next;
  if (need <= search->query_cap)
    return 0;
  cap = search->query_cap ? search->query_cap : 32;
  while (cap < need) {
    if (cap > ((size_t)-1) / 2)
      return -1;
    cap *= 2;
  }
  next = (char *)realloc(search->query, cap);
  if (!next)
    return -1;
  search->query = next;
  search->query_cap = cap;
  return 0;
}

static int sl_history_search_append(sl_history_search_t *search,
                                    const char *bytes, size_t len) {
  if (!search || !bytes)
    return -1;
  if (sl_history_search_reserve(search, search->query_len + len + 1) != 0)
    return -1;
  memcpy(search->query + search->query_len, bytes, len);
  search->query_len += len;
  search->query[search->query_len] = '\0';
  return 0;
}

static void sl_history_search_backspace(sl_history_search_t *search) {
  size_t prev_len;
  if (!search || search->query_len == 0)
    return;
  prev_len = sl_utf8_prev_cluster_len(search->query, search->query_len);
  search->query_len -= prev_len;
  search->query[search->query_len] = '\0';
}

static int sl_history_search_item_matches(const char *item, const char *query) {
  if (!item || !query)
    return 0;
  if (query[0] == '\0')
    return 1;
  return strstr(item, query) != NULL;
}

static int sl_history_search_find_from(sl_history_t *history, const char *query,
                                       int start, int stop) {
  int i;
  if (!history || history->len <= 0)
    return -1;
  if (start >= history->len)
    start = history->len - 1;
  if (stop < 0)
    stop = 0;
  for (i = start; i >= stop; i--) {
    if (sl_history_search_item_matches(history->items[i], query))
      return i;
  }
  return -1;
}

static int sl_history_search_find(sl_impl_t *impl, sl_history_search_t *search,
                                  int repeat) {
  int found;
  if (!impl || !search)
    return -1;
  if (impl->history.len <= 0)
    return -1;
  if (repeat && search->has_match && search->match_index >= 0) {
    found = sl_history_search_find_from(&impl->history, search->query,
                                        search->match_index - 1, 0);
    if (found >= 0)
      return found;
    found = sl_history_search_find_from(&impl->history, search->query,
                                        impl->history.len - 1,
                                        search->match_index + 1);
    if (found >= 0)
      return found;
    if (sl_history_search_item_matches(impl->history.items[search->match_index],
                                       search->query))
      return search->match_index;
    return -1;
  }
  return sl_history_search_find_from(&impl->history, search->query,
                                     impl->history.len - 1, 0);
}

static int sl_history_search_update_prompt(sl_history_search_t *search) {
  const char *prefix;
  const char *suffix;
  size_t len;
  char *prompt;
  if (!search)
    return -1;
  prefix = search->has_match ? "(r-search)`" : "(failed)`";
  suffix = "': ";
  len = strlen(prefix) + search->query_len + strlen(suffix) + 1;
  prompt = (char *)malloc(len);
  if (!prompt)
    return -1;
  memcpy(prompt, prefix, strlen(prefix));
  memcpy(prompt + strlen(prefix), search->query, search->query_len);
  memcpy(prompt + strlen(prefix) + search->query_len, suffix,
         strlen(suffix) + 1);
  free(search->prompt);
  search->prompt = prompt;
  return 0;
}

static int sl_history_search_refresh(sl_t *self, sl_history_search_t *search,
                                     int repeat) {
  sl_impl_t *impl;
  int found;
  impl = sl_impl(self);
  if (!impl || !search || !impl->history_search_active)
    return -1;
  found = sl_history_search_find(impl, search, repeat);
  if (found >= 0) {
    search->has_match = 1;
    search->match_index = found;
    if (sl_buf_set(self, impl->history.items[found]) != 0)
      return -1;
  } else {
    search->has_match = 0;
    search->match_index = -1;
    if (sl_buf_set(self, search->saved ? search->saved : "") != 0)
      return -1;
  }
  return sl_history_search_update_prompt(search);
}

static int sl_history_search_start(sl_t *self, sl_history_search_t *search) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || !search)
    return -1;
  if (!impl->history_search_active) {
    sl_history_search_cleanup(self, search);
    search->saved = sl_strdup(impl->buf);
    if (!search->saved)
      return -1;
    if (sl_history_search_reserve(search, 1) != 0)
      return -1;
    search->query[0] = '\0';
    search->query_len = 0;
    search->match_index = -1;
    impl->history_search_active = 1;
  }
  return sl_history_search_refresh(self, search, search->has_match);
}

static int sl_history_search_cancel(sl_t *self, sl_history_search_t *search) {
  int rc;
  rc = 0;
  if (search && sl_history_search_active_impl(self))
    rc = sl_buf_set(self, search->saved ? search->saved : "");
  sl_history_search_cleanup(self, search);
  return rc;
}

static int sl_history_search_accept(sl_t *self, sl_history_search_t *search) {
  sl_impl_t *impl = sl_impl(self);
  if (impl && impl->history_search_active)
    sl_edit_clear_undo(&impl->edit);
  sl_history_search_cleanup(self, search);
  return 0;
}

static int sl_dispatch_key_binding(sl_t *self, int key,
                                   sl_key_action_t *action) {
  sl_impl_t *impl;
  sl_key_binding_t *binding;
  impl = sl_impl(self);
  if (!impl || !action)
    return SL_ERROR_INVALID;
  binding = sl_find_key_binding(impl, (sl_key_t)key);
  if (!binding)
    return SL_OK;
  *action = SL_KEY_ACTION_HANDLED;
  return binding->callback(self, (sl_key_t)key, binding->userdata, action);
}

static int sl_prompt_queue_take_method(sl_t *self, size_t index, char **out);
static int sl_prompt_queue_enqueue_draft_method(sl_t *self);

/* Called only for deliberate submission. Passive enqueue never classifies or
 * clears a cancellation stop. Requeueing retains the reserved identity/slot. */
static int sl_prompt_queue_admit(sl_t *self, sl_prompt_queue_mode_t mode,
                                 int *admitted) {
  sl_impl_t *impl = sl_impl(self);
  sl_prompt_queue_t *queue = &impl->prompt_queue;
  sl_prompt_queue_policy_t policy = {0, 0, 1};
  int rc, editing = queue->editing.info.id != 0;
  int promote = mode == SL_PROMPT_QUEUE_MODE_STEER;
  int index = editing ? queue->editing_index : queue->len;
  *admitted = 0;
  if (!queue->enabled || !impl->len)
    return SL_OK;
  if (editing) {
    policy = queue->editing.info.policy;
    if (mode == SL_PROMPT_QUEUE_MODE_QUEUED)
      mode = queue->editing.info.mode;
  }
  if (queue->policy_callback) {
    rc = queue->policy_callback(self, impl->buf, mode, &policy,
                                queue->policy_userdata);
    if (rc != SL_OK) {
      sl_set_error(self, "prompt queue policy rejected submission");
      return rc;
    }
  }
  if ((policy.enqueue != 0 && policy.enqueue != 1) ||
      (policy.independent != 0 && policy.independent != 1) ||
      (policy.executable != 0 && policy.executable != 1) ||
      (!policy.enqueue && policy.independent)) {
    sl_set_error(self, "prompt queue policy fields must be valid booleans");
    return SL_ERROR_INVALID;
  }
  /* An edited managed entry cannot escape its queue or cancellation state. */
  if (editing && queue->editing.info.policy.enqueue)
    policy.enqueue = 1;
  if (!policy.enqueue &&
      !(editing &&
        (queue->policy_callback || !queue->editing.info.policy.executable ||
         sl_prompt_queue_blocked_before(queue, &queue->editing.info, index))))
    return SL_OK;
  rc = sl_prompt_queue_enqueue_draft_method(self);
  if (rc != SL_OK)
    return rc;
  /* The committed entry occupies its original reserved position after edit. */
  queue->items[index].info.policy = policy;
  queue->items[index].info.mode = mode;
  if (!editing || promote)
    queue->items[index].info.stopped = 0;
  sl_prompt_queue_schedule(impl);
  *admitted = 1;
  return SL_OK;
}

static void sl_prompt_queue_bell(sl_impl_t *impl) {
  if (impl)
    (void)write(impl->output_fd, "\a", 1);
}

int sl_queue_cached_edit_begin(sl_t *self, int index) {
  sl_impl_t *impl = sl_impl(self);
  if (sl_prompt_queue_require_fifo(
          self, impl, &impl->prompt_queue.items[index].info) != SL_OK) {
    sl_prompt_queue_bell(impl);
    return SL_OK;
  }
  if (sl_buf_set(self, impl->prompt_queue.items[index].text) != 0) {
    sl_set_error(self, "failed to restore queued draft into editor");
    return SL_ERROR_NOMEM;
  }
  impl->prompt_queue.editing = impl->prompt_queue.items[index];
  impl->prompt_queue.editing_index = index;
  (void)sl_prompt_queue_take_raw(&impl->prompt_queue, index);
  impl->prompt_queue.approval_id = 0;
  sl_history_nav_reset(impl);
  return SL_OK;
}

static int sl_prompt_queue_edit_at(sl_t *self, int index) {
  return sl_impl(self)->backend ? sl_backend_edit(self, index)
                                : sl_queue_cached_edit_begin(self, index);
}

static int sl_handle_prompt_queue_key(sl_t *self, int key, int *handled,
                                      int *submit, char **promoted) {
  sl_impl_t *impl;
  sl_prompt_queue_keys_t *keys;
  int rc;
  if (handled)
    *handled = 0;
  if (submit)
    *submit = 0;
  if (!self || !handled || !submit || !promoted)
    return SL_ERROR_INVALID;
  impl = sl_impl(self);
  if (!sl_prompt_queue_enabled(impl))
    return SL_OK;
  keys = &impl->prompt_queue.keys;
  if (impl->backend) {
    if (key == SL_KEY_ENTER ||
        (keys->enqueue_draft != SL_KEY_NONE &&
         key == (int)keys->enqueue_draft) ||
        (keys->submit_or_promote_newest != SL_KEY_NONE &&
         key == (int)keys->submit_or_promote_newest)) {
      *handled = 1;
      if (!impl->len && !impl->prompt_queue.editing.info.id &&
          key == (int)keys->submit_or_promote_newest)
        rc = sl_backend_promote_newest(self);
      else
        rc = sl_backend_submit(self, key == (int)keys->submit_or_promote_newest
                                         ? SL_PROMPT_QUEUE_MODE_STEER
                                         : SL_PROMPT_QUEUE_MODE_QUEUED);
      if (rc != SL_OK)
        sl_prompt_queue_bell(impl);
    } else if (keys->edit_newest != SL_KEY_NONE &&
               key == (int)keys->edit_newest) {
      *handled = 1;
      if (sl_backend_edit(self, impl->prompt_queue.len - 1) != SL_OK)
        sl_prompt_queue_bell(impl);
    }
    return SL_OK;
  }
  /* Empty edits still own a queue entry. Route every keyboard submission
   * or requeue through the same deletion gate as sl_submit().
   */
  if (impl->len == 0 && impl->prompt_queue.editing.info.id &&
      (key == SL_KEY_ENTER ||
       (keys->enqueue_draft != SL_KEY_NONE &&
        (sl_key_t)key == keys->enqueue_draft) ||
       (keys->submit_or_promote_newest != SL_KEY_NONE &&
        (sl_key_t)key == keys->submit_or_promote_newest))) {
    *handled = 1;
    *submit = 1;
    return SL_OK;
  }
  if (impl->len && (key == SL_KEY_ENTER ||
                    (keys->submit_or_promote_newest != SL_KEY_NONE &&
                     (sl_key_t)key == keys->submit_or_promote_newest))) {
    int admitted = 0;
    rc = sl_prompt_queue_admit(self,
                               (sl_key_t)key == keys->submit_or_promote_newest
                                   ? SL_PROMPT_QUEUE_MODE_STEER
                                   : SL_PROMPT_QUEUE_MODE_QUEUED,
                               &admitted);
    if (admitted || rc != SL_OK) {
      *handled = 1;
      if (rc != SL_OK)
        sl_prompt_queue_bell(impl);
      return SL_OK;
    }
  }
  if (keys->enqueue_draft != SL_KEY_NONE &&
      (sl_key_t)key == keys->enqueue_draft) {
    if (impl->prompt_queue.profile == SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS &&
        !impl->statusline.busy) {
      if (impl->len == 0) {
        *handled = 1;
        sl_prompt_queue_bell(impl);
      }
      return SL_OK;
    }
    *handled = 1;
    if (impl->len == 0) {
      sl_prompt_queue_bell(impl);
      return SL_OK;
    }
    rc = sl_prompt_queue_enqueue_draft_method(self);
    if (rc == SL_ERROR_FULL) {
      sl_prompt_queue_bell(impl);
      return SL_OK;
    }
    return rc;
  }
  if (keys->edit_newest != SL_KEY_NONE && (sl_key_t)key == keys->edit_newest) {
    *handled = 1;
    if (impl->len != 0 || impl->prompt_queue.editing.info.id) {
      sl_set_error(self,
                   "active draft is not empty; queued draft was not replaced");
      sl_prompt_queue_bell(impl);
      return SL_OK;
    }
    if (impl->prompt_queue.len == 0) {
      sl_prompt_queue_bell(impl);
      return SL_OK;
    }
    return sl_prompt_queue_edit_at(self, impl->prompt_queue.len - 1);
  }
  if (keys->submit_or_promote_newest != SL_KEY_NONE &&
      (sl_key_t)key == keys->submit_or_promote_newest) {
    *handled = 1;
    if (impl->len == 0 && impl->prompt_queue.len &&
        impl->prompt_queue.items[impl->prompt_queue.len - 1]
            .info.policy.enqueue) {
      sl_prompt_queue_entry_info_t *info =
          &impl->prompt_queue.items[impl->prompt_queue.len - 1].info;
      info->mode = SL_PROMPT_QUEUE_MODE_STEER;
      info->stopped = 0;
      sl_prompt_queue_schedule(impl);
      return SL_OK;
    }
    if (impl->prompt_queue.profile == SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS &&
        impl->statusline.busy) {
      if (impl->len != 0) {
        rc = sl_prompt_queue_append_raw(impl, impl->buf,
                                        SL_PROMPT_QUEUE_MODE_STEER);
        if (rc == 1) {
          sl_prompt_queue_bell(impl);
          return SL_OK;
        }
        if (rc != 0) {
          sl_set_error(self, "failed to queue steer draft");
          return SL_ERROR_NOMEM;
        }
        impl->prompt_queue.items[impl->prompt_queue.len - 1].info.stopped = 0;
        if (impl->history_auto_add) {
          rc = sl_history_add_impl(self, impl->buf);
          if (rc != SL_OK) {
            char *discard = sl_prompt_queue_take_raw(
                &impl->prompt_queue, impl->prompt_queue.len - 1);
            free(discard);
            return rc;
          }
          impl->prompt_queue.items[impl->prompt_queue.len - 1]
              .history_recorded = 1;
        }
        if (sl_buf_set(self, "") != 0) {
          char *discard = sl_prompt_queue_take_raw(&impl->prompt_queue,
                                                   impl->prompt_queue.len - 1);
          free(discard);
          sl_set_error(self, "failed to clear steered draft");
          return SL_ERROR_NOMEM;
        }
        sl_prompt_queue_finish_requeue(&impl->prompt_queue);
        sl_history_nav_reset(impl);
        return SL_OK;
      }
      if (impl->prompt_queue.len == 0) {
        sl_prompt_queue_bell(impl);
        return SL_OK;
      }
      impl->prompt_queue.items[impl->prompt_queue.len - 1].info.mode =
          SL_PROMPT_QUEUE_MODE_STEER;
      impl->prompt_queue.items[impl->prompt_queue.len - 1].info.stopped = 0;
      sl_prompt_queue_schedule(impl);
      return SL_OK;
    }
    if (impl->len != 0) {
      *submit = 1;
      return SL_OK;
    }
    if (impl->prompt_queue.len == 0) {
      sl_prompt_queue_bell(impl);
      return SL_OK;
    }
    if (impl->prompt_queue.items[impl->prompt_queue.len - 1].info.held) {
      sl_set_error(self, "queued prompt requires explicit approval");
      sl_prompt_queue_bell(impl);
      return SL_OK;
    }
    /* Promotion is explicit, but never waives execution readiness. A managed
     * entry resumes only itself; legacy promotion keeps new-turn behavior. */
    if (!impl->prompt_queue.items[impl->prompt_queue.len - 1]
             .info.policy.executable) {
      sl_prompt_queue_bell(impl);
      return SL_OK;
    }
    {
      int index = impl->prompt_queue.len - 1;
      int stopped = impl->prompt_queue.items[index].info.stopped;
      impl->prompt_queue.items[index].info.stopped = 0;
      if (!sl_prompt_queue_eligible(&impl->prompt_queue, index)) {
        impl->prompt_queue.items[index].info.stopped = stopped;
        sl_prompt_queue_bell(impl);
        return SL_OK;
      }
    }
    return sl_prompt_queue_take_method(
        self, (size_t)(impl->prompt_queue.len - 1), promoted);
  }
  return SL_OK;
}

static int sl_kill_draft(sl_t *self, size_t start, size_t end, int backward) {
  sl_impl_t *impl = sl_impl(self);
  char *text;
  size_t len;
  if (start == end) {
    impl->edit.command = SL_EDIT_OTHER;
    return 0;
  }
  text = sl_edit_prepare_kill(&impl->edit, impl->buf + start, end - start,
                              backward, &len);
  if (!text || sl_buf_delete(self, start, end - start) != 0) {
    free(text);
    impl->edit.command = SL_EDIT_OTHER;
    sl_set_error(self, "failed to retain killed draft text");
    return -1;
  }
  sl_edit_commit_kill(&impl->edit, text, len);
  impl->cursor = start;
  return 0;
}

static int sl_clear_draft(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  if (impl->prompt_queue.editing.info.id) {
    sl_edit_t recovery;
    int status;
    /* Prepare the text-only undo before retiring the queued context. Never
     * carry its earlier edits or identity into the new ordinary draft. */
    memset(&recovery, 0, sizeof(recovery));
    recovery.enabled = 1;
    if (sl_edit_record(&recovery, 0, impl->buf, impl->len, 0, impl->cursor) !=
        0) {
      sl_set_error(self, "failed to retain discarded draft text for undo");
      return -1;
    }
    status = sl_prompt_queue_reject(self, impl->prompt_queue.editing.info.id);
    if (status != SL_OK) {
      sl_edit_clear_undo(&recovery);
      return -1;
    }
    impl->edit.undo = recovery.undo;
    impl->edit.group = recovery.group;
    return 0;
  }
  if (sl_buf_delete(self, 0, impl->len) != 0)
    return -1;
  impl->cursor = 0;
  return 0;
}

static int sl_stash_draft(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  char *saved;
  if (!impl->len)
    return 0;
  saved = sl_strdup(impl->buf);
  if (!saved || sl_buf_delete(self, 0, impl->len) != 0) {
    free(saved);
    sl_set_error(self, "failed to stash draft text");
    return -1;
  }
  free(impl->edit.stash);
  impl->edit.stash = saved;
  impl->cursor = 0;
  return 0;
}

static int sl_append_stash(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl->edit.stash)
    return 0;
  if (sl_buf_insert(self, impl->len, impl->edit.stash,
                    strlen(impl->edit.stash)) != 0)
    return -1;
  impl->cursor = impl->len;
  return 0;
}

static int sl_yank_draft(sl_t *self, int rotate) {
  sl_impl_t *impl = sl_impl(self);
  sl_edit_t *edit = &impl->edit;
  int index = 0;
  size_t at = impl->cursor, removed = 0;
  if (!edit->kill_count)
    return -1;
  if (rotate) {
    if (!edit->yank_valid || (edit->last_command != SL_EDIT_YANK &&
                              edit->last_command != SL_EDIT_YANK_POP))
      return -1;
    index = (edit->yank_index + 1) % edit->kill_count;
    at = edit->yank_at;
    removed = edit->yank_len;
  }
  if (sl_buf_replace(self, at, removed, edit->kills[index].text,
                     edit->kills[index].len) != 0)
    return -1;
  impl->cursor = at + edit->kills[index].len;
  edit->yank_valid = 1;
  edit->yank_at = at;
  edit->yank_len = edit->kills[index].len;
  edit->yank_index = index;
  return 0;
}

static int sl_transpose_draft(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  size_t end, right, left;
  char *text;
  int status;
  if (!impl->cursor)
    return 0;
  end = impl->cursor;
  if (end < impl->len && impl->buf[end] != '\n')
    end += sl_utf8_next_cluster_len(impl->buf, impl->len, end);
  right = end - sl_utf8_prev_cluster_len(impl->buf, end);
  if (!right)
    return 0;
  left = right - sl_utf8_prev_cluster_len(impl->buf, right);
  text = malloc(end - left);
  if (!text)
    return -1;
  memcpy(text, impl->buf + right, end - right);
  memcpy(text + end - right, impl->buf + left, right - left);
  status = sl_buf_replace(self, left, end - left, text, end - left);
  free(text);
  if (!status)
    impl->cursor = end;
  return status;
}

static char *sl_readline_impl(sl_t *self, const char *prompt,
                              int queue_dispatch, sl_prompt_source_t *source) {
  sl_impl_t *impl;
  sl_history_search_t search;
  char *result;
  int done;
  int eof;
  int cancelled;
  int interrupted;
  int suspended;
  int failed;
  char *promoted;
  char *queued;
  size_t pasted_since_render = 0;
  unsigned int deferred_since_render = 0;
  impl = sl_impl(self);
  if (!impl)
    return NULL;
  impl->prompt_queue.last_id = 0;
  if (!impl->readline_suspended)
    impl->prompt_queue.approval_id = 0;
  if (source)
    *source = SL_PROMPT_SOURCE_NONE;
  sl_set_readline_status(self, SL_READLINE_NONE);
  if (!prompt)
    prompt = SL_DEFAULT_PROMPT;
  if (!isatty(impl->input_fd) || !isatty(impl->output_fd)) {
    if (impl->backend) {
      sl_set_error(self, "backend queue editing requires a TTY; use "
                         "queue_backend_request for plain input");
      sl_set_readline_status(self, SL_READLINE_ERROR);
      return NULL;
    }
    result = sl_readline_plain(self, prompt);
    if (result && source)
      *source = SL_PROMPT_SOURCE_DIRECT;
    return result;
  }
  if (sl_enable_raw(self) != 0) {
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  if (!impl->readline_suspended &&
      !(impl->backend && impl->prompt_queue.editing.info.id) &&
      sl_buf_set(self, "") != 0) {
    sl_disable_raw(self);
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  if (sl_enable_bracketed_paste(impl) != 0) {
    sl_disable_raw(self);
    sl_set_error(self, "failed to enable bracketed paste");
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  if (impl->readline_suspended) {
    search = impl->suspended_search;
  } else {
    memset(&search, 0, sizeof(search));
    search.match_index = -1;
    impl->history_search_active = 0;
    impl->history_index = -1;
    free(impl->history_edit);
    impl->history_edit = NULL;
  }
  impl->bracketed_paste = 0;
  impl->request_submit = 0;
  impl->request_cancel = 0;
  impl->request_suspend = 0;
  impl->request_queue_dispatch = 0;
  impl->control_x_pending = 0;
  impl->active_readline = 1;
  sl_prompt_queue_schedule(impl);
  if (!sl_surface_is_native(impl->output_surface))
    sl_render_store_clear(impl);
  impl->active_prompt =
      impl->history_search_active && search.prompt ? search.prompt : prompt;
  if (sl_render_apply(self, impl->active_prompt) != 0) {
    impl->active_prompt = NULL;
    impl->active_readline = 0;
    sl_disable_bracketed_paste(impl);
    sl_disable_raw(self);
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  impl->readline_suspended = 0;
  memset(&impl->suspended_search, 0, sizeof(impl->suspended_search));
  /* Acquire optional scroll-region ownership before invoking producers.
   * Feed callbacks themselves never perform a terminal round trip. */
  impl->scroll_region_probe_pending = 0;
  if (impl->live_scroll_region && !sl_prompt_uses_absolute_rows(impl))
    (void)sl_try_pin_scroll_region(self);
  done = 0;
  eof = 0;
  cancelled = 0;
  interrupted = 0;
  suspended = 0;
  failed = 0;
  promoted = NULL;
  queued = NULL;
  impl->edit.enabled = 1;
  while (!done) {
    const char *render_prompt;
    char paste_bytes[8];
    size_t paste_len;
    int key;
    int queue_key_checked = 0;
    int replayed = 0, waiting_edit;
    int deferred_input = 0;
    int binding_done = 0;
    sl_key_action_t action;
    sl_backend_pump(self);
    sl_edit_end(&impl->edit);
    if (!queue_dispatch)
      impl->request_queue_dispatch = 0;
    if (impl->prompt_queue.approval_id &&
        sl_prompt_held_index(impl, impl->prompt_queue.approval_id) < 0)
      impl->prompt_queue.approval_id = 0;
    if (!impl->prompt_queue.approval_id ||
        sl_prompt_approval_index(impl, impl->prompt_queue.approval_id) < 0)
      impl->prompt_queue.approval_focused = 0;
    render_prompt =
        impl->history_search_active && search.prompt ? search.prompt : prompt;
    impl->active_prompt = render_prompt;
    sl_refresh_scroll_region(self);
    paste_len = 0;
    waiting_edit = sl_backend_wait_keys(impl);
    if (!waiting_edit &&
        sl_backend_pop_key(impl, &key, paste_bytes, &paste_len, &binding_done))
      replayed = 1;
    else if (impl->bracketed_paste ||
             (waiting_edit && sl_backend_reading_paste(impl)))
      key = sl_read_paste_input(self, paste_bytes, &paste_len);
    else
      key = sl_read_key(self);
    if ((waiting_edit || sl_backend_wait_key(impl, key)) &&
        key != SL_KEY_NONE && key != SL_KEY_CTRL_C && key != SL_KEY_CTRL_L &&
        key != SL_KEY_ESCAPE &&
        (impl->bracketed_paste || sl_backend_reading_paste(impl) ||
         !sl_find_key_binding(impl, (sl_key_t)key) || key == SL_KEY_ENTER ||
         key == (int)impl->prompt_queue.keys.enqueue_draft ||
         key == (int)impl->prompt_queue.keys.submit_or_promote_newest)) {
      if (!paste_len && key >= 128 && key < 256)
        (void)sl_read_utf8_input(self, (char)key, paste_bytes, &paste_len);
      if (sl_backend_push_key(self, key, paste_bytes, paste_len,
                              binding_done) != SL_OK) {
        failed = done = 1;
        break;
      }
      key = SL_KEY_NONE;
      deferred_input = 1;
      ++deferred_since_render;
      paste_len = 0;
      errno = 0;
    }
    if (key != SL_KEY_NONE && impl->control_x_pending) {
      int approval_key =
          key == SL_KEY_ALT_A || key == SL_KEY_ALT_E ||
          (impl->prompt_queue.approval_focused &&
           (key == SL_KEY_ALT_Y || key == SL_KEY_ALT_BASE + 'Y' ||
            key == SL_KEY_ALT_BASE + 'n' || key == SL_KEY_ALT_BASE + 'N')) ||
          (impl->prompt_queue.editing.info.id &&
           (key == SL_KEY_ALT_BASE + 'n' || key == SL_KEY_ALT_BASE + 'N'));
      impl->control_x_pending = 0;
      if (key == SL_KEY_CTRL_U)
        key = SL_KEY_CTRL_UNDERSCORE;
      else if (!approval_key && key != SL_KEY_PASTE_BEGIN &&
               key != SL_KEY_CTRL_C && key != SL_KEY_CTRL_L &&
               key != SL_KEY_ESCAPE) {
        if (key >= 128 && key < 256) {
          char bytes[4];
          size_t byte_len;
          if (!replayed)
            (void)sl_read_utf8_input(self, (char)key, bytes, &byte_len);
        }
        key = SL_KEY_UNKNOWN;
      }
    }
    if (key != SL_KEY_NONE &&
        (key != SL_KEY_CTRL_L || sl_find_key_binding(impl, (sl_key_t)key))) {
      int command = SL_EDIT_OTHER;
      if (impl->bracketed_paste && key != SL_KEY_PASTE_END)
        command = SL_EDIT_PASTE;
      else if (sl_find_key_binding(impl, (sl_key_t)key))
        command = SL_EDIT_OTHER;
      else if (key == SL_KEY_CTRL_U || key == SL_KEY_CTRL_K ||
               key == SL_KEY_CTRL_W || key == SL_KEY_ALT_D ||
               key == SL_KEY_ALT_BACKSPACE)
        command = SL_EDIT_KILL;
      else if (key == SL_KEY_CTRL_Y)
        command = SL_EDIT_YANK;
      else if (key == SL_KEY_ALT_Y)
        command = SL_EDIT_YANK_POP;
      else if (key >= 32 && key < 256 && key != 127)
        command = SL_EDIT_INSERT;
      sl_edit_begin(&impl->edit, command);
    }
    /* A watch completion may become ready while a key sequence is being
     * decoded. Preserve a real key; release FIFO work only on an otherwise
     * idle iteration. */
    if (key == SL_KEY_NONE && queue_dispatch && impl->request_queue_dispatch &&
        impl->len == 0 && !impl->prompt_queue.editing.info.id &&
        !impl->history_search_active && !impl->bracketed_paste &&
        !impl->control_x_pending &&
        sl_prompt_queue_first_queued(&impl->prompt_queue) >= 0) {
      queued = sl_prompt_queue_deliver_raw(
          &impl->prompt_queue,
          sl_prompt_queue_first_queued(&impl->prompt_queue));
      if (!queued) {
        sl_set_error(self, "failed to dispatch queued prompt");
        failed = 1;
      }
      impl->request_queue_dispatch = 0;
      done = 1;
      continue;
    }
    if (impl->bracketed_paste && key != SL_KEY_NONE) {
      if (key == SL_KEY_PASTE_END) {
        impl->bracketed_paste = 0;
      } else if (paste_len > 0 && paste_len <= impl->line_max_len - impl->len) {
        if (sl_buf_insert(self, impl->cursor, paste_bytes, paste_len) == 0) {
          impl->cursor += paste_len;
          pasted_since_render += paste_len;
        } else {
          failed = 1;
          done = 1;
        }
      }
      render_prompt =
          impl->history_search_active && search.prompt ? search.prompt : prompt;
      impl->active_prompt = render_prompt;
      if (!done && (!impl->bracketed_paste || pasted_since_render >= 4096) &&
          sl_render_apply(self, render_prompt) != 0) {
        failed = 1;
        done = 1;
      }
      if (!impl->bracketed_paste || pasted_since_render >= 4096)
        pasted_since_render = 0;
      continue;
    }
    action = SL_KEY_ACTION_PASS;
    switch (key) {
    case SL_KEY_NONE:
      if (errno == EINTR) {
        errno = 0;
        if (sl_render_apply(self, render_prompt) != 0) {
          failed = 1;
          done = 1;
        }
      } else if (errno != 0) {
        failed = 1;
        done = 1;
      } else {
        if (impl->idle_callback)
          impl->idle_callback(self, impl->idle_userdata);
        if (impl->request_submit) {
          sl_history_search_accept(self, &search);
          done = 1;
          break;
        }
        if (impl->request_cancel) {
          cancelled = 1;
          done = 1;
          break;
        }
        if (impl->request_suspend) {
          suspended = 1;
          done = 1;
          break;
        }
        if (!impl->prompt_queue.approval_id) {
          int index = sl_prompt_approval_index(impl, 0);
          if (index >= 0) {
            impl->prompt_queue.approval_id =
                impl->prompt_queue.items[index].info.id;
            impl->prompt_queue.approval_focused = 0;
          }
        }
        render_prompt = impl->history_search_active && search.prompt
                            ? search.prompt
                            : prompt;
        impl->active_prompt = render_prompt;
      }
      break;
    default:
      /* Alt-Y/N answers require explicit focus. Alt-E edits an offered approval
       * in either focus state; all other keys retain ordinary editing. */
      if ((impl->prompt_queue.approval_id || key == SL_KEY_ALT_E) &&
          sl_prompt_approval_index(impl, impl->prompt_queue.approval_id) >= 0) {
        sl_prompt_queue_t *queue = &impl->prompt_queue;
        if (queue->approval_focused &&
            (key == SL_KEY_ESCAPE || key == SL_KEY_ALT_A)) {
          queue->approval_focused = 0;
          break;
        }
        if (key == SL_KEY_ALT_E ||
            (queue->approval_focused &&
             (key == SL_KEY_ALT_BASE + 'y' || key == SL_KEY_ALT_BASE + 'Y' ||
              key == SL_KEY_ALT_BASE + 'n' || key == SL_KEY_ALT_BASE + 'N'))) {
          int index = sl_prompt_approval_index(impl, queue->approval_id);
          sl_prompt_id_t id = queue->items[index].info.id;
          int rc;
          queue->approval_id = 0;
          queue->approval_focused = 0;
          rc = key == SL_KEY_ALT_E ? sl_prompt_queue_edit_at(self, index)
               : key == SL_KEY_ALT_BASE + 'y' || key == SL_KEY_ALT_BASE + 'Y'
                   ? sl_prompt_queue_approve(self, id)
                   : sl_prompt_queue_reject(self, id);
          if (rc != SL_OK) {
            if (impl->backend)
              sl_prompt_queue_bell(impl);
            else {
              failed = 1;
              done = 1;
            }
          }
          break;
        }
      }
      if (!binding_done &&
          sl_dispatch_key_binding(self, key, &action) != SL_OK) {
        sl_set_error(self, "key callback failed");
        failed = 1;
        done = 1;
        break;
      }
      if (impl->request_submit)
        action = SL_KEY_ACTION_SUBMIT;
      else if (impl->request_cancel)
        action = SL_KEY_ACTION_CANCEL;
      else if (impl->request_suspend)
        action = SL_KEY_ACTION_SUSPEND;
      if (action != SL_KEY_ACTION_PASS)
        impl->edit.command = SL_EDIT_OTHER;
      if (action == SL_KEY_ACTION_HANDLED) {
        impl->preferred_column_valid = 0;
        break;
      }
      if (action == SL_KEY_ACTION_SUBMIT) {
        sl_history_search_accept(self, &search);
        done = 1;
        break;
      }
      if (action == SL_KEY_ACTION_CANCEL) {
        (void)sl_history_search_cancel(self, &search);
        cancelled = 1;
        done = 1;
        break;
      }
      if (action == SL_KEY_ACTION_INTERRUPT) {
        interrupted = 1;
        done = 1;
        break;
      }
      if (action == SL_KEY_ACTION_SUSPEND) {
        if (!impl->edit.changed)
          impl->edit.command = impl->edit.last_command;
        suspended = 1;
        done = 1;
        break;
      }
      if (action == SL_KEY_ACTION_RESET || key == SL_KEY_CTRL_L) {
        int reset_status;
        render_prompt = impl->history_search_active && search.prompt
                            ? search.prompt
                            : prompt;
        impl->active_prompt = render_prompt;
        impl->render_reset = 1;
        reset_status = sl_render_apply(self, render_prompt);
        impl->render_reset = 0;
        if (reset_status != 0) {
          sl_set_error(self, "failed to reset prompt layout");
          failed = 1;
          done = 1;
        }
        break;
      }
      /* Owner control bindings remain responsive while an edit hold is
       * pending. PASS defers only the native action; replay must not invoke
       * the callback twice. Submission keys retain their draft context above.
       */
      if (!replayed && key != SL_KEY_CTRL_C && key != SL_KEY_ESCAPE &&
          (sl_backend_wait_keys(impl) || sl_backend_wait_key(impl, key))) {
        if (!paste_len && key >= 128 && key < 256)
          (void)sl_read_utf8_input(self, (char)key, paste_bytes, &paste_len);
        if (sl_backend_push_key(self, key, paste_bytes, paste_len, 1) !=
            SL_OK) {
          failed = done = 1;
          break;
        }
        deferred_input = 1;
        ++deferred_since_render;
        break;
      }
      if (key == SL_KEY_CTRL_X && !impl->history_search_active) {
        impl->control_x_pending = 1;
        break;
      }
      if (key != SL_KEY_UP && key != SL_KEY_DOWN)
        impl->preferred_column_valid = 0;
      /* A callback owns a fresh undo group. PASS still retains the built-in
       * kill/yank command identity used for coalescing and yank rotation. */
      if (action == SL_KEY_ACTION_PASS) {
        if (key == SL_KEY_CTRL_U || key == SL_KEY_CTRL_K ||
            key == SL_KEY_CTRL_W || key == SL_KEY_ALT_D ||
            key == SL_KEY_ALT_BACKSPACE)
          impl->edit.command = SL_EDIT_KILL;
        else if (key == SL_KEY_CTRL_Y)
          impl->edit.command = SL_EDIT_YANK;
        else if (key == SL_KEY_ALT_Y)
          impl->edit.command = SL_EDIT_YANK_POP;
      }
      /* Configured queue keys remain reserved even when their action is a
       * no-op in the current profile/state. */
      if ((key == SL_KEY_ALT_BASE + 'k' || key == SL_KEY_ALT_BASE + 'K' ||
           key == SL_KEY_ALT_BASE + 'j' || key == SL_KEY_ALT_BASE + 'J' ||
           key == SL_KEY_ALT_BASE + 'n' || key == SL_KEY_ALT_BASE + 'N') &&
          (!sl_prompt_queue_enabled(impl) ||
           ((sl_key_t)key != impl->prompt_queue.keys.enqueue_draft &&
            (sl_key_t)key != impl->prompt_queue.keys.edit_newest &&
            (sl_key_t)key !=
                impl->prompt_queue.keys.submit_or_promote_newest))) {
        int status;
        impl->prompt_queue.approval_focused = 0;
        if (impl->history_search_active) {
          sl_history_search_accept(self, &search);
          impl->active_prompt = prompt;
        }
        status = key == SL_KEY_ALT_BASE + 'k' || key == SL_KEY_ALT_BASE + 'K'
                     ? sl_stash_draft(self)
                 : key == SL_KEY_ALT_BASE + 'j' || key == SL_KEY_ALT_BASE + 'J'
                     ? sl_append_stash(self)
                     : sl_clear_draft(self);
        if (status != 0)
          sl_prompt_queue_bell(impl);
        break;
      }
      if (key == SL_KEY_ALT_A &&
          sl_prompt_approval_index(impl, impl->prompt_queue.approval_id) >= 0) {
        int index =
            sl_prompt_approval_index(impl, impl->prompt_queue.approval_id);
        impl->prompt_queue.approval_id =
            impl->prompt_queue.items[index].info.id;
        impl->prompt_queue.approval_focused = 1;
        break;
      }
      impl->prompt_queue.approval_focused = 0;
      if (impl->history_search_active) {
        switch (key) {
        case SL_KEY_CTRL_C:
          interrupted = 1;
          done = 1;
          break;
        case SL_KEY_ENTER:
          sl_history_search_accept(self, &search);
          /* Return the match to normal editing; this Enter does not submit. */
          break;
        case SL_KEY_CTRL_R:
          if (sl_history_search_refresh(self, &search, 1) != 0) {
            sl_set_error(self, "failed to update reverse history search");
            failed = 1;
            done = 1;
          }
          break;
        case SL_KEY_ESCAPE:
        case 7:
          if (sl_history_search_cancel(self, &search) != 0) {
            sl_set_error(self, "failed to cancel reverse history search");
            failed = 1;
            done = 1;
          }
          break;
        case SL_KEY_BACKSPACE:
        case 8:
          sl_history_search_backspace(&search);
          if (sl_history_search_refresh(self, &search, 0) != 0) {
            sl_set_error(self, "failed to update reverse history search");
            failed = 1;
            done = 1;
          }
          break;
        case SL_KEY_CTRL_U:
          search.query_len = 0;
          if (search.query)
            search.query[0] = '\0';
          if (sl_history_search_refresh(self, &search, 0) != 0) {
            sl_set_error(self, "failed to update reverse history search");
            failed = 1;
            done = 1;
          }
          break;
        default:
          if (key >= 32 && key < 256 && key != 127) {
            char bytes[4];
            size_t byte_len;
            (void)sl_read_utf8_input(self, (char)key, bytes, &byte_len);
            if (search.query_len + byte_len > impl->line_max_len ||
                sl_history_search_append(&search, bytes, byte_len) != 0 ||
                sl_history_search_refresh(self, &search, 0) != 0) {
              sl_set_error(self, "failed to update reverse history search");
              failed = 1;
              done = 1;
            }
          }
          break;
        }
        break;
      }
      {
        int queue_handled;
        int queue_submit;
        queue_handled = 0;
        queue_submit = 0;
        queue_key_checked = 1;
        if (sl_handle_prompt_queue_key(self, key, &queue_handled, &queue_submit,
                                       &promoted) != SL_OK) {
          failed = 1;
          done = 1;
          break;
        }
        if (queue_handled) {
          if (queue_submit || promoted)
            done = 1;
          break;
        }
      }
      switch (key) {
      case SL_KEY_CTRL_C:
        interrupted = 1;
        done = 1;
        break;
      case SL_KEY_ENTER:
        done = 1;
        break;
      case SL_KEY_CTRL_J:
        if (sl_buf_insert(self, impl->cursor, "\n", 1) == 0)
          impl->cursor++;
        break;
      case SL_KEY_CTRL_UNDERSCORE:
        if (sl_edit_undo(&impl->edit, impl->buf, impl->cap, &impl->len,
                         &impl->cursor) < 0) {
          sl_set_error(self, "invalid draft undo state");
          failed = 1;
          done = 1;
        }
        break;
      case SL_KEY_CTRL_Y:
      case SL_KEY_ALT_Y:
        if (sl_yank_draft(self, key == SL_KEY_ALT_Y) != 0) {
          impl->edit.command = SL_EDIT_OTHER;
          sl_prompt_queue_bell(impl);
        }
        break;
      case SL_KEY_CTRL_T:
        if (sl_transpose_draft(self) != 0)
          sl_prompt_queue_bell(impl);
        break;
      case SL_KEY_CTRL_R:
        if (sl_history_search_start(self, &search) != 0) {
          sl_set_error(self, "failed to start reverse history search");
          failed = 1;
          done = 1;
        }
        break;
      case SL_KEY_CTRL_V: {
        char *path = NULL;
        char error[SL_ERROR_LEN];
        if (impl->disable_image_paste) {
          sl_set_error(self, "clipboard image paste is disabled");
        } else if (sl_clipboard_save_image(impl->image_paste_path_template,
                                           &path, error, sizeof(error)) != 0) {
          sl_set_error(self, error);
        } else {
          size_t path_len = strlen(path);
          if (path_len > impl->line_max_len - impl->len) {
            sl_set_error(self, "clipboard image path exceeds the line limit");
            sl_clipboard_discard_image(path, impl->image_paste_path_template);
          } else if (sl_buf_insert(self, impl->cursor, path, path_len) != 0) {
            sl_clipboard_discard_image(path, impl->image_paste_path_template);
            failed = 1;
            done = 1;
          } else {
            impl->cursor += path_len;
          }
          free(path);
        }
        break;
      }
      case SL_KEY_PASTE_BEGIN:
        impl->bracketed_paste = 1;
        pasted_since_render = 0;
        break;
      case SL_KEY_PASTE_END:
        impl->bracketed_paste = 0;
        break;
      case SL_KEY_LEFT:
      case SL_KEY_CTRL_B:
        if (impl->cursor > 0)
          impl->cursor -= sl_utf8_prev_cluster_len(impl->buf, impl->cursor);
        break;
      case SL_KEY_RIGHT:
      case SL_KEY_CTRL_F:
        if (impl->cursor < impl->len)
          impl->cursor +=
              sl_utf8_next_cluster_len(impl->buf, impl->len, impl->cursor);
        break;
      case SL_KEY_UP:
        if (!sl_move_visual(self, prompt, -1) && impl->cursor == impl->len)
          sl_history_nav(self, -1);
        break;
      case SL_KEY_CTRL_P:
        sl_history_nav(self, -1);
        break;
      case SL_KEY_DOWN:
        if (!sl_move_visual(self, prompt, 1) && impl->history_index != -1)
          sl_history_nav(self, 1);
        break;
      case SL_KEY_CTRL_N:
        if (impl->history_index != -1)
          sl_history_nav(self, 1);
        break;
      case SL_KEY_HOME:
      case SL_KEY_CTRL_A:
        impl->cursor = sl_line_start(impl->buf, impl->cursor);
        break;
      case SL_KEY_END:
      case SL_KEY_CTRL_E:
        impl->cursor = sl_line_end(impl->buf, impl->len, impl->cursor);
        break;
      case SL_KEY_CTRL_U: {
        size_t start;
        start = sl_line_start(impl->buf, impl->cursor);
        if (start == impl->cursor && start > 0)
          start--;
        if (sl_kill_draft(self, start, impl->cursor, 1) != 0)
          sl_prompt_queue_bell(impl);
        break;
      }
      case SL_KEY_CTRL_K: {
        size_t end;
        end = sl_line_end(impl->buf, impl->len, impl->cursor);
        /* At a line boundary, remove only the newline to join the lines. */
        if (end == impl->cursor && end < impl->len)
          end++;
        if (sl_kill_draft(self, impl->cursor, end, 0) != 0)
          sl_prompt_queue_bell(impl);
        break;
      }
      case SL_KEY_CTRL_W:
        if (impl->cursor > 0) {
          size_t start, wp;
          start = sl_line_start(impl->buf, impl->cursor);
          wp = start + sl_whitespace_word_backward(impl->buf + start,
                                                   impl->cursor - start);
          if (wp == impl->cursor)
            wp--;
          if (sl_kill_draft(self, wp, impl->cursor, 1) != 0)
            sl_prompt_queue_bell(impl);
        }
        break;
      case SL_KEY_BACKSPACE:
      case 8:
        if (impl->cursor > 0) {
          size_t prev_len;
          prev_len = sl_utf8_prev_cluster_len(impl->buf, impl->cursor);
          if (sl_buf_delete(self, impl->cursor - prev_len, prev_len) == 0)
            impl->cursor -= prev_len;
        }
        break;
      case SL_KEY_DELETE:
      case SL_KEY_CTRL_D:
        if (impl->cursor < impl->len)
          sl_buf_delete(
              self, impl->cursor,
              sl_utf8_next_cluster_len(impl->buf, impl->len, impl->cursor));
        else if (key == SL_KEY_CTRL_D && impl->len == 0) {
          eof = 1;
          done = 1;
        }
        break;
      case SL_KEY_ALT_B:
        impl->cursor = sl_word_backward(impl->buf, impl->cursor);
        break;
      case SL_KEY_ALT_F:
        impl->cursor = sl_word_forward(impl->buf, impl->len, impl->cursor);
        break;
      case SL_KEY_ALT_D:
        if (sl_kill_draft(self, impl->cursor,
                          sl_word_forward(impl->buf, impl->len, impl->cursor),
                          0) != 0)
          sl_prompt_queue_bell(impl);
        break;
      case SL_KEY_ALT_BACKSPACE:
        if (sl_kill_draft(self, sl_word_backward(impl->buf, impl->cursor),
                          impl->cursor, 1) != 0)
          sl_prompt_queue_bell(impl);
        break;
      case SL_KEY_UNKNOWN:
      case SL_KEY_ESCAPE:
        break;
      default:
        if (key >= 32 && key < 256 && key != 127) {
          char bytes[4];
          size_t byte_len;
          if (replayed && paste_len) {
            memcpy(bytes, paste_bytes, paste_len);
            byte_len = paste_len;
          } else
            (void)sl_read_utf8_input(self, (char)key, bytes, &byte_len);
          if (sl_buf_insert(self, impl->cursor, bytes, byte_len) == 0)
            impl->cursor += byte_len;
        }
        break;
      }
      break;
    }
    /* Backend admission stays asynchronous for callback submits as well as
     * native keys. Accepted remote text is never returned as local delivery. */
    if (impl->backend && done && !failed && !cancelled && !interrupted &&
        !suspended && !eof && !promoted && !queued) {
      if (sl_backend_submit(self, SL_PROMPT_QUEUE_MODE_QUEUED) != SL_OK)
        sl_prompt_queue_bell(impl);
      impl->request_submit = 0;
      done = 0;
    }
    /* Custom submit actions obey the same admission policy as Enter. */
    if (done && !failed && !cancelled && !interrupted && !suspended && !eof &&
        !promoted && !queued && impl->len && !impl->history_search_active &&
        !queue_key_checked) {
      int admitted = 0;
      int status = sl_prompt_queue_admit(
          self,
          impl->prompt_queue.keys.submit_or_promote_newest != SL_KEY_NONE &&
                  (sl_key_t)key ==
                      impl->prompt_queue.keys.submit_or_promote_newest
              ? SL_PROMPT_QUEUE_MODE_STEER
              : SL_PROMPT_QUEUE_MODE_QUEUED,
          &admitted);
      if (admitted || status != SL_OK) {
        done = 0;
        impl->request_submit = 0;
        if (status != SL_OK)
          sl_prompt_queue_bell(impl);
      }
    }
    /* Every submit route (Enter, key binding, idle/watch callback) shares the
     * hold gate. Editing never becomes implicit approval. */
    if (done && !failed && !cancelled && !interrupted && !suspended && !eof &&
        !promoted && !queued && impl->prompt_queue.editing.info.id &&
        (impl->prompt_queue.editing.info.held || impl->len == 0)) {
      int status = SL_OK;
      impl->request_submit = 0;
      done = 0;
      if (impl->len)
        status = sl_prompt_queue_enqueue_draft_method(self);
      else
        status =
            sl_prompt_queue_reject(self, impl->prompt_queue.editing.info.id);
      if (status != SL_OK) {
        failed = 1;
        done = 1;
      }
    }
    render_prompt =
        impl->history_search_active && search.prompt ? search.prompt : prompt;
    impl->active_prompt = render_prompt;
    /* Deferred keys leave the draft unchanged. Keep callbacks live but
     * coalesce frame construction, as for a large bracketed paste. A real
     * idle tick still redraws even below this bounded batch. */
    if (!done && (!deferred_input || deferred_since_render >= 4096)) {
      if (sl_render_apply(self, render_prompt) != 0) {
        failed = 1;
        done = 1;
      }
      deferred_since_render = 0;
    }
  }
  sl_edit_end(&impl->edit);
  impl->edit.enabled = 0;
  impl->active_readline = 0;
  if (suspended) {
    /* Only the editor continuation is retained. The owner closes the output
     * session before a child runs; producer bytes are never saved or replayed.
     */
    if (!impl->output_stream_active &&
        (sl_surface_is_native(impl->output_surface)
             ? sl_native_session_close(self)
             : sl_render_clear_active(self)) != 0) {
      failed = 1;
    } else {
      impl->suspended_search = search;
      impl->readline_suspended = 1;
      /* Public host edits remain part of this draft while the owner has
       * terminal control. Unjournaled mutations would invalidate undo ranges.
       */
      impl->edit.enabled = 1;
      impl->active_prompt = NULL;
      sl_disable_bracketed_paste(impl);
      if (!impl->output_stream_active) {
        sl_release_auto_scroll_region(impl);
        sl_disable_raw(self);
      }
      (void)sl_show_cursor(impl);
      sl_set_readline_status(self, SL_READLINE_SUSPENDED);
      return NULL;
    }
  }
  impl->active_prompt = prompt;
  if (interrupted) {
    sl_backend_cancel_waiting(impl);
    if (impl->backend && impl->prompt_queue.editing.info.id)
      (void)sl_backend_cancel_edit(self);
    sl_prompt_queue_restore_edit(self);
    sl_prompt_queue_stop_queued_turns(self);
    free(promoted);
    free(queued);
    sl_history_search_cleanup(self, &search);
    sl_render_clear_active(self);
    sl_release_auto_scroll_region(impl);
    (void)sl_show_cursor(impl);
    impl->active_prompt = NULL;
    impl->bracketed_paste = 0;
    sl_disable_bracketed_paste(impl);
    sl_disable_raw(self);
    sl_set_readline_status(self, SL_READLINE_INTERRUPTED);
    if (sl_surface_is_native(impl->output_surface)) {
      /* Signal handlers and the shell must see unrestricted terminal rows. */
      sl_reset_scroll_region(impl->output_fd);
      (void)sl_write_frame_cursor(impl, sl_terminal_rows(impl),
                                  sl_terminal_rows(impl) - 1, 0);
    }
    raise(SIGINT);
    if (sl_surface_is_native(impl->output_surface)) {
      /* A returning handler keeps the output session available. */
      (void)sl_output_surface_reconcile(
          self, sl_prompt_top(impl, sl_output_prompt_rows(impl)));
      (void)sl_write_frame_cursor(impl, sl_terminal_rows(impl),
                                  sl_terminal_rows(impl) - 1, 0);
    }
    return NULL;
  }
  if (failed) {
    sl_prompt_queue_restore_edit(self);
    free(promoted);
    free(queued);
    sl_history_search_cleanup(self, &search);
    sl_release_auto_scroll_region(impl);
    (void)sl_show_cursor(impl);
    impl->active_prompt = NULL;
    impl->bracketed_paste = 0;
    sl_disable_bracketed_paste(impl);
    sl_disable_raw(self);
    free(impl->history_edit);
    impl->history_edit = NULL;
    impl->history_index = -1;
    if (!(impl->backend && impl->prompt_queue.editing.info.id))
      sl_buf_set(self, "");
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  impl->cursor = impl->len;
  if (eof || cancelled) {
    if (cancelled)
      sl_prompt_queue_stop_queued_turns(self);
    if (sl_history_search_cancel(self, &search) != 0)
      failed = 1;
  } else {
    sl_history_search_accept(self, &search);
  }
  if (!failed && (sl_render_apply(self, prompt) != 0 ||
                  sl_render_finish(self, queue_dispatch) != 0))
    failed = 1;
  impl->bracketed_paste = 0;
  sl_disable_bracketed_paste(impl);
  /* The native session owns terminal input between prompts too, so cursor
   * reports remain readable until output_stream_end restores the terminal. */
  if ((!queue_dispatch && !sl_surface_is_native(impl->output_surface)) || eof ||
      failed)
    sl_disable_raw(self);
  sl_release_auto_scroll_region(impl);
  (void)sl_show_cursor(impl);
  impl->active_prompt = NULL;
  if (failed) {
    sl_prompt_queue_restore_edit(self);
    free(promoted);
    free(queued);
    sl_history_search_cleanup(self, &search);
    free(impl->history_edit);
    impl->history_edit = NULL;
    impl->history_index = -1;
    if (!(impl->backend && impl->prompt_queue.editing.info.id))
      sl_buf_set(self, "");
    sl_set_readline_status(self, SL_READLINE_ERROR);
    return NULL;
  }
  if (eof || cancelled) {
    if (cancelled && impl->backend && impl->prompt_queue.editing.info.id)
      (void)sl_backend_cancel_edit(self);
    if (cancelled)
      sl_backend_cancel_waiting(impl);
    sl_prompt_queue_restore_edit(self);
    free(promoted);
    free(queued);
    sl_history_search_cleanup(self, &search);
    free(impl->history_edit);
    impl->history_edit = NULL;
    impl->history_index = -1;
    if (!(impl->backend && impl->prompt_queue.editing.info.id))
      sl_buf_set(self, "");
    sl_set_readline_status(self,
                           cancelled ? SL_READLINE_CANCELLED : SL_READLINE_EOF);
    return NULL;
  }
  result = promoted ? promoted : queued ? queued : sl_strdup(impl->buf);
  if (!result) {
    sl_prompt_queue_restore_edit(self);
    sl_disable_raw(self);
    sl_set_readline_status(self, SL_READLINE_ERROR);
    sl_history_search_cleanup(self, &search);
    free(impl->history_edit);
    impl->history_edit = NULL;
    impl->history_index = -1;
    sl_buf_set(self, "");
    return NULL;
  }
  if (!promoted && !queued && impl->history_auto_add &&
      sl_history_add_impl(self, result) != SL_OK) {
    free(result);
    result = NULL;
    sl_disable_raw(self);
    sl_set_readline_status(self, SL_READLINE_ERROR);
  }
  {
    sl_prompt_source_t result_source;
    result_source = promoted || (queued && impl->prompt_queue.last_mode ==
                                               SL_PROMPT_QUEUE_MODE_STEER)
                        ? SL_PROMPT_SOURCE_PROMOTED
                    : queued ? SL_PROMPT_SOURCE_QUEUED
                             : SL_PROMPT_SOURCE_DIRECT;
    if (result) {
      if (!promoted && !queued) {
        impl->prompt_queue.last_id = impl->prompt_queue.editing.info.id;
        sl_prompt_queue_finish_edit(&impl->prompt_queue);
      }
      sl_set_readline_status(self, SL_READLINE_SUBMITTED);
      if (source)
        *source = result_source;
      if (!queued)
        sl_prompt_queue_resume_on_user_turn(impl, result_source);
    }
  }
  sl_history_search_cleanup(self, &search);
  if (!result)
    sl_prompt_queue_restore_edit(self);
  free(impl->history_edit);
  impl->history_edit = NULL;
  impl->history_index = -1;
  sl_buf_set(self, "");
  return result;
}

static char *sl_readline_method(sl_t *self, const char *prompt) {
  return sl_readline_impl(self, prompt, 0, NULL);
}

static char *sl_next_prompt_method(sl_t *self, const char *prompt,
                                   sl_prompt_source_t *source) {
  sl_impl_t *impl;
  char *result;
  if (source)
    *source = SL_PROMPT_SOURCE_NONE;
  impl = sl_impl(self);
  if (!impl)
    return NULL;
  impl->prompt_queue.last_id = 0;
  if (sl_prompt_queue_enabled(impl) && !impl->readline_suspended &&
      !impl->prompt_queue.editing.info.id &&
      sl_prompt_queue_first_queued(&impl->prompt_queue) >= 0) {
    result = sl_prompt_queue_deliver_raw(
        &impl->prompt_queue, sl_prompt_queue_first_queued(&impl->prompt_queue));
    if (!result) {
      sl_set_error(self, "failed to dequeue prompt");
      sl_set_readline_status(self, SL_READLINE_ERROR);
      return NULL;
    }
    sl_set_readline_status(self, SL_READLINE_SUBMITTED);
    if (source)
      *source = impl->prompt_queue.last_mode == SL_PROMPT_QUEUE_MODE_STEER
                    ? SL_PROMPT_SOURCE_PROMOTED
                    : SL_PROMPT_SOURCE_QUEUED;
    return result;
  }
  result = sl_readline_impl(self, prompt, 1, source);
  return result;
}

static void sl_destroy_method(sl_t *self) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!self)
    return;
  if (sl_backend_callback_active(impl)) {
    sl_set_error(self,
                 "cannot destroy softline handle from a backend callback");
    return;
  }
  if (impl && (impl->watch_callback_depth != 0 || impl->history_busy)) {
    sl_set_error(self,
                 impl->history_busy
                     ? "cannot destroy softline handle from a history callback"
                     : "cannot destroy softline handle from a watch callback");
    return;
  }
  if (impl) {
    sl_backend_destroy(self);
    if (sl_surface_is_native(impl->output_surface))
      (void)sl_native_session_close(self);
    sl_release_auto_scroll_region(impl);
    sl_disable_raw(self);
    (void)sl_show_cursor(impl);
    (void)sl_history_close_impl(self);
    sl_history_clear(&impl->history);
    sl_prompt_queue_clear_raw(&impl->prompt_queue);
    sl_prompt_queue_finish_edit(&impl->prompt_queue);
    free(impl->prompt_queue.items);
    sl_statusline_clear(&impl->statusline);
    free(impl->quoted_prompt_prefix);
    free(impl->status_message);
    free(impl->status_message_prefix);
    free(impl->image_paste_path_template);
    sl_render_store_clear(impl);
    sl_surface_destroy(impl->output_surface);
    free(impl->history_edit);
    sl_history_search_cleanup(self, &impl->suspended_search);
    free(impl->pending_input);
    free(impl->retained_prompt);
    sl_edit_destroy(&impl->edit);
    free(impl->buf);
    free(impl);
  }
  free(self);
}

static void sl_free_string_method(sl_t *self, char *ptr) {
  (void)self;
  free(ptr);
}

static int sl_set_screen_width_method(sl_t *self, int width) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || width < 0) {
    sl_set_error(self, "invalid screen width");
    return SL_ERROR_INVALID;
  }
  if (!sl_surface_is_native(impl->output_surface) && impl->active_prompt &&
      impl->rendered_rows > 0 && sl_render_clear_active(self) != 0) {
    sl_set_error(self, "failed to clear prompt before changing width");
    return SL_ERROR_IO;
  }
  impl->screen_width = width;
  if (impl->active_prompt && sl_render_apply(self, impl->active_prompt) != 0) {
    sl_set_error(self, "failed to redraw prompt after changing width");
    return SL_ERROR_IO;
  }
  return SL_OK;
}

static int sl_set_live_scroll_region_method(sl_t *self, int enabled) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || enabled < 0) {
    sl_set_error(self, "invalid live scroll region configuration");
    return SL_ERROR_INVALID;
  }
  if (!enabled)
    impl->scroll_region_probe_pending = 0;
  else if (!impl->live_scroll_region && impl->active_prompt &&
           !sl_prompt_uses_absolute_rows(impl))
    impl->scroll_region_probe_pending = 1;
  if (!enabled && impl->auto_scroll_pinned) {
    if (sl_render_clear_active(self) != 0) {
      sl_set_error(self, "failed to leave live scroll region");
      return SL_ERROR_IO;
    }
    sl_release_auto_scroll_region(impl);
    impl->live_scroll_region = 0;
    if (impl->active_prompt &&
        sl_render_apply(self, impl->active_prompt) != 0) {
      sl_set_error(self,
                   "failed to redraw prompt after leaving live scroll region");
      return SL_ERROR_IO;
    }
    if (sl_show_cursor(impl) != 0) {
      sl_set_error(self,
                   "failed to restore cursor after leaving live scroll region");
      return SL_ERROR_IO;
    }
    return SL_OK;
  }
  impl->live_scroll_region = enabled;
  return SL_OK;
}

static int sl_set_image_paste_path_template_method(sl_t *self,
                                                   const char *path_template) {
  sl_impl_t *impl = sl_impl(self);
  char *copy = NULL;
  if (!impl)
    return SL_ERROR_INVALID;
  if (!sl_clipboard_valid_path_template(path_template)) {
    sl_set_error(self, "invalid image paste path template: use an absolute "
                       "path with one '*' placeholder");
    return SL_ERROR_INVALID;
  }
  if (path_template) {
    copy = strdup(path_template);
    if (!copy) {
      sl_set_error(self, "failed to copy image paste path template");
      return SL_ERROR;
    }
  }
  free(impl->image_paste_path_template);
  impl->image_paste_path_template = copy;
  return SL_OK;
}

static int sl_set_prompt_queue_method(sl_t *self, int enabled, int max_entries,
                                      int preview_entries) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (!impl || enabled < 0 || max_entries < 1 || preview_entries < 1) {
    sl_set_error(self, "invalid prompt queue configuration");
    return SL_ERROR_INVALID;
  }
  if (!enabled && impl->prompt_queue.editing.info.id) {
    sl_set_error(self,
                 "finish or reject the queued edit before disabling queueing");
    return SL_ERROR_INVALID;
  }
  if (enabled &&
      impl->prompt_queue.len + (impl->prompt_queue.editing.info.id ? 1 : 0) >
          max_entries) {
    sl_set_error(self, "prompt queue exceeds requested capacity");
    return SL_ERROR_INVALID;
  }
  if (!enabled)
    sl_prompt_queue_clear_raw(&impl->prompt_queue);
  if (!enabled)
    impl->prompt_queue.stopped = 0;
  impl->prompt_queue.enabled = enabled;
  impl->prompt_queue.max_entries = max_entries;
  impl->prompt_queue.preview_entries = preview_entries;
  sl_prompt_queue_sync_queued_turns_delivery(impl);
  return SL_OK;
}

static void sl_prompt_queue_profile_keys(sl_prompt_queue_profile_t profile,
                                         sl_prompt_queue_keys_t *keys) {
  if (!keys)
    return;
  keys->enqueue_draft = SL_KEY_TAB;
  keys->edit_newest = SL_KEY_ALT_E;
  keys->submit_or_promote_newest = SL_KEY_NONE;
  if (profile == SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) {
    keys->enqueue_draft = SL_KEY_ENTER;
    keys->submit_or_promote_newest = SL_KEY_ALT_ENTER;
  }
}

static size_t sl_prompt_queue_count_method(const sl_t *self) {
  const sl_impl_t *impl;
  impl = sl_impl((sl_t *)self);
  return impl ? (size_t)impl->prompt_queue.len : 0;
}

static size_t sl_prompt_queue_capacity_method(const sl_t *self) {
  const sl_impl_t *impl;
  impl = sl_impl((sl_t *)self);
  return impl ? (size_t)impl->prompt_queue.max_entries : 0;
}

static int sl_prompt_queue_peek_method(const sl_t *self, size_t index,
                                       char **out) {
  sl_impl_t *impl;
  char *copy;
  if (out)
    *out = NULL;
  impl = sl_impl((sl_t *)self);
  if (!impl || !out) {
    sl_set_error((sl_t *)self, "invalid prompt queue peek arguments");
    return SL_ERROR_INVALID;
  }
  if (!sl_prompt_queue_index_valid(index, impl->prompt_queue.len, 0)) {
    sl_set_error((sl_t *)self, "prompt queue index is out of range");
    return SL_ERROR_INVALID;
  }
  copy = sl_strdup(impl->prompt_queue.items[index].text);
  if (!copy) {
    sl_set_error((sl_t *)self, "failed to copy prompt queue entry");
    return SL_ERROR_NOMEM;
  }
  *out = copy;
  return SL_OK;
}

static int sl_prompt_queue_insert_mode(sl_t *self, size_t index,
                                       const char *text, int held,
                                       sl_prompt_queue_mode_t mode,
                                       sl_prompt_id_t *out_id) {
  sl_impl_t *impl;
  int rc;
  if (out_id)
    *out_id = 0;
  impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK)
    return SL_ERROR_INVALID;
  if (!text || text[0] == '\0' || (held != 0 && held != 1) ||
      strlen(text) > impl->line_max_len ||
      !sl_prompt_queue_index_valid(index, impl->prompt_queue.len, 1)) {
    sl_set_error(self, "prompt queue insertion requires text within the line "
                       "limit and a valid index");
    return SL_ERROR_INVALID;
  }
  rc = sl_prompt_queue_insert_raw(impl, (int)index, text, mode, held, 0);
  if (rc == 1) {
    sl_set_error(self, "prompt queue is full");
    return SL_ERROR_FULL;
  }
  if (rc != 0) {
    sl_set_error(self, "failed to insert prompt queue entry");
    return SL_ERROR_NOMEM;
  }
  if (out_id)
    *out_id = impl->prompt_queue.items[index].info.id;
  sl_prompt_queue_schedule(impl);
  return SL_OK;
}

static int sl_prompt_queue_insert_method(sl_t *self, size_t index,
                                         const char *text, int held,
                                         sl_prompt_id_t *out_id) {
  return sl_prompt_queue_insert_mode(self, index, text, held,
                                     SL_PROMPT_QUEUE_MODE_QUEUED, out_id);
}

static int sl_inject_queued_prompt_method(sl_t *self, const char *text,
                                          sl_prompt_id_t *out_id) {
  const sl_impl_t *impl = sl_impl(self);
  return sl_prompt_queue_insert_mode(
      self, impl ? (size_t)impl->prompt_queue.len : 0, text, 1,
      SL_PROMPT_QUEUE_MODE_QUEUED, out_id);
}

static int sl_inject_steer_prompt_method(sl_t *self, const char *text,
                                         sl_prompt_id_t *out_id) {
  const sl_impl_t *impl = sl_impl(self);
  return sl_prompt_queue_insert_mode(
      self, impl ? (size_t)impl->prompt_queue.len : 0, text, 1,
      SL_PROMPT_QUEUE_MODE_STEER, out_id);
}

static int sl_set_auto_approval_method(sl_t *self, int enabled) {
  sl_impl_t *impl = sl_impl(self);
  int i;
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (!impl || (enabled != 0 && enabled != 1)) {
    sl_set_error(self, "auto_approval must be 0 or 1");
    return SL_ERROR_INVALID;
  }
  impl->prompt_queue.auto_approval = enabled;
  if (enabled) {
    for (i = 0; i < impl->prompt_queue.len; i++)
      impl->prompt_queue.items[i].info.held = 0;
    impl->prompt_queue.editing.info.held = 0;
    impl->prompt_queue.approval_id = 0;
    sl_prompt_queue_schedule(impl);
  }
  return SL_OK;
}

static int sl_get_auto_approval_method(const sl_t *self, int *out) {
  const sl_impl_t *impl = sl_impl_const(self);
  if (!impl || !out)
    return SL_ERROR_INVALID;
  *out = impl->prompt_queue.auto_approval;
  return SL_OK;
}

static int sl_prompt_queue_append_method(sl_t *self, const char *text, int held,
                                         sl_prompt_id_t *out_id) {
  const sl_impl_t *impl = sl_impl(self);
  return sl_prompt_queue_insert_method(
      self, impl ? (size_t)impl->prompt_queue.len : 0, text, held, out_id);
}

static int sl_prompt_queue_replace_method(sl_t *self, size_t index,
                                          const char *text) {
  sl_impl_t *impl;
  int rc;
  impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK)
    return SL_ERROR_INVALID;
  if (!text || text[0] == '\0' || strlen(text) > impl->line_max_len ||
      !sl_prompt_queue_index_valid(index, impl->prompt_queue.len, 0)) {
    sl_set_error(self, "prompt queue replacement requires text within the line "
                       "limit and a valid index");
    return SL_ERROR_INVALID;
  }
  rc = sl_prompt_queue_replace_raw(impl, (int)index, text);
  if (rc != 0) {
    sl_set_error(self, "failed to replace prompt queue entry");
    return SL_ERROR_NOMEM;
  }
  return SL_OK;
}

static int sl_prompt_queue_take_method(sl_t *self, size_t index, char **out) {
  sl_impl_t *impl;
  if (out)
    *out = NULL;
  impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (impl)
    impl->prompt_queue.last_id = 0;
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK)
    return SL_ERROR_INVALID;
  if (!out || !sl_prompt_queue_index_valid(index, impl->prompt_queue.len, 0)) {
    sl_set_error(self,
                 "prompt queue take requires an output and a valid index");
    return SL_ERROR_INVALID;
  }
  if (!sl_prompt_queue_eligible(&impl->prompt_queue, (int)index)) {
    sl_set_error(self, "queued prompt is not eligible for delivery");
    return SL_ERROR_INVALID;
  }
  *out = sl_prompt_queue_deliver_raw(&impl->prompt_queue, (int)index);
  if (!*out) {
    sl_set_error(self, "failed to take prompt queue entry");
    return SL_ERROR;
  }
  sl_prompt_queue_schedule(impl);
  return SL_OK;
}

static int sl_prompt_queue_get_mode_method(const sl_t *self, size_t index,
                                           sl_prompt_queue_mode_t *out) {
  const sl_impl_t *impl = sl_impl((sl_t *)self);
  if (!impl || !out ||
      !sl_prompt_queue_index_valid(index, impl->prompt_queue.len, 0)) {
    sl_set_error((sl_t *)self, "invalid prompt queue mode query");
    return SL_ERROR_INVALID;
  }
  *out = impl->prompt_queue.items[index].info.mode;
  return SL_OK;
}

static int sl_prompt_queue_set_mode_method(sl_t *self, size_t index,
                                           sl_prompt_queue_mode_t mode) {
  sl_impl_t *impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (!impl || !sl_prompt_queue_enabled(impl) ||
      !sl_prompt_queue_index_valid(index, impl->prompt_queue.len, 0) ||
      (mode != SL_PROMPT_QUEUE_MODE_QUEUED &&
       mode != SL_PROMPT_QUEUE_MODE_STEER)) {
    sl_set_error(self, "invalid prompt queue mode update");
    return SL_ERROR_INVALID;
  }
  impl->prompt_queue.items[index].info.mode = mode;
  sl_prompt_queue_schedule(impl);
  return SL_OK;
}

static int sl_prompt_queue_info_method(const sl_t *self, size_t index,
                                       sl_prompt_queue_entry_info_t *out) {
  const sl_impl_t *impl = sl_impl_const(self);
  if (out)
    memset(out, 0, sizeof(*out));
  if (!impl || !out ||
      !sl_prompt_queue_index_valid(index, impl->prompt_queue.len, 0)) {
    sl_set_error((sl_t *)self, "invalid prompt queue info query");
    return SL_ERROR_INVALID;
  }
  *out = impl->prompt_queue.items[index].info;
  out->eligible = sl_prompt_queue_eligible(&impl->prompt_queue, (int)index);
  return SL_OK;
}

static int
sl_prompt_queue_draft_info_method(const sl_t *self,
                                  sl_prompt_queue_entry_info_t *out) {
  const sl_impl_t *impl = sl_impl_const(self);
  if (out)
    memset(out, 0, sizeof(*out));
  if (!impl || !out)
    return SL_ERROR_INVALID;
  *out = impl->prompt_queue.editing.info;
  out->eligible = 0;
  return SL_OK;
}

static int sl_prompt_queue_approve_method(sl_t *self, sl_prompt_id_t id) {
  sl_impl_t *impl = sl_impl(self);
  int i;
  if (impl && impl->backend)
    return sl_backend_action(self, id, SL_QUEUE_APPROVE);
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK || !id)
    return SL_ERROR_INVALID;
  if (impl->prompt_queue.editing.info.id == id) {
    if (sl_prompt_queue_require_fifo(self, impl,
                                     &impl->prompt_queue.editing.info) != SL_OK)
      return SL_ERROR_INVALID;
    if (impl->prompt_queue.approval_id == id)
      impl->prompt_queue.approval_id = 0;
    impl->prompt_queue.editing.info.held = 0;
    return SL_OK;
  }
  for (i = 0; i < impl->prompt_queue.len; i++) {
    sl_prompt_queue_entry_info_t *info = &impl->prompt_queue.items[i].info;
    if (info->id != id)
      continue;
    if (sl_prompt_queue_require_fifo(self, impl, info) != SL_OK)
      return SL_ERROR_INVALID;
    if (impl->prompt_queue.approval_id == id)
      impl->prompt_queue.approval_id = 0;
    info->held = 0;
    sl_prompt_queue_schedule(impl);
    return SL_OK;
  }
  sl_set_error(self, "prompt queue identity does not exist");
  return SL_ERROR_INVALID;
}

static int sl_prompt_queue_reject_method(sl_t *self, sl_prompt_id_t id) {
  sl_impl_t *impl = sl_impl(self);
  int i;
  if (impl && impl->backend) {
    int status = sl_backend_action(self, id, SL_QUEUE_WITHDRAW);
    if (status == SL_OK && impl->prompt_queue.editing.info.id == id) {
      impl->history_search_active = 0;
      sl_queue_cached_edit_restore(self);
      impl->request_submit = 0;
      sl_history_nav_reset(impl);
    }
    return status;
  }
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK || !id)
    return SL_ERROR_INVALID;
  if (impl->prompt_queue.editing.info.id == id) {
    if (sl_prompt_queue_require_fifo(self, impl,
                                     &impl->prompt_queue.editing.info) != SL_OK)
      return SL_ERROR_INVALID;
    /* Rejection ends the search context as well as the edited identity.
     * Clear its undo baseline before replacing the candidate buffer. */
    impl->history_search_active = 0;
    if (sl_buf_set(self, "") != 0)
      return SL_ERROR_NOMEM;
    if (impl->prompt_queue.approval_id == id)
      impl->prompt_queue.approval_id = 0;
    sl_prompt_queue_finish_edit(&impl->prompt_queue);
    impl->request_submit = 0;
    /* The read loop owns the retired search allocation and frees it on
     * exit/restart. Ctrl-G can no longer restore rejected content. */
    sl_history_nav_reset(impl);
    sl_prompt_queue_schedule(impl);
    return SL_OK;
  }
  for (i = 0; i < impl->prompt_queue.len; i++) {
    if (impl->prompt_queue.items[i].info.id == id) {
      if (sl_prompt_queue_require_fifo(
              self, impl, &impl->prompt_queue.items[i].info) != SL_OK)
        return SL_ERROR_INVALID;
      if (impl->prompt_queue.approval_id == id)
        impl->prompt_queue.approval_id = 0;
      free(sl_prompt_queue_take_raw(&impl->prompt_queue, i));
      sl_prompt_queue_schedule(impl);
      return SL_OK;
    }
  }
  sl_set_error(self, "prompt queue identity does not exist");
  return SL_ERROR_INVALID;
}

static sl_prompt_id_t sl_last_prompt_id_method(const sl_t *self) {
  const sl_impl_t *impl = sl_impl_const(self);
  return impl ? impl->prompt_queue.last_id : 0;
}

static int sl_prompt_queue_clear_method(sl_t *self) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (!impl) {
    sl_set_error(self, "invalid prompt queue handle");
    return SL_ERROR_INVALID;
  }
  sl_prompt_queue_clear_raw(&impl->prompt_queue);
  return SL_OK;
}

static int sl_prompt_queue_enqueue_draft_method(sl_t *self) {
  sl_impl_t *impl;
  int rc, managed_edit;
  impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_submit(self, SL_PROMPT_QUEUE_MODE_QUEUED);
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK)
    return SL_ERROR_INVALID;
  if (impl->active_readline && impl->len == 0 &&
      impl->prompt_queue.editing.info.id)
    return sl_prompt_queue_reject(self, impl->prompt_queue.editing.info.id);
  if (!impl->active_readline || impl->len == 0) {
    sl_set_error(
        self, "prompt queue draft enqueue requires an active nonempty editor");
    return SL_ERROR_INVALID;
  }
  rc = sl_prompt_queue_append_raw(impl, impl->buf, SL_PROMPT_QUEUE_MODE_QUEUED);
  if (rc == 1) {
    sl_set_error(self, "prompt queue is full");
    return SL_ERROR_FULL;
  }
  if (rc != 0) {
    sl_set_error(self, "failed to queue active draft");
    return SL_ERROR_NOMEM;
  }
  if (impl->history_auto_add &&
      (!impl->prompt_queue.editing.history_recorded ||
       (!impl->prompt_queue.policy_callback &&
        !impl->prompt_queue.editing.info.policy.enqueue))) {
    int status = sl_history_add_impl(self, impl->buf);
    if (status != SL_OK) {
      char *discard = sl_prompt_queue_take_raw(&impl->prompt_queue,
                                               impl->prompt_queue.len - 1);
      free(discard);
      return status;
    }
    impl->prompt_queue.items[impl->prompt_queue.len - 1].history_recorded = 1;
  }
  if (sl_buf_set(self, "") != 0) {
    free(sl_prompt_queue_take_raw(&impl->prompt_queue,
                                  impl->prompt_queue.len - 1));
    sl_set_error(self, "failed to clear queued draft");
    return SL_ERROR;
  }
  managed_edit = impl->prompt_queue.editing.info.policy.enqueue;
  sl_prompt_queue_finish_requeue(&impl->prompt_queue);
  impl->request_submit = 0;
  sl_history_nav_reset(impl);
  if (managed_edit)
    sl_prompt_queue_schedule(impl);
  return SL_OK;
}

static int sl_set_prompt_queue_policy_method(
    sl_t *self, sl_prompt_queue_policy_callback_t callback, void *userdata) {
  sl_impl_t *impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK)
    return SL_ERROR_INVALID;
  impl->prompt_queue.policy_callback = callback;
  impl->prompt_queue.policy_userdata = userdata;
  sl_prompt_queue_schedule(impl);
  return SL_OK;
}

static int sl_prompt_queue_set_executable_method(sl_t *self, sl_prompt_id_t id,
                                                 int executable) {
  sl_impl_t *impl = sl_impl(self);
  sl_prompt_queue_entry_info_t *info = NULL;
  int i;
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK || !id ||
      (executable != 0 && executable != 1))
    return SL_ERROR_INVALID;
  if (impl->prompt_queue.editing.info.id == id)
    info = &impl->prompt_queue.editing.info;
  for (i = 0; !info && i < impl->prompt_queue.len; i++)
    if (impl->prompt_queue.items[i].info.id == id)
      info = &impl->prompt_queue.items[i].info;
  if (!info) {
    sl_set_error(self, "prompt queue identity does not exist");
    return SL_ERROR_INVALID;
  }
  info->policy.executable = executable;
  sl_prompt_queue_schedule(impl);
  return SL_OK;
}

static int sl_get_prompt_queue_stopped_method(const sl_t *self, int *out) {
  const sl_impl_t *impl = sl_impl_const(self);
  if (!impl || !out)
    return SL_ERROR_INVALID;
  *out = impl->prompt_queue.stopped;
  return SL_OK;
}

static int sl_prompt_queue_resume_method(sl_t *self) {
  sl_impl_t *impl = sl_impl(self);
  if (impl && impl->backend) {
    sl_queue_request_t request;
    memset(&request, 0, sizeof(request));
    request.operation = SL_QUEUE_RESUME;
    return sl_queue_backend_request(self, &request, NULL);
  }
  if (sl_prompt_queue_require_enabled(self, impl) != SL_OK)
    return SL_ERROR_INVALID;
  sl_prompt_queue_resume_backlog(impl);
  return SL_OK;
}

static int
sl_set_prompt_queue_delivery_method(sl_t *self,
                                    sl_prompt_queue_delivery_t delivery) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (impl && impl->backend)
    return sl_backend_unsupported(self);
  if (!impl || (delivery != SL_PROMPT_QUEUE_DELIVERY_AUTO &&
                delivery != SL_PROMPT_QUEUE_DELIVERY_MANUAL)) {
    sl_set_error(self, "invalid prompt queue delivery mode");
    return SL_ERROR_INVALID;
  }
  impl->prompt_queue.host_controls_delivery =
      delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL;
  if (impl->prompt_queue.profile == SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) {
    sl_prompt_queue_sync_queued_turns_delivery(impl);
  } else
    impl->prompt_queue.delivery = delivery;
  sl_prompt_queue_schedule(impl);
  return SL_OK;
}

static int
sl_get_prompt_queue_delivery_method(const sl_t *self,
                                    sl_prompt_queue_delivery_t *out) {
  const sl_impl_t *impl;
  impl = sl_impl((sl_t *)self);
  if (!impl || !out) {
    sl_set_error((sl_t *)self, "invalid prompt queue delivery output");
    return SL_ERROR_INVALID;
  }
  *out = impl->prompt_queue.delivery;
  return SL_OK;
}

static int
sl_set_prompt_queue_profile_method(sl_t *self,
                                   sl_prompt_queue_profile_t profile) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || (profile != SL_PROMPT_QUEUE_PROFILE_DEFAULT &&
                profile != SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS)) {
    sl_set_error(self, "invalid prompt queue profile");
    return SL_ERROR_INVALID;
  }
  impl->prompt_queue.profile = profile;
  if (impl->backend) {
    sl_prompt_queue_profile_keys(profile, &impl->prompt_queue.keys);
    return SL_OK;
  }
  impl->prompt_queue.host_controls_delivery = 0;
  if (profile != SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) {
    impl->prompt_queue.delivery = SL_PROMPT_QUEUE_DELIVERY_AUTO;
    sl_prompt_queue_resume_backlog(impl);
  }
  sl_prompt_queue_profile_keys(profile, &impl->prompt_queue.keys);
  sl_prompt_queue_sync_queued_turns_delivery(impl);
  return SL_OK;
}

static int sl_get_prompt_queue_profile_method(const sl_t *self,
                                              sl_prompt_queue_profile_t *out) {
  const sl_impl_t *impl;
  impl = sl_impl((sl_t *)self);
  if (!impl || !out) {
    sl_set_error((sl_t *)self, "invalid prompt queue profile output");
    return SL_ERROR_INVALID;
  }
  *out = impl->prompt_queue.profile;
  return SL_OK;
}

static int sl_prompt_queue_keys_valid(const sl_prompt_queue_keys_t *keys) {
  if (!keys)
    return 0;
  if (keys->enqueue_draft != SL_KEY_NONE &&
      (keys->enqueue_draft == keys->edit_newest ||
       keys->enqueue_draft == keys->submit_or_promote_newest))
    return 0;
  if (keys->edit_newest != SL_KEY_NONE &&
      keys->edit_newest == keys->submit_or_promote_newest)
    return 0;
  return 1;
}

static int sl_set_prompt_queue_keys_method(sl_t *self,
                                           const sl_prompt_queue_keys_t *keys) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || !sl_prompt_queue_keys_valid(keys)) {
    sl_set_error(self, "prompt queue keys must be non-duplicate actions");
    return SL_ERROR_INVALID;
  }
  impl->prompt_queue.keys = *keys;
  return SL_OK;
}

static int sl_get_prompt_queue_keys_method(const sl_t *self,
                                           sl_prompt_queue_keys_t *out) {
  const sl_impl_t *impl;
  impl = sl_impl((sl_t *)self);
  if (!impl || !out) {
    sl_set_error((sl_t *)self, "invalid prompt queue keys output");
    return SL_ERROR_INVALID;
  }
  *out = impl->prompt_queue.keys;
  return SL_OK;
}

static int sl_set_prompt_theme_method(sl_t *self, sl_prompt_theme_t theme) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || theme < SL_PROMPT_THEME_PLAIN ||
      theme > SL_PROMPT_THEME_DEFAULT) {
    sl_set_error(self, "invalid prompt theme");
    return SL_ERROR_INVALID;
  }
  impl->prompt_theme = theme;
  return SL_OK;
}

static int sl_set_quoted_prompt_prefix_method(sl_t *self, const char *prefix) {
  sl_impl_t *impl = sl_impl(self);
  char *copy = NULL;
  if (!impl ||
      (prefix && (prefix[0] == '\0' || !sl_statusline_text_valid(prefix)))) {
    sl_set_error(
        self,
        "quoted prompt prefix must be nonempty printable single-line UTF-8");
    return SL_ERROR_INVALID;
  }
  if (prefix) {
    copy = strdup(prefix);
    if (!copy) {
      sl_set_error(self, "failed to allocate quoted prompt prefix");
      return SL_ERROR;
    }
  }
  free(impl->quoted_prompt_prefix);
  impl->quoted_prompt_prefix = copy;
  return SL_OK;
}

static int sl_set_quoted_prompt_style_method(sl_t *self,
                                             const sl_quote_style_t *style) {
  sl_impl_t *impl = sl_impl(self);
  if (!impl)
    return SL_ERROR_INVALID;
  if (style) {
    impl->quoted_prompt_style = *style;
    impl->quoted_prompt_style_custom = 1;
  } else {
    memset(&impl->quoted_prompt_style, 0, sizeof(impl->quoted_prompt_style));
    impl->quoted_prompt_style_custom = 0;
  }
  return SL_OK;
}

static int sl_set_statusline_method(sl_t *self, int enabled,
                                    size_t starting_element) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || enabled < 0) {
    sl_set_error(self, "invalid status line configuration");
    return SL_ERROR_INVALID;
  }
  impl->statusline.enabled = enabled;
  impl->statusline.start_element = starting_element;
  return SL_OK;
}

static int sl_set_status_message_method(sl_t *self, const char *message) {
  sl_impl_t *impl;
  char *copy;
  impl = sl_impl(self);
  if (!impl || !sl_statusline_text_valid(message)) {
    sl_set_error(self, "status message must be printable single-line UTF-8");
    return SL_ERROR_INVALID;
  }
  copy = message && message[0] ? sl_strdup(message) : NULL;
  if (message && message[0] && !copy) {
    sl_set_error(self, "failed to allocate status message");
    return SL_ERROR_NOMEM;
  }
  free(impl->status_message);
  impl->status_message = copy;
  if (impl->active_prompt && sl_render_apply(self, impl->active_prompt) != 0)
    return SL_ERROR_IO;
  return SL_OK;
}

static int sl_set_status_message_prefix_method(sl_t *self, const char *prefix) {
  sl_impl_t *impl;
  char *copy;
  impl = sl_impl(self);
  if (!impl || !sl_statusline_text_valid(prefix)) {
    sl_set_error(self,
                 "status message prefix must be printable single-line UTF-8");
    return SL_ERROR_INVALID;
  }
  copy = prefix ? sl_strdup(prefix) : NULL;
  if (prefix && !copy) {
    sl_set_error(self, "failed to allocate status message prefix");
    return SL_ERROR_NOMEM;
  }
  free(impl->status_message_prefix);
  impl->status_message_prefix = copy;
  if (impl->active_prompt && sl_render_apply(self, impl->active_prompt) != 0)
    return SL_ERROR_IO;
  return SL_OK;
}

static int sl_set_status_message_colors_method(sl_t *self,
                                               sl_theme_color_t prefix_color,
                                               sl_theme_color_t text_color) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || prefix_color < SL_THEME_COLOR_MUTED ||
      prefix_color > SL_THEME_COLOR_ELEMENT_7 ||
      text_color < SL_THEME_COLOR_MUTED ||
      text_color > SL_THEME_COLOR_ELEMENT_7) {
    sl_set_error(self, "invalid status message theme color");
    return SL_ERROR_INVALID;
  }
  impl->status_message_prefix_color = prefix_color;
  impl->status_message_text_color = text_color;
  if (impl->active_prompt && sl_render_apply(self, impl->active_prompt) != 0)
    return SL_ERROR_IO;
  return SL_OK;
}

static int sl_set_status_elements_method(sl_t *self,
                                         const char *const *elements,
                                         size_t count) {
  char *copies[SL_STATUS_MAX_ELEMENTS];
  sl_impl_t *impl;
  size_t i;
  size_t limit;
  int result;
  int truncated;
  memset(copies, 0, sizeof(copies));
  impl = sl_impl(self);
  if (!impl || (count > 0 && !elements)) {
    sl_set_error(self, "invalid status line elements");
    return SL_ERROR_INVALID;
  }
  truncated = count > SL_STATUS_MAX_ELEMENTS;
  result = SL_ERROR_NOMEM;
  limit = truncated ? SL_STATUS_MAX_ELEMENTS - 1 : count;
  for (i = 0; i < limit; i++) {
    if (!sl_statusline_text_valid(elements[i])) {
      sl_set_error(self, "status line element contains control characters");
      result = SL_ERROR_INVALID;
      goto fail;
    }
    if (elements[i]) {
      copies[i] = sl_strdup(elements[i]);
      if (!copies[i]) {
        sl_set_error(self, "failed to allocate status line element");
        goto fail;
      }
    }
  }
  if (truncated) {
    copies[SL_STATUS_MAX_ELEMENTS - 1] = sl_strdup("...");
    if (!copies[SL_STATUS_MAX_ELEMENTS - 1]) {
      sl_set_error(self, "failed to allocate status line truncation");
      goto fail;
    }
    limit = SL_STATUS_MAX_ELEMENTS;
  }
  sl_statusline_clear(&impl->statusline);
  for (i = 0; i < limit; i++) {
    impl->statusline.elements[i] = copies[i];
    copies[i] = NULL;
  }
  impl->statusline.count = limit;
  impl->statusline.truncated = truncated;
  return SL_OK;

fail:
  for (i = 0; i < SL_STATUS_MAX_ELEMENTS; i++)
    free(copies[i]);
  return result;
}

static int sl_set_status_element_method(sl_t *self, size_t index,
                                        const char *element) {
  char *copy;
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || index >= SL_STATUS_MAX_ELEMENTS ||
      !sl_statusline_text_valid(element)) {
    sl_set_error(self, "invalid status line element");
    return SL_ERROR_INVALID;
  }
  copy = element ? sl_strdup(element) : NULL;
  if (element && !copy) {
    sl_set_error(self, "failed to allocate status line element");
    return SL_ERROR_NOMEM;
  }
  free(impl->statusline.elements[index]);
  impl->statusline.elements[index] = copy;
  if (copy && index + 1 > impl->statusline.count)
    impl->statusline.count = index + 1;
  while (impl->statusline.count > 0 &&
         !impl->statusline.elements[impl->statusline.count - 1])
    impl->statusline.count--;
  if (index == SL_STATUS_MAX_ELEMENTS - 1 && !copy)
    impl->statusline.truncated = 0;
  return SL_OK;
}

static int sl_set_status_busy_method(sl_t *self, int busy) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || busy < 0) {
    sl_set_error(self, "invalid status busy state");
    return SL_ERROR_INVALID;
  }
  impl->statusline.busy = busy;
  impl->statusline.spinner_time_valid = 0;
  sl_prompt_queue_sync_queued_turns_delivery(impl);
  return SL_OK;
}

static int sl_set_status_spinner_method(sl_t *self, int enabled) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl || enabled < 0) {
    sl_set_error(self, "invalid status spinner configuration");
    return SL_ERROR_INVALID;
  }
  impl->statusline.spinner = enabled;
  impl->statusline.spinner_frame = 0;
  impl->statusline.spinner_time_valid = 0;
  return SL_OK;
}

static int sl_set_status_idle_marker_method(sl_t *self, char marker) {
  sl_impl_t *impl;
  unsigned char value;
  impl = sl_impl(self);
  value = (unsigned char)marker;
  if (!impl || (marker != '\0' && (value < 0x20 || value > 0x7e))) {
    sl_set_error(self, "invalid status idle marker");
    return SL_ERROR_INVALID;
  }
  impl->statusline.idle_marker = marker;
  return SL_OK;
}

static int sl_set_idle_callback_method(sl_t *self, sl_idle_callback_t callback,
                                       void *userdata) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl)
    return SL_ERROR_INVALID;
  impl->idle_callback = callback;
  impl->idle_userdata = userdata;
  return SL_OK;
}

static int sl_watch_events_valid(unsigned int events) {
  unsigned int allowed;
  allowed = SL_WATCH_READ | SL_WATCH_WRITE | SL_WATCH_ERROR | SL_WATCH_HANGUP;
  return events != 0 && (events & ~allowed) == 0;
}

static int sl_watch_add_method(sl_t *self, int fd, unsigned int events,
                               sl_watch_callback_t callback, void *userdata,
                               sl_watch_id_t *out_id) {
  sl_impl_t *impl;
  sl_watch_id_t id;
  int i;
  if (out_id)
    *out_id = 0;
  impl = sl_impl(self);
  if (!impl || !out_id || fd < 0 || !callback ||
      !sl_watch_events_valid(events) || !isatty(impl->input_fd) ||
      !isatty(impl->output_fd)) {
    sl_set_error(self, "external watches require an interactive TTY handle");
    return SL_ERROR_INVALID;
  }
  for (i = 0; i < SL_MAX_WATCHES; i++) {
    if (impl->watches[i].id == 0)
      break;
  }
  if (i == SL_MAX_WATCHES) {
    sl_set_error(self, "too many external event watches");
    return SL_ERROR_FULL;
  }
  id = impl->next_watch_id;
  do {
    id++;
    if (id == 0)
      id++;
  } while (sl_watch_find(impl, id));
  impl->next_watch_id = id;
  impl->watches[i].id = id;
  impl->watches[i].fd = fd;
  impl->watches[i].events = events;
  impl->watches[i].callback = callback;
  impl->watches[i].userdata = userdata;
  *out_id = id;
  return SL_OK;
}

static int sl_watch_modify_method(sl_t *self, sl_watch_id_t id,
                                  unsigned int events) {
  sl_impl_t *impl;
  sl_watch_t *watch;
  impl = sl_impl(self);
  if (!impl || !sl_watch_events_valid(events)) {
    sl_set_error(self, "invalid external event watch conditions");
    return SL_ERROR_INVALID;
  }
  watch = sl_watch_find(impl, id);
  if (!watch) {
    sl_set_error(self, "external event watch was not found");
    return SL_ERROR_INVALID;
  }
  watch->events = events;
  return SL_OK;
}

static int sl_watch_remove_method(sl_t *self, sl_watch_id_t id) {
  sl_impl_t *impl;
  sl_watch_t *watch;
  impl = sl_impl(self);
  if (!impl)
    return SL_ERROR_INVALID;
  watch = sl_watch_find(impl, id);
  if (!watch) {
    sl_set_error(self, "external event watch was not found");
    return SL_ERROR_INVALID;
  }
  memset(watch, 0, sizeof(*watch));
  return SL_OK;
}

static int sl_watch_clear_method(sl_t *self) {
  sl_impl_t *impl;
  impl = sl_impl(self);
  if (!impl)
    return SL_ERROR_INVALID;
  memset(impl->watches, 0, sizeof(impl->watches));
  return SL_OK;
}

static const char *sl_last_error_method(const sl_t *self) {
  const sl_impl_t *impl;
  impl = sl_impl_const(self);
  if (!impl || impl->error[0] == '\0')
    return NULL;
  return impl->error;
}

static sl_readline_status_t sl_last_readline_status_method(const sl_t *self) {
  const sl_impl_t *impl;
  impl = sl_impl_const(self);
  if (!impl)
    return SL_READLINE_NONE;
  return impl->last_readline_status;
}

void sl_config_init(sl_config_t *config) {
  if (!config)
    return;
  config->input_fd = STDIN_FILENO;
  config->output_fd = STDOUT_FILENO;
  config->screen_width = 0;
  config->live_scroll_region = 0;
  config->history_max_len = SL_HISTORY_DEFAULT_MAX;
  config->line_max_len = SL_LINE_DEFAULT_MAX;
  config->prompt_queue = 0;
  config->prompt_queue_max_entries = SL_PROMPT_QUEUE_DEFAULT_MAX;
  config->prompt_queue_preview_entries = SL_PROMPT_QUEUE_DEFAULT_PREVIEWS;
  config->prompt_theme = SL_PROMPT_THEME_DEFAULT;
  config->statusline = 0;
  config->statusline_start_element = 0;
  config->status_spinner = 0;
  config->status_busy = 0;
  config->status_idle_marker = '+';
  config->clear_prompt_on_exit = 0;
  config->disable_image_paste = 0;
  config->image_paste_path_template = NULL;
  config->queue_backend = NULL;
  config->queue_backend_context = NULL;
}

static sl_t *sl_create_with_config_impl(const sl_config_t *config) {
  sl_config_t local;
  sl_t *self;
  sl_impl_t *impl;
  if (!config) {
    sl_config_init(&local);
    config = &local;
  }
  if (config->history_max_len < 0 || config->screen_width < 0 ||
      (!config->queue_backend && config->queue_backend_context) ||
      config->live_scroll_region < 0 || config->clear_prompt_on_exit < 0 ||
      config->disable_image_paste < 0 ||
      !sl_clipboard_valid_path_template(config->image_paste_path_template) ||
      config->prompt_queue < 0 || config->prompt_queue_max_entries < 1 ||
      config->prompt_queue_preview_entries < 1 ||
      config->prompt_theme < SL_PROMPT_THEME_PLAIN ||
      config->prompt_theme > SL_PROMPT_THEME_DEFAULT ||
      config->statusline < 0 || config->status_spinner < 0 ||
      config->status_busy < 0 ||
      (config->status_idle_marker != '\0' &&
       ((unsigned char)config->status_idle_marker < 0x20 ||
        (unsigned char)config->status_idle_marker > 0x7e)) ||
      config->line_max_len == 0 || config->line_max_len > (size_t)INT_MAX - 2)
    return NULL;
  self = (sl_t *)calloc(1, sizeof(*self));
  impl = (sl_impl_t *)calloc(1, sizeof(*impl));
  if (!self || !impl) {
    free(self);
    free(impl);
    return NULL;
  }
  self->readline = sl_readline_method;
  self->destroy = sl_destroy_method;
  self->free_string = sl_free_string_method;
  self->history_add = sl_history_add_impl;
  self->history_set_max_len = sl_history_set_max;
  self->history_save = sl_history_save_impl;
  self->history_load = sl_history_load_impl;
  self->history_set_backend = sl_history_set_backend_impl;
  self->history_open = sl_history_open_impl;
  self->history_close = sl_history_close_impl;
  self->history_compact = sl_history_compact_impl;
  self->history_set_auto_add = sl_history_set_auto_add_impl;
  self->history_search_active = sl_history_search_active_impl;
  self->set_screen_width = sl_set_screen_width_method;
  self->set_live_scroll_region = sl_set_live_scroll_region_method;
  self->set_image_paste_path_template = sl_set_image_paste_path_template_method;
  self->set_idle_callback = sl_set_idle_callback_method;
  self->watch_add = sl_watch_add_method;
  self->watch_modify = sl_watch_modify_method;
  self->watch_remove = sl_watch_remove_method;
  self->watch_clear = sl_watch_clear_method;
  self->output_stream_begin = sl_output_stream_begin_method;
  self->output_stream_reconcile = sl_output_stream_reconcile_method;
  self->output_stream_write = sl_output_stream_write_method;
  self->output_stream_end = sl_output_stream_end_method;
  self->output_stream_write_quoted_prompt =
      sl_output_stream_write_quoted_prompt_method;
  self->set_quoted_prompt_prefix = sl_set_quoted_prompt_prefix_method;
  self->set_quoted_prompt_style = sl_set_quoted_prompt_style_method;
  self->set_status_message_prefix = sl_set_status_message_prefix_method;
  self->set_status_message_colors = sl_set_status_message_colors_method;
  self->bind_key = sl_bind_key_method;
  self->insert = sl_buf_insert_cstr;
  self->set_buffer = sl_buf_set_public;
  self->buffer = sl_buffer_method;
  self->cursor = sl_cursor_method;
  self->set_cursor = sl_set_cursor_method;
  self->submit = sl_submit_method;
  self->cancel = sl_cancel_method;
  self->suspend = sl_suspend_method;
  self->print_above = sl_print_above_method;
  self->last_readline_status = sl_last_readline_status_method;
  self->last_error = sl_last_error_method;
  self->impl = impl;
  self->next_prompt = sl_next_prompt_method;
  self->set_prompt_queue = sl_set_prompt_queue_method;
  self->set_prompt_theme = sl_set_prompt_theme_method;
  self->set_statusline = sl_set_statusline_method;
  self->set_status_message = sl_set_status_message_method;
  impl->status_message_prefix_color = SL_THEME_COLOR_MUTED;
  impl->status_message_text_color = SL_THEME_COLOR_SECONDARY;
  self->prompt_queue_get_mode = sl_prompt_queue_get_mode_method;
  self->prompt_queue_set_mode = sl_prompt_queue_set_mode_method;
  self->set_status_elements = sl_set_status_elements_method;
  self->set_status_element = sl_set_status_element_method;
  self->set_status_busy = sl_set_status_busy_method;
  self->set_status_spinner = sl_set_status_spinner_method;
  self->set_status_idle_marker = sl_set_status_idle_marker_method;
  self->prompt_queue_count = sl_prompt_queue_count_method;
  self->prompt_queue_capacity = sl_prompt_queue_capacity_method;
  self->prompt_queue_peek = sl_prompt_queue_peek_method;
  self->prompt_queue_insert = sl_prompt_queue_insert_method;
  self->prompt_queue_append = sl_prompt_queue_append_method;
  self->inject_queued_prompt = sl_inject_queued_prompt_method;
  self->inject_steer_prompt = sl_inject_steer_prompt_method;
  self->set_auto_approval = sl_set_auto_approval_method;
  self->get_auto_approval = sl_get_auto_approval_method;
  self->prompt_queue_replace = sl_prompt_queue_replace_method;
  self->prompt_queue_take = sl_prompt_queue_take_method;
  self->prompt_queue_clear = sl_prompt_queue_clear_method;
  self->prompt_queue_enqueue_draft = sl_prompt_queue_enqueue_draft_method;
  self->prompt_queue_info = sl_prompt_queue_info_method;
  self->prompt_queue_draft_info = sl_prompt_queue_draft_info_method;
  self->prompt_queue_approve = sl_prompt_queue_approve_method;
  self->prompt_queue_reject = sl_prompt_queue_reject_method;
  self->last_prompt_id = sl_last_prompt_id_method;
  self->set_prompt_queue_delivery = sl_set_prompt_queue_delivery_method;
  self->get_prompt_queue_delivery = sl_get_prompt_queue_delivery_method;
  self->set_prompt_queue_profile = sl_set_prompt_queue_profile_method;
  self->get_prompt_queue_profile = sl_get_prompt_queue_profile_method;
  self->set_prompt_queue_keys = sl_set_prompt_queue_keys_method;
  self->get_prompt_queue_keys = sl_get_prompt_queue_keys_method;
  self->set_prompt_queue_policy = sl_set_prompt_queue_policy_method;
  self->prompt_queue_set_executable = sl_prompt_queue_set_executable_method;
  self->get_prompt_queue_stopped = sl_get_prompt_queue_stopped_method;
  self->prompt_queue_resume = sl_prompt_queue_resume_method;
  self->queue_backend_request = sl_queue_backend_request_impl;
  self->queue_backend_result = sl_queue_backend_result_impl;
  self->queue_backend_update = sl_queue_backend_update_impl;
  self->queue_backend_info = sl_queue_backend_info_impl;
  self->queue_backend_entry = sl_queue_backend_entry_impl;
  self->queue_backend_pending = sl_queue_backend_pending_impl;
  self->queue_backend_recover = sl_queue_backend_recover_impl;
  impl->input_fd = config->input_fd >= 0 ? config->input_fd : STDIN_FILENO;
  impl->output_fd = config->output_fd >= 0 ? config->output_fd : STDOUT_FILENO;
  impl->screen_width = config->screen_width;
  impl->line_max_len = config->line_max_len;
  impl->live_scroll_region = config->live_scroll_region;
  impl->clear_prompt_on_exit = config->clear_prompt_on_exit;
  impl->disable_image_paste = config->disable_image_paste;
  if (config->image_paste_path_template) {
    impl->image_paste_path_template = strdup(config->image_paste_path_template);
    if (!impl->image_paste_path_template) {
      sl_destroy_method(self);
      return NULL;
    }
  }
  impl->prompt_queue.enabled = config->prompt_queue;
  impl->prompt_queue.max_entries = config->prompt_queue_max_entries;
  impl->prompt_queue.preview_entries = config->prompt_queue_preview_entries;
  impl->prompt_queue.delivery = SL_PROMPT_QUEUE_DELIVERY_AUTO;
  impl->prompt_queue.profile = SL_PROMPT_QUEUE_PROFILE_DEFAULT;
  sl_prompt_queue_profile_keys(impl->prompt_queue.profile,
                               &impl->prompt_queue.keys);
  impl->prompt_theme = config->prompt_theme;
  impl->statusline.enabled = config->statusline;
  impl->statusline.start_element = config->statusline_start_element;
  impl->statusline.spinner = config->status_spinner;
  impl->statusline.busy = config->status_busy;
  impl->statusline.idle_marker = config->status_idle_marker;
  impl->history.max_len = config->history_max_len;
  impl->history_index = -1;
  if (sl_buf_reserve(self, 1) != 0) {
    sl_destroy_method(self);
    return NULL;
  }
  impl->buf[0] = '\0';
  if (sl_backend_create(self, config) != SL_OK) {
    sl_destroy_method(self);
    return NULL;
  }
  return self;
}

sl_t *sl_create(void) {
  sl_config_t config;
  sl_config_init(&config);
  return sl_create_with_config_impl(&config);
}

sl_t *sl_create_with_config(const sl_config_t *config) {
  return sl_create_with_config_impl(config);
}

char *sl_readline(sl_t *self, const char *prompt) {
  if (!self || !self->readline)
    return NULL;
  return self->readline(self, prompt);
}

char *sl_next_prompt(sl_t *self, const char *prompt,
                     sl_prompt_source_t *source) {
  if (source)
    *source = SL_PROMPT_SOURCE_NONE;
  if (!self || !self->next_prompt)
    return NULL;
  return self->next_prompt(self, prompt, source);
}

void sl_destroy(sl_t *self) {
  if (self && self->destroy)
    self->destroy(self);
}

void sl_free_string(sl_t *self, char *ptr) {
  if (self && self->free_string)
    self->free_string(self, ptr);
  else
    free(ptr);
}

int sl_history_add(sl_t *self, const char *line) {
  if (!self || !self->history_add)
    return SL_ERROR_INVALID;
  return self->history_add(self, line);
}

int sl_history_set_max_len(sl_t *self, int max_len) {
  if (!self || !self->history_set_max_len)
    return SL_ERROR_INVALID;
  return self->history_set_max_len(self, max_len);
}

int sl_history_save(sl_t *self, const char *filename) {
  if (!self || !self->history_save)
    return SL_ERROR_INVALID;
  return self->history_save(self, filename);
}

int sl_history_load(sl_t *self, const char *filename) {
  if (!self || !self->history_load)
    return SL_ERROR_INVALID;
  return self->history_load(self, filename);
}

int sl_history_set_backend(sl_t *self, const char *key,
                           sl_history_load_callback_t load,
                           sl_history_append_callback_t append,
                           void *userdata) {
  if (!self || !self->history_set_backend)
    return SL_ERROR_INVALID;
  return self->history_set_backend(self, key, load, append, userdata);
}

int sl_history_open(sl_t *self, const char *key, const char *directory) {
  if (!self || !self->history_open)
    return SL_ERROR_INVALID;
  return self->history_open(self, key, directory);
}

int sl_history_close(sl_t *self) {
  if (!self || !self->history_close)
    return SL_ERROR_INVALID;
  return self->history_close(self);
}

int sl_history_compact(sl_t *self) {
  if (!self || !self->history_compact)
    return SL_ERROR_INVALID;
  return self->history_compact(self);
}

int sl_history_set_auto_add(sl_t *self, int enabled) {
  if (!self || !self->history_set_auto_add)
    return SL_ERROR_INVALID;
  return self->history_set_auto_add(self, enabled);
}

int sl_set_screen_width(sl_t *self, int width) {
  if (!self || !self->set_screen_width)
    return SL_ERROR_INVALID;
  return self->set_screen_width(self, width);
}

int sl_set_live_scroll_region(sl_t *self, int enabled) {
  if (!self || !self->set_live_scroll_region)
    return SL_ERROR_INVALID;
  return self->set_live_scroll_region(self, enabled);
}

int sl_set_image_paste_path_template(sl_t *self, const char *path_template) {
  if (!self || !self->set_image_paste_path_template)
    return SL_ERROR_INVALID;
  return self->set_image_paste_path_template(self, path_template);
}

int sl_set_prompt_queue(sl_t *self, int enabled, int max_entries,
                        int preview_entries) {
  if (!self || !self->set_prompt_queue)
    return SL_ERROR_INVALID;
  return self->set_prompt_queue(self, enabled, max_entries, preview_entries);
}

size_t sl_prompt_queue_count(const sl_t *self) {
  if (!self || !self->prompt_queue_count)
    return 0;
  return self->prompt_queue_count(self);
}

size_t sl_prompt_queue_capacity(const sl_t *self) {
  if (!self || !self->prompt_queue_capacity)
    return 0;
  return self->prompt_queue_capacity(self);
}

int sl_prompt_queue_peek(const sl_t *self, size_t index, char **out) {
  if (out)
    *out = NULL;
  if (!self || !self->prompt_queue_peek)
    return SL_ERROR_INVALID;
  return self->prompt_queue_peek(self, index, out);
}

int sl_prompt_queue_insert(sl_t *self, size_t index, const char *text, int held,
                           sl_prompt_id_t *out_id) {
  if (out_id)
    *out_id = 0;
  if (!self || !self->prompt_queue_insert)
    return SL_ERROR_INVALID;
  return self->prompt_queue_insert(self, index, text, held, out_id);
}

int sl_prompt_queue_append(sl_t *self, const char *text, int held,
                           sl_prompt_id_t *out_id) {
  if (out_id)
    *out_id = 0;
  if (!self || !self->prompt_queue_append)
    return SL_ERROR_INVALID;
  return self->prompt_queue_append(self, text, held, out_id);
}

int sl_inject_queued_prompt(sl_t *self, const char *text,
                            sl_prompt_id_t *out_id) {
  if (out_id)
    *out_id = 0;
  if (!self || !self->inject_queued_prompt)
    return SL_ERROR_INVALID;
  return self->inject_queued_prompt(self, text, out_id);
}

int sl_inject_steer_prompt(sl_t *self, const char *text,
                           sl_prompt_id_t *out_id) {
  if (out_id)
    *out_id = 0;
  if (!self || !self->inject_steer_prompt)
    return SL_ERROR_INVALID;
  return self->inject_steer_prompt(self, text, out_id);
}

int sl_set_auto_approval(sl_t *self, int enabled) {
  if (!self || !self->set_auto_approval)
    return SL_ERROR_INVALID;
  return self->set_auto_approval(self, enabled);
}

int sl_get_auto_approval(const sl_t *self, int *out) {
  if (!self || !self->get_auto_approval)
    return SL_ERROR_INVALID;
  return self->get_auto_approval(self, out);
}

int sl_prompt_queue_info(const sl_t *self, size_t index,
                         sl_prompt_queue_entry_info_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!self || !self->prompt_queue_info)
    return SL_ERROR_INVALID;
  return self->prompt_queue_info(self, index, out);
}

int sl_prompt_queue_draft_info(const sl_t *self,
                               sl_prompt_queue_entry_info_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!self || !self->prompt_queue_draft_info)
    return SL_ERROR_INVALID;
  return self->prompt_queue_draft_info(self, out);
}

int sl_prompt_queue_approve(sl_t *self, sl_prompt_id_t id) {
  if (!self || !self->prompt_queue_approve)
    return SL_ERROR_INVALID;
  return self->prompt_queue_approve(self, id);
}

int sl_prompt_queue_reject(sl_t *self, sl_prompt_id_t id) {
  if (!self || !self->prompt_queue_reject)
    return SL_ERROR_INVALID;
  return self->prompt_queue_reject(self, id);
}

sl_prompt_id_t sl_last_prompt_id(const sl_t *self) {
  if (!self || !self->last_prompt_id)
    return 0;
  return self->last_prompt_id(self);
}

int sl_prompt_queue_replace(sl_t *self, size_t index, const char *text) {
  if (!self || !self->prompt_queue_replace)
    return SL_ERROR_INVALID;
  return self->prompt_queue_replace(self, index, text);
}

int sl_prompt_queue_take(sl_t *self, size_t index, char **out) {
  if (out)
    *out = NULL;
  if (!self || !self->prompt_queue_take)
    return SL_ERROR_INVALID;
  return self->prompt_queue_take(self, index, out);
}

int sl_prompt_queue_get_mode(const sl_t *self, size_t index,
                             sl_prompt_queue_mode_t *out) {
  if (!self || !self->prompt_queue_get_mode)
    return SL_ERROR_INVALID;
  return self->prompt_queue_get_mode(self, index, out);
}

int sl_prompt_queue_set_mode(sl_t *self, size_t index,
                             sl_prompt_queue_mode_t mode) {
  if (!self || !self->prompt_queue_set_mode)
    return SL_ERROR_INVALID;
  return self->prompt_queue_set_mode(self, index, mode);
}

int sl_prompt_queue_clear(sl_t *self) {
  if (!self || !self->prompt_queue_clear)
    return SL_ERROR_INVALID;
  return self->prompt_queue_clear(self);
}

int sl_prompt_queue_enqueue_draft(sl_t *self) {
  if (!self || !self->prompt_queue_enqueue_draft)
    return SL_ERROR_INVALID;
  return self->prompt_queue_enqueue_draft(self);
}

int sl_set_prompt_queue_delivery(sl_t *self,
                                 sl_prompt_queue_delivery_t delivery) {
  if (!self || !self->set_prompt_queue_delivery)
    return SL_ERROR_INVALID;
  return self->set_prompt_queue_delivery(self, delivery);
}

int sl_get_prompt_queue_delivery(const sl_t *self,
                                 sl_prompt_queue_delivery_t *out) {
  if (!self || !self->get_prompt_queue_delivery)
    return SL_ERROR_INVALID;
  return self->get_prompt_queue_delivery(self, out);
}

int sl_set_prompt_queue_profile(sl_t *self, sl_prompt_queue_profile_t profile) {
  if (!self || !self->set_prompt_queue_profile)
    return SL_ERROR_INVALID;
  return self->set_prompt_queue_profile(self, profile);
}

int sl_get_prompt_queue_profile(const sl_t *self,
                                sl_prompt_queue_profile_t *out) {
  if (!self || !self->get_prompt_queue_profile)
    return SL_ERROR_INVALID;
  return self->get_prompt_queue_profile(self, out);
}

int sl_set_prompt_queue_keys(sl_t *self, const sl_prompt_queue_keys_t *keys) {
  if (!self || !self->set_prompt_queue_keys)
    return SL_ERROR_INVALID;
  return self->set_prompt_queue_keys(self, keys);
}

int sl_get_prompt_queue_keys(const sl_t *self, sl_prompt_queue_keys_t *out) {
  if (!self || !self->get_prompt_queue_keys)
    return SL_ERROR_INVALID;
  return self->get_prompt_queue_keys(self, out);
}

int sl_set_prompt_queue_policy(sl_t *self,
                               sl_prompt_queue_policy_callback_t callback,
                               void *userdata) {
  if (!self || !self->set_prompt_queue_policy)
    return SL_ERROR_INVALID;
  return self->set_prompt_queue_policy(self, callback, userdata);
}

int sl_prompt_queue_set_executable(sl_t *self, sl_prompt_id_t id,
                                   int executable) {
  if (!self || !self->prompt_queue_set_executable)
    return SL_ERROR_INVALID;
  return self->prompt_queue_set_executable(self, id, executable);
}

int sl_get_prompt_queue_stopped(const sl_t *self, int *out) {
  if (!self || !self->get_prompt_queue_stopped)
    return SL_ERROR_INVALID;
  return self->get_prompt_queue_stopped(self, out);
}

int sl_prompt_queue_resume(sl_t *self) {
  if (!self || !self->prompt_queue_resume)
    return SL_ERROR_INVALID;
  return self->prompt_queue_resume(self);
}

int sl_set_prompt_theme(sl_t *self, sl_prompt_theme_t theme) {
  if (!self || !self->set_prompt_theme)
    return SL_ERROR_INVALID;
  return self->set_prompt_theme(self, theme);
}

int sl_set_statusline(sl_t *self, int enabled, size_t starting_element) {
  if (!self || !self->set_statusline)
    return SL_ERROR_INVALID;
  return self->set_statusline(self, enabled, starting_element);
}

int sl_set_status_message(sl_t *self, const char *message) {
  if (!self || !self->set_status_message)
    return SL_ERROR_INVALID;
  return self->set_status_message(self, message);
}

int sl_set_status_message_prefix(sl_t *self, const char *prefix) {
  if (!self || !self->set_status_message_prefix)
    return SL_ERROR_INVALID;
  return self->set_status_message_prefix(self, prefix);
}

int sl_set_status_message_colors(sl_t *self, sl_theme_color_t prefix_color,
                                 sl_theme_color_t text_color) {
  if (!self || !self->set_status_message_colors)
    return SL_ERROR_INVALID;
  return self->set_status_message_colors(self, prefix_color, text_color);
}

int sl_set_status_elements(sl_t *self, const char *const *elements,
                           size_t count) {
  if (!self || !self->set_status_elements)
    return SL_ERROR_INVALID;
  return self->set_status_elements(self, elements, count);
}

int sl_set_status_element(sl_t *self, size_t index, const char *element) {
  if (!self || !self->set_status_element)
    return SL_ERROR_INVALID;
  return self->set_status_element(self, index, element);
}

int sl_set_status_busy(sl_t *self, int busy) {
  if (!self || !self->set_status_busy)
    return SL_ERROR_INVALID;
  return self->set_status_busy(self, busy);
}

int sl_set_status_spinner(sl_t *self, int enabled) {
  if (!self || !self->set_status_spinner)
    return SL_ERROR_INVALID;
  return self->set_status_spinner(self, enabled);
}

int sl_set_status_idle_marker(sl_t *self, char marker) {
  if (!self || !self->set_status_idle_marker)
    return SL_ERROR_INVALID;
  return self->set_status_idle_marker(self, marker);
}

int sl_set_idle_callback(sl_t *self, sl_idle_callback_t callback,
                         void *userdata) {
  if (!self || !self->set_idle_callback)
    return SL_ERROR_INVALID;
  return self->set_idle_callback(self, callback, userdata);
}

int sl_watch_add(sl_t *self, int fd, unsigned int events,
                 sl_watch_callback_t callback, void *userdata,
                 sl_watch_id_t *out_id) {
  if (out_id)
    *out_id = 0;
  if (!self || !self->watch_add)
    return SL_ERROR_INVALID;
  return self->watch_add(self, fd, events, callback, userdata, out_id);
}

int sl_watch_modify(sl_t *self, sl_watch_id_t id, unsigned int events) {
  if (!self || !self->watch_modify)
    return SL_ERROR_INVALID;
  return self->watch_modify(self, id, events);
}

int sl_watch_remove(sl_t *self, sl_watch_id_t id) {
  if (!self || !self->watch_remove)
    return SL_ERROR_INVALID;
  return self->watch_remove(self, id);
}

int sl_watch_clear(sl_t *self) {
  if (!self || !self->watch_clear)
    return SL_ERROR_INVALID;
  return self->watch_clear(self);
}

int sl_bind_key(sl_t *self, sl_key_t key, sl_key_callback_t callback,
                void *userdata) {
  if (!self || !self->bind_key)
    return SL_ERROR_INVALID;
  return self->bind_key(self, key, callback, userdata);
}

int sl_history_search_active(const sl_t *self) {
  if (!self || !self->history_search_active)
    return 0;
  return self->history_search_active(self);
}

int sl_insert(sl_t *self, const char *text) {
  if (!self || !self->insert)
    return SL_ERROR_INVALID;
  return self->insert(self, text);
}

int sl_set_buffer(sl_t *self, const char *text) {
  if (!self || !self->set_buffer)
    return SL_ERROR_INVALID;
  return self->set_buffer(self, text);
}

const char *sl_buffer(const sl_t *self) {
  if (!self || !self->buffer)
    return NULL;
  return self->buffer(self);
}

size_t sl_cursor(const sl_t *self) {
  if (!self || !self->cursor)
    return 0;
  return self->cursor(self);
}

int sl_set_cursor(sl_t *self, size_t cursor) {
  if (!self || !self->set_cursor)
    return SL_ERROR_INVALID;
  return self->set_cursor(self, cursor);
}

int sl_submit(sl_t *self) {
  if (!self || !self->submit)
    return SL_ERROR_INVALID;
  return self->submit(self);
}

int sl_cancel(sl_t *self) {
  if (!self || !self->cancel)
    return SL_ERROR_INVALID;
  return self->cancel(self);
}

int sl_suspend(sl_t *self) {
  return self && self->suspend ? self->suspend(self) : SL_ERROR_INVALID;
}

int sl_print_above(sl_t *self, sl_stream_callback_t callback, void *userdata) {
  if (!self || !self->print_above)
    return SL_ERROR_INVALID;
  return self->print_above(self, callback, userdata);
}

int sl_output_stream_begin(sl_t *self) {
  if (!self || !self->output_stream_begin)
    return SL_ERROR_INVALID;
  return self->output_stream_begin(self);
}

int sl_output_stream_reconcile(sl_t *self) {
  if (!self || !self->output_stream_reconcile)
    return SL_ERROR_INVALID;
  return self->output_stream_reconcile(self);
}

int sl_output_stream_write(sl_t *self, const char *bytes, size_t length) {
  if (!self || !self->output_stream_write)
    return SL_ERROR_INVALID;
  return self->output_stream_write(self, bytes, length);
}

int sl_output_stream_end(sl_t *self) {
  if (!self || !self->output_stream_end)
    return SL_ERROR_INVALID;
  return self->output_stream_end(self);
}

int sl_output_stream_write_quoted_prompt(sl_t *self, const char *text) {
  if (!self || !self->output_stream_write_quoted_prompt)
    return SL_ERROR_INVALID;
  return self->output_stream_write_quoted_prompt(self, text);
}

int sl_set_quoted_prompt_prefix(sl_t *self, const char *prefix) {
  if (!self || !self->set_quoted_prompt_prefix)
    return SL_ERROR_INVALID;
  return self->set_quoted_prompt_prefix(self, prefix);
}

int sl_set_quoted_prompt_style(sl_t *self, const sl_quote_style_t *style) {
  if (!self || !self->set_quoted_prompt_style)
    return SL_ERROR_INVALID;
  return self->set_quoted_prompt_style(self, style);
}

sl_readline_status_t sl_last_readline_status(const sl_t *self) {
  if (!self || !self->last_readline_status)
    return SL_READLINE_NONE;
  return self->last_readline_status(self);
}

const char *sl_last_error(const sl_t *self) {
  if (!self || !self->last_error)
    return NULL;
  return self->last_error(self);
}
