#include "softline/transcript.h"
#include "softline_internal.h"
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <unistd.h>
#include <vterm.h>

enum {
  VIEW_LIMIT = 8,
  VIEW_ROWS = 200,
  VIEW_COLUMNS = 320,
  HISTORY_ROWS = 2000,
  HISTORY_BYTES = 2 * 1024 * 1024,
  ENGINE_BYTES = 16 * 1024 * 1024,
  PAINT_BYTES = 1024 * 1024,
  SEARCH_BYTES = 256
};

typedef union allocation allocation_t;
union allocation {
  struct {
    allocation_t *next, *previous;
    size_t bytes;
  } link;
  long double alignment;
};

typedef struct history_row {
  VTermScreenCell *cells;
  int columns;
} history_row_t;

typedef struct transcript_match {
  int row, start, end; /* End is exclusive. Zero means no selection. */
} transcript_match_t;

typedef struct transcript {
  sl_transcript_id_t id;
  int rows, columns, columns_offset;
  int scroll, paused, dirty, alternate, pushed;
  char query[SEARCH_BYTES + 1], input[SEARCH_BYTES + 1];
  int searching, was_paused, no_match;
  transcript_match_t match;
  size_t first, count, history_bytes, allocated;
  history_row_t history[HISTORY_ROWS];
  VTerm *vt;
  VTermScreen *screen;
  allocation_t *allocations;
  jmp_buf oom;
} transcript_t;

typedef struct painted_row {
  char *bytes;
  size_t length, capacity;
} painted_row_t;

struct sl_transcripts {
  transcript_t *items[VIEW_LIMIT];
  sl_transcript_id_t last;
  int count, selected, entered, rows, columns;
  int repaint, shown_rows, paint_count;
  size_t paint_bytes;
  painted_row_t *paint;
  char line[VIEW_COLUMNS * 120 + 256];
  char search_text[VIEW_COLUMNS * VTERM_MAX_CHARS_PER_CELL * 4 + 1];
  int search_offsets[VIEW_COLUMNS + 1];
};

static void *engine_alloc(size_t bytes, void *context) {
  transcript_t *view = context;
  allocation_t *block;
  if (bytes > ENGINE_BYTES - view->allocated ||
      !(block = calloc(1, sizeof(*block) + bytes)))
    longjmp(view->oom, 1);
  block->link.bytes = bytes;
  block->link.next = view->allocations;
  if (view->allocations)
    view->allocations->link.previous = block;
  view->allocations = block;
  view->allocated += bytes;
  return block + 1;
}

static void engine_free(void *pointer, void *context) {
  transcript_t *view = context;
  allocation_t *block;
  if (!pointer)
    return;
  block = (allocation_t *)pointer - 1;
  if (block->link.previous)
    block->link.previous->link.next = block->link.next;
  else
    view->allocations = block->link.next;
  if (block->link.next)
    block->link.next->link.previous = block->link.previous;
  view->allocated -= block->link.bytes;
  free(block);
}

static void forget_oldest(transcript_t *view) {
  history_row_t *row = &view->history[view->first];
  view->history_bytes -= (size_t)row->columns * sizeof(*row->cells);
  free(row->cells);
  view->first = (view->first + 1) % HISTORY_ROWS;
  --view->count;
  if (view->match.end && !view->alternate && --view->match.row < 0)
    view->match.end = 0;
  if (view->scroll > view->rows + (int)view->count)
    view->scroll = view->rows + (int)view->count;
}

static int clear_history(void *context) {
  transcript_t *view = context;
  while (view->count)
    forget_oldest(view);
  view->scroll = 0;
  view->match.end = 0;
  return 1;
}

static int retain_line(int columns, const VTermScreenCell *cells,
                       void *context) {
  transcript_t *view = context;
  size_t bytes = (size_t)columns * sizeof(*cells);
  history_row_t *row;
  VTermScreenCell *copy;
  if (view->alternate || columns < 1 || columns > VIEW_COLUMNS)
    return 0;
  copy = malloc(bytes);
  if (!copy) {
    view->match.end = 0;
    return 0;
  }
  while (view->count == HISTORY_ROWS ||
         view->history_bytes + bytes > HISTORY_BYTES)
    forget_oldest(view);
  row = &view->history[(view->first + view->count++) % HISTORY_ROWS];
  row->cells = copy;
  row->columns = columns;
  memcpy(copy, cells, bytes);
  view->history_bytes += bytes;
  ++view->pushed;
  if (view->paused)
    ++view->scroll;
  return 1;
}

static int history_rows(const transcript_t *view) {
  return view->alternate ? 0 : (int)view->count;
}

static int includes_row(VTermRect rect, int row) {
  return row >= rect.start_row && row < rect.end_row;
}

static int damage(VTermRect rect, void *context) {
  transcript_t *view = context;
  if (view->match.end &&
      includes_row(rect, view->match.row - history_rows(view)))
    view->match.end = 0;
  return 1;
}

static int move_cells(VTermRect dest, VTermRect src, void *context) {
  transcript_t *view = context;
  int history = history_rows(view);
  /* libvterm pushes scrolled-off rows before reporting the move. Selections
   * already in history keep their index (adjusted separately on eviction). */
  if (view->match.end && view->match.row >= history) {
    int row = view->match.row - history + view->pushed;
    if (includes_row(src, row) && src.start_col == 0 && dest.start_col == 0 &&
        src.end_col == view->columns && dest.end_col == view->columns)
      view->match.row = history + dest.start_row + row - src.start_row;
    else if (includes_row(src, row) || includes_row(dest, row))
      view->match.end = 0;
    else
      view->match.row = history + row;
  }
  view->pushed = 0;
  return 1;
}

static int property(VTermProp prop, VTermValue *value, void *context) {
  transcript_t *view = context;
  if (prop == VTERM_PROP_ALTSCREEN) {
    view->alternate = value->boolean;
    view->scroll = 0;
    view->paused = view->searching;
    view->match.end = view->pushed = 0;
  }
  return 1;
}

static void discard_response(const char *bytes, size_t length, void *context) {
  (void)bytes;
  (void)length;
  (void)context;
}

static void release_engine(transcript_t *view) {
  while (view->allocations)
    engine_free(view->allocations + 1, view);
  view->vt = NULL;
  view->screen = NULL;
  view->match.end = 0;
  view->dirty = 1;
}

static int reset_engine(transcript_t *view) {
  static VTermAllocatorFunctions allocator = {engine_alloc, engine_free};
  static const VTermScreenCallbacks callbacks = {.damage = damage,
                                                 .moverect = move_cells,
                                                 .settermprop = property,
                                                 .sb_pushline = retain_line,
                                                 .sb_clear = clear_history};
  release_engine(view);
  clear_history(view);
  view->columns_offset = view->alternate = 0;
  view->pushed = view->paused = view->searching = view->no_match = 0;
  view->query[0] = '\0';
  if (setjmp(view->oom)) {
    release_engine(view);
    return SL_ERROR_NOMEM;
  }
  view->vt =
      vterm_new_with_allocator(view->rows, view->columns, &allocator, view);
  vterm_set_utf8(view->vt, 1);
  vterm_output_set_callback(view->vt, discard_response, NULL);
  view->screen = vterm_obtain_screen(view->vt);
  vterm_screen_enable_altscreen(view->screen, 1);
  vterm_screen_set_callbacks(view->screen, &callbacks, view);
  vterm_screen_reset(view->screen, 1);
  return SL_OK;
}

static int geometry(int rows, int columns) {
  return rows > 0 && rows <= VIEW_ROWS && columns > 0 &&
         columns <= VIEW_COLUMNS;
}

static struct sl_transcripts *registry(const sl_t *self) {
  const sl_impl_t *impl = self ? self->impl : NULL;
  return impl ? impl->transcripts : NULL;
}

static int index_of(const struct sl_transcripts *views, sl_transcript_id_t id) {
  int i;
  for (i = 0; views && i < views->count; ++i)
    if (views->items[i]->id == id)
      return i;
  return -1;
}

static transcript_t *lookup(sl_t *self, sl_transcript_id_t id) {
  struct sl_transcripts *views = registry(self);
  int index = index_of(views, id);
  return index >= 0 ? views->items[index] : NULL;
}

static int transcript_add(sl_t *self, const char *title, int rows, int columns,
                          sl_transcript_id_t *id) {
  sl_impl_t *impl = self ? self->impl : NULL;
  struct sl_transcripts *views;
  transcript_t *view;
  int rc;
  if (!impl || !title || !id || !geometry(rows, columns))
    return SL_ERROR_INVALID;
  views = impl->transcripts;
  if (!views) {
    views = calloc(1, sizeof(*views));
    if (!views)
      return SL_ERROR_NOMEM;
    views->selected = -1;
    impl->transcripts = views;
  }
  if (views->count == VIEW_LIMIT || views->last == ULONG_MAX)
    return SL_ERROR_FULL;
  view = calloc(1, sizeof(*view));
  if (!view)
    return SL_ERROR_NOMEM;
  view->rows = rows;
  view->columns = columns;
  rc = reset_engine(view);
  if (rc != SL_OK) {
    free(view);
    return rc;
  }
  views->items[views->count++] = view;
  view->id = *id = ++views->last;
  if (views->selected >= 0)
    views->items[views->selected]->dirty = 1;
  return SL_OK;
}

static void write_text(transcript_t *view, const unsigned char *data,
                       size_t length) {
  /* Remove ASCII controls before feeding libvterm. UTF-8 still arrives in
   * order, so combining characters split across host chunks stay intact. */
  size_t start = 0, i;
  for (i = 0; i < length; ++i) {
    unsigned char c = data[i];
    if (c >= 32 && c != 127)
      continue;
    if (i > start)
      vterm_input_write(view->vt, (const char *)data + start, i - start);
    if (c == '\n')
      vterm_input_write(view->vt, "\r\n", 2);
    else if (c == '\t')
      vterm_input_write(view->vt, "\t", 1);
    start = i + 1;
  }
  if (start < length)
    vterm_input_write(view->vt, (const char *)data + start, length - start);
}

static int transcript_write(sl_t *self, sl_transcript_id_t id,
                            sl_transcript_format_t format, const void *bytes,
                            size_t length) {
  transcript_t *view = lookup(self, id);
  if (!view || (!bytes && length) ||
      (format != SL_TRANSCRIPT_TEXT && format != SL_TRANSCRIPT_VT))
    return SL_ERROR_INVALID;
  if (!view->vt)
    return SL_ERROR_NOT_READY;
  if (!length)
    return SL_OK;
  if (setjmp(view->oom)) {
    release_engine(view);
    return SL_ERROR_NOMEM;
  }
  if (format == SL_TRANSCRIPT_TEXT)
    write_text(view, bytes, length);
  else
    vterm_input_write(view->vt, bytes, length);
  view->dirty = 1;
  return SL_OK;
}

static int transcript_resize(sl_t *self, sl_transcript_id_t id, int rows,
                             int columns) {
  transcript_t *view = lookup(self, id);
  if (!view || !geometry(rows, columns))
    return SL_ERROR_INVALID;
  if (view->rows == rows && view->columns == columns)
    return SL_OK;
  view->rows = rows;
  view->columns = columns;
  view->columns_offset = 0;
  view->match.end = 0;
  if (!view->vt)
    return SL_ERROR_NOT_READY;
  if (setjmp(view->oom)) {
    release_engine(view);
    return SL_ERROR_NOMEM;
  }
  vterm_set_size(view->vt, rows, columns);
  view->pushed = 0;
  view->dirty = 1;
  return SL_OK;
}

static int transcript_reset(sl_t *self, sl_transcript_id_t id) {
  transcript_t *view = lookup(self, id);
  return view ? reset_engine(view) : SL_ERROR_INVALID;
}

static int transcript_visible(const sl_t *self) {
  struct sl_transcripts *views = registry(self);
  return views && views->selected >= 0;
}

static void cancel_search(transcript_t *view) {
  if (view->searching) {
    view->searching = 0;
    view->paused = view->was_paused || view->scroll > 0;
    view->dirty = 1;
  }
}

int sl_transcript_leave(sl_t *self) {
  sl_impl_t *impl = self->impl;
  struct sl_transcripts *views = impl->transcripts;
  if (views) {
    if (views->selected >= 0)
      cancel_search(views->items[views->selected]);
    if (views->entered) {
      if (impl->raw_guard && sl_tty_guard_alternate(impl->raw_guard, 0))
        return -1;
      views->entered = 0;
      impl->cursor_hidden = 0;
    }
    views->selected = -1;
  }
  return sl_output_replay_transcript(self) == SL_OK ? 0 : -1;
}

static int transcript_remove(sl_t *self, sl_transcript_id_t id) {
  struct sl_transcripts *views = registry(self);
  int at = index_of(views, id);
  transcript_t *view;
  if (at < 0)
    return SL_ERROR_INVALID;
  if (views->selected == at && sl_transcript_leave(self))
    return SL_ERROR_IO;
  if (views->selected > at)
    --views->selected;
  view = views->items[at];
  release_engine(view);
  clear_history(view);
  free(view);
  --views->count;
  memmove(views->items + at, views->items + at + 1,
          (size_t)(views->count - at) * sizeof(views->items[0]));
  if (views->selected >= 0)
    views->items[views->selected]->dirty = 1;
  return SL_OK;
}

void sl_transcript_destroy(sl_t *self) {
  sl_impl_t *impl = self->impl;
  struct sl_transcripts *views = impl->transcripts;
  int i;
  if (!views)
    return;
  (void)sl_transcript_leave(self);
  views->selected = -1;
  while (views->count)
    (void)transcript_remove(self, views->items[views->count - 1]->id);
  for (i = 0; i < views->paint_count; ++i)
    free(views->paint[i].bytes);
  free(views->paint);
  free(views);
  impl->transcripts = NULL;
}

void sl_transcript_init_receiver(sl_t *self) {
  self->transcript_add = transcript_add;
  self->transcript_write = transcript_write;
  self->transcript_resize = transcript_resize;
  self->transcript_reset = transcript_reset;
  self->transcript_remove = transcript_remove;
  self->transcript_visible = transcript_visible;
}

int sl_transcript_add(sl_t *self, const char *title, int rows, int columns,
                      sl_transcript_id_t *id) {
  return self && self->transcript_add
             ? self->transcript_add(self, title, rows, columns, id)
             : SL_ERROR_INVALID;
}

int sl_transcript_write(sl_t *self, sl_transcript_id_t id,
                        sl_transcript_format_t format, const void *bytes,
                        size_t length) {
  return self && self->transcript_write
             ? self->transcript_write(self, id, format, bytes, length)
             : SL_ERROR_INVALID;
}

int sl_transcript_resize(sl_t *self, sl_transcript_id_t id, int rows,
                         int columns) {
  return self && self->transcript_resize
             ? self->transcript_resize(self, id, rows, columns)
             : SL_ERROR_INVALID;
}

int sl_transcript_reset(sl_t *self, sl_transcript_id_t id) {
  return self && self->transcript_reset ? self->transcript_reset(self, id)
                                        : SL_ERROR_INVALID;
}

int sl_transcript_remove(sl_t *self, sl_transcript_id_t id) {
  return self && self->transcript_remove ? self->transcript_remove(self, id)
                                         : SL_ERROR_INVALID;
}

int sl_transcript_visible(const sl_t *self) {
  return self && self->transcript_visible ? self->transcript_visible(self) : 0;
}

static int limit(int value, int minimum, int maximum) {
  if (value < minimum)
    return minimum;
  return value > maximum ? maximum : value;
}

static int scroll_limit(const transcript_t *view, int height) {
  int maximum = history_rows(view) + view->rows - height;
  return maximum > 0 ? maximum : 0;
}

static void find_match(struct sl_transcripts *views, transcript_t *view,
                       int direction);

static uint32_t lower_ascii(uint32_t character) {
  return character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                              : character;
}

static void search_key(struct sl_transcripts *views, transcript_t *view,
                       int key, const char *bytes, size_t length) {
  size_t i, size = strlen(view->input);
  char character;
  if (key == SL_KEY_ESCAPE || (key == SL_KEY_ENTER && !size)) {
    cancel_search(view);
  } else if (key == SL_KEY_ENTER) {
    for (i = 0; i <= size; ++i)
      view->query[i] = (char)lower_ascii((unsigned char)view->input[i]);
    view->searching = view->match.end = 0;
    view->no_match = 0;
    find_match(views, view, 1);
  } else if (key == SL_KEY_BACKSPACE || key == 8) {
    view->input[size - sl_utf8_prev_cluster_len(view->input, size)] = '\0';
  } else if (key == SL_KEY_CTRL_U) {
    view->input[0] = '\0';
  } else {
    if (!length && key >= ' ' && key <= '~') {
      character = (char)key;
      bytes = &character;
      length = 1;
    }
    if (length && length <= SEARCH_BYTES - size &&
        !memchr(bytes, '\0', length) && !memchr(bytes, '\n', length) &&
        !memchr(bytes, '\t', length)) {
      memcpy(view->input + size, bytes, length);
      view->input[size + length] = '\0';
      if (!sl_quoted_prompt_text_valid(view->input + size))
        view->input[size] = '\0';
    }
  }
  view->dirty = 1;
}

/* Return 1 when the viewer consumed the key, 0 for normal editor handling. */
int sl_transcript_key(sl_t *self, int key, const char *bytes, size_t length) {
  sl_impl_t *impl = self->impl;
  struct sl_transcripts *views = impl->transcripts;
  transcript_t *view;
  int page, half, step = 0;
  if (!views || !views->count)
    return 0;
  if (key == SL_KEY_CTRL_T &&
      (views->selected >= 0 || (impl->raw_guard && isatty(impl->output_fd)))) {
    if (views->selected + 1 == views->count) {
      if (sl_transcript_leave(self))
        return -1;
    } else {
      if (views->selected >= 0)
        cancel_search(views->items[views->selected]);
      views->items[++views->selected]->dirty = 1;
    }
    return 1;
  }
  if (views->selected < 0 || key == SL_KEY_NONE)
    return 0;
  view = views->items[views->selected];
  if (view->searching && key != SL_KEY_CTRL_C && key != SL_KEY_ALT_BASE + 's' &&
      key != SL_KEY_ALT_BASE + 'S') {
    search_key(views, view, key, bytes, length);
    return 1;
  }
  if (key == SL_KEY_ESCAPE || key == SL_KEY_CTRL_C || key == 'q' ||
      key == SL_KEY_ALT_BASE + 's' || key == SL_KEY_ALT_BASE + 'S') {
    if (sl_transcript_leave(self))
      return -1;
    return key == SL_KEY_ESCAPE || key == SL_KEY_CTRL_C || key == 'q';
  }
  page = views->rows > 2 ? views->rows - 2 : 1;
  half = views->rows > 2 ? (views->rows - 1) / 2 : 1;
  switch (key) {
  case SL_KEY_UP:
  case 'k':
    step = 1;
    break;
  case SL_KEY_DOWN:
  case 'j':
    step = -1;
    break;
  case 'K':
    step = 5;
    break;
  case 'J':
    step = -5;
    break;
  case 'u':
    step = half;
    break;
  case 'd':
    step = -half;
    break;
  case SL_KEY_PAGE_UP:
  case 'b':
    step = page;
    break;
  case SL_KEY_PAGE_DOWN:
  case ' ':
  case 'f':
    step = -page;
    break;
  case SL_KEY_LEFT:
    --view->columns_offset;
    break;
  case SL_KEY_RIGHT:
    ++view->columns_offset;
    break;
  case SL_KEY_HOME:
  case 'g':
    view->scroll = scroll_limit(view, views->rows - 1);
    view->paused = 1;
    break;
  case SL_KEY_END:
  case 'G':
    view->scroll = view->paused = 0;
    view->match.end = 0;
    break;
  case '/':
    view->input[0] = '\0';
    view->was_paused = view->paused;
    view->searching = view->paused = 1;
    break;
  case 'n':
  case 'N':
    find_match(views, view, key == 'n' ? 1 : -1);
    break;
  case 'c':
    view->query[0] = '\0';
    view->match.end = 0;
    break;
  default:
    return 1;
  }
  if (key != '/' && key != 'n' && key != 'N')
    view->no_match = 0;
  if (step) {
    view->scroll =
        limit(view->scroll + step, 0, scroll_limit(view, views->rows - 1));
    view->paused = view->scroll > 0 || step > 0;
  }
  view->dirty = 1;
  return 1;
}

static char *utf8(char *out, uint32_t ch) {
  if (ch < 32 || (ch >= 127 && ch < 160) || ch > 0x10ffff ||
      (ch >= 0xd800 && ch <= 0xdfff))
    ch = ' ';
  if (ch < 0x80) {
    *out++ = (char)ch;
  } else if (ch < 0x800) {
    *out++ = (char)(0xc0 | ch >> 6);
    *out++ = (char)(0x80 | (ch & 63));
  } else {
    if (ch >= 0x10000) {
      *out++ = (char)(0xf0 | ch >> 18);
      *out++ = (char)(0x80 | ((ch >> 12) & 63));
    } else {
      *out++ = (char)(0xe0 | ch >> 12);
    }
    *out++ = (char)(0x80 | ((ch >> 6) & 63));
    *out++ = (char)(0x80 | (ch & 63));
  }
  return out;
}

static const history_row_t *retained_row(const transcript_t *view, int row) {
  return row < history_rows(view)
             ? &view->history[(view->first + (size_t)row) % HISTORY_ROWS]
             : NULL;
}

static VTermScreenCell row_cell(const transcript_t *view, int row, int column) {
  const history_row_t *retained = retained_row(view, row);
  VTermScreenCell cell = {0};
  if (retained)
    return retained->cells[column];
  {
    VTermPos pos = {row - history_rows(view), column};
    vterm_screen_get_cell(view->screen, pos, &cell);
  }
  return cell;
}

static int row_columns(const transcript_t *view, int row) {
  const history_row_t *retained = retained_row(view, row);
  return retained ? retained->columns : view->columns;
}

/* Project one row to literal UTF-8 and map byte offsets back to screen cells.
 * A continuation column of a wide cell cannot start or end a match. */
static void search_row(struct sl_transcripts *views, const transcript_t *view,
                       int row, int columns) {
  char *out = views->search_text;
  int column, i;
  for (column = 0; column < columns; ++column) {
    VTermScreenCell cell = row_cell(view, row, column);
    if (cell.chars[0] == UINT32_MAX) {
      views->search_offsets[column] = -1;
      continue;
    }
    views->search_offsets[column] = (int)(out - views->search_text);
    if (!cell.chars[0] || cell.attrs.conceal) {
      *out++ = ' ';
      if (cell.width == 2)
        *out++ = ' ';
    } else {
      for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
        out = utf8(out, lower_ascii(cell.chars[i]));
    }
  }
  views->search_offsets[columns] = (int)(out - views->search_text);
}

static int match_end(const struct sl_transcripts *views, const char *query,
                     int length, int column, int columns) {
  const int *offsets = views->search_offsets;
  int start = offsets[column], end = start + length;
  if (start < 0 || end > offsets[columns] ||
      memcmp(views->search_text + start, query, (size_t)length))
    return 0;
  for (; column < columns; ++column)
    if (offsets[column + 1] >= end)
      return offsets[column + 1] == end ? column + 1 : 0;
  return 0;
}

static void reveal_match(struct sl_transcripts *views, transcript_t *view,
                         int first, int height, int row, int start, int end) {
  int width = limit(views->columns, 1, VIEW_COLUMNS);
  int maximum = scroll_limit(view, height);
  view->match.row = row;
  view->match.start = start;
  view->match.end = end;
  view->no_match = 0;
  if (row < first)
    first = row;
  else if (row >= first + height)
    first = row - height + 1;
  view->scroll = limit(maximum - first, 0, maximum);
  view->paused = 1;
  if (start < view->columns_offset || end - start > width)
    view->columns_offset = start;
  else if (end > view->columns_offset + width)
    view->columns_offset = end - width;
}

static void find_match(struct sl_transcripts *views, transcript_t *view,
                       int direction) {
  int height = views->rows > 1 ? views->rows - 1 : 1;
  int maximum = scroll_limit(view, height);
  int first = maximum - limit(view->scroll, 0, maximum);
  int total = history_rows(view) + view->rows;
  int row = direction > 0 ? first : limit(first + height - 1, 0, total - 1);
  int column = direction > 0 ? 0 : VIEW_COLUMNS - 1;
  int length = (int)strlen(view->query);
  if (!view->screen || !length)
    return;
  if (view->match.end) {
    row = view->match.row;
    column = view->match.start + direction;
  }
  for (; row >= 0 && row < total; row += direction) {
    int columns = row_columns(view, row);
    search_row(views, view, row, columns);
    if (direction < 0 && column >= columns)
      column = columns - 1;
    for (; column >= 0 && column < columns; column += direction) {
      int end = match_end(views, view->query, length, column, columns);
      if (end) {
        reveal_match(views, view, first, height, row, column, end);
        return;
      }
    }
    column = direction > 0 ? 0 : VIEW_COLUMNS - 1;
  }
  view->no_match = 1;
}

static void mark_matches(struct sl_transcripts *views, const transcript_t *view,
                         int row, int columns, unsigned char *marks) {
  int column, length = (int)strlen(view->query);
  if (!length)
    return;
  search_row(views, view, row, columns);
  for (column = 0; column < columns; ++column) {
    int end = match_end(views, view->query, length, column, columns);
    if (end)
      memset(marks + column, 1, (size_t)(end - column));
  }
  if (view->match.end && view->match.row == row)
    memset(marks + view->match.start, 2,
           (size_t)(view->match.end - view->match.start));
}

static char *color(char *out, const VTermColor *color, int background) {
  if (VTERM_COLOR_IS_DEFAULT_FG(color) || VTERM_COLOR_IS_DEFAULT_BG(color))
    return out;
  if (VTERM_COLOR_IS_INDEXED(color))
    return out + sprintf(out, ";%d;5;%u", background ? 48 : 38,
                         (unsigned int)color->indexed.idx);
  return out + sprintf(out, ";%d;2;%u;%u;%u", background ? 48 : 38,
                       (unsigned int)color->rgb.red,
                       (unsigned int)color->rgb.green,
                       (unsigned int)color->rgb.blue);
}

static char *cell_style(char *out, const VTermScreenCell *cell, int highlight) {
  *out++ = '\033';
  *out++ = '[';
  *out++ = '0';
  if (cell->attrs.bold) {
    *out++ = ';';
    *out++ = '1';
  }
  if (cell->attrs.italic) {
    *out++ = ';';
    *out++ = '3';
  }
  if (cell->attrs.underline) {
    *out++ = ';';
    *out++ = '4';
  }
  if (cell->attrs.reverse) {
    *out++ = ';';
    *out++ = '7';
  }
  if (cell->attrs.conceal) {
    *out++ = ';';
    *out++ = '8';
  }
  if (cell->attrs.strike) {
    *out++ = ';';
    *out++ = '9';
  }
  out = color(out, &cell->fg, 0);
  out = color(out, &cell->bg, 1);
  if (highlight)
    out += sprintf(out, ";27;30;%d", highlight == 2 ? 103 : 43);
  *out++ = 'm';
  return out;
}

static size_t search_header(char *out, const char *text, int width, int *used) {
  size_t length = strlen(text), start = length;
  int columns = 1;
  while (start) {
    size_t previous = start - sl_utf8_prev_cluster_len(text, start);
    int cells;
    sl_utf8_cluster_len_width(text, start, previous, &cells);
    if (columns + cells > width)
      break;
    columns += cells;
    start = previous;
  }
  *used = columns;
  return (size_t)sprintf(out, "/%s", text + start);
}

static int paint_row(struct sl_transcripts *views, sl_tty_guard_t *guard,
                     int row, char *end, int used) {
  painted_row_t *saved = row < views->paint_count ? &views->paint[row] : NULL;
  size_t length;
  int status;
  memcpy(end, "\033[0m", 4);
  end += 4;
  /* At the physical right margin the cursor can still be on the last
   * painted cell, with autowrap pending. There is no tail to erase there. */
  if (used < views->columns) {
    memcpy(end, "\033[K", 3);
    end += 3;
  }
  length = (size_t)(end - views->line);
  if (saved && length == saved->length &&
      !memcmp(views->line, saved->bytes, length))
    return 0;
  status = sl_tty_guard_write_alternate(guard, views->line, length);
  if (status || !saved)
    return status;
  if (saved->capacity < length &&
      length - saved->capacity <= PAINT_BYTES - views->paint_bytes) {
    char *bytes = realloc(saved->bytes, length);
    if (bytes) {
      views->paint_bytes += length - saved->capacity;
      saved->bytes = bytes;
      saved->capacity = length;
    }
  }
  saved->length = 0;
  if (length <= saved->capacity) {
    memcpy(saved->bytes, views->line, length);
    saved->length = length;
  }
  return 0;
}

static int paint_content(struct sl_transcripts *views, sl_tty_guard_t *guard,
                         const transcript_t *view, int index, int row,
                         int width) {
  int c, j, used = 0, row_width = row_columns(view, index);
  unsigned char marks[VIEW_COLUMNS] = {0};
  char *out = views->line + sprintf(views->line, "\033[%d;1H\033[0m", row + 1);
  char *tail = out;
  char previous[80] = "\033[0m";
  mark_matches(views, view, index, row_width, marks);
  for (c = 0; c < width && c + view->columns_offset < row_width; ++c) {
    int column = c + view->columns_offset;
    VTermScreenCell cell = row_cell(view, index, column);
    char style[80], *end;
    if (cell.chars[0] == UINT32_MAX) {
      if (!c)
        *out++ = ' ';
      continue;
    }
    if (cell.width == 2 && c + 1 == width)
      break;
    end = cell_style(style, &cell, marks[column]);
    *end = '\0';
    if (strcmp(style, previous)) {
      memcpy(out, style, (size_t)(end - style));
      out += end - style;
      strcpy(previous, style);
    }
    if (!cell.chars[0])
      *out++ = ' ';
    else
      for (j = 0; j < VTERM_MAX_CHARS_PER_CELL && cell.chars[j]; ++j)
        out = utf8(out, cell.chars[j]);
    if ((cell.chars[0] && cell.chars[0] != ' ') || cell.chars[1] ||
        strcmp(style, "\033[0m")) {
      tail = out;
      used = c + (cell.width == 2 ? 2 : 1);
    }
  }
  return paint_row(views, guard, row, tail, used);
}

int sl_transcript_paint(sl_t *self, int rows, int columns) {
  sl_impl_t *impl = self->impl;
  struct sl_transcripts *views = impl->transcripts;
  transcript_t *view = views->items[views->selected];
  sl_tty_guard_t *guard = impl->raw_guard;
  int height = rows - 1, width, history, total, first, widest;
  int line, status, shown, count, used;
  int refresh =
      views->repaint || rows != views->rows || columns != views->columns;
  char *out;
  char title[96];
  if (!views->entered || !sl_tty_guard_in_alternate(guard)) {
    if (sl_tty_guard_alternate(guard, 1))
      return -1;
    views->entered = 1;
    refresh = 1;
  }
  if (!view->dirty && !refresh)
    return 0;
  views->rows = rows;
  views->columns = columns;
  /* A timed-out or interrupted write can leave a partial escape sequence
   * on the terminal. In that case the next paint starts from a clean screen. */
  views->repaint = 1;
  if (refresh) {
    static const char clear[] = "\030\033[H\033[0m\033[2J";
    status = sl_tty_guard_write_alternate(guard, clear, sizeof(clear) - 1);
    if (status)
      return status < 0 ? -1 : 0;
    for (line = 0; line < views->paint_count; ++line)
      views->paint[line].length = 0;
    views->shown_rows = 0;
  }
  width = limit(columns, 0, VIEW_COLUMNS);
  history = history_rows(view);
  total = history + view->rows;
  if (height < 0)
    height = 0;
  view->scroll = limit(view->scroll, 0, scroll_limit(view, height));
  widest = view->columns;
  for (line = 0; line < history; ++line) {
    int retained = row_columns(view, line);
    if (retained > widest)
      widest = retained;
  }
  view->columns_offset =
      limit(view->columns_offset, 0, widest > width ? widest - width : 0);
  first = scroll_limit(view, height) - view->scroll;
  snprintf(title, sizeof(title), "%s%d/%d  jkJKudgG scroll  / search  q quit%s",
           view->no_match ? "No further match   " : "", views->selected + 1,
           views->count, view->query[0] ? "  c clear  nN match" : "");
  shown = 1 + limit(height, 0, view->screen ? total : 1);
  count = views->shown_rows > shown ? views->shown_rows : shown;
  if (count > views->paint_count) {
    painted_row_t *paint =
        realloc(views->paint, (size_t)count * sizeof(*paint));
    if (paint) {
      memset(paint + views->paint_count, 0,
             (size_t)(count - views->paint_count) * sizeof(*paint));
      views->paint = paint;
      views->paint_count = count;
    }
  }
  out = views->line + sprintf(views->line, "\033[1;1H\033[0m\033[7m");
  if (view->searching && width)
    out += search_header(out, view->input, width, &used);
  else {
    used = sprintf(out, "%.*s", width, title);
    out += used;
  }
  status = paint_row(views, guard, 0, out, used);
  if (status)
    return status < 0 ? -1 : 0;
  for (line = 1; line < count; ++line) {
    if (view->screen && line < shown)
      status = paint_content(views, guard, view, first + line - 1, line, width);
    else {
      out = views->line + sprintf(views->line, "\033[%d;1H\033[0m", line + 1);
      used = line < shown
                 ? sprintf(out, "%.*s", width, "Transcript unavailable")
                 : 0;
      status = paint_row(views, guard, line, out + used, used);
    }
    if (status)
      return status < 0 ? -1 : 0;
  }
  views->shown_rows = shown;
  views->repaint = 0;
  view->dirty = 0;
  return 0;
}
