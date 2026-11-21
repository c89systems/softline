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
  ENGINE_BYTES = 16 * 1024 * 1024
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

typedef struct transcript {
  sl_transcript_id_t id;
  char title[81];
  int rows, columns, columns_offset;
  int scroll, dirty, alternate;
  size_t first, count, history_bytes, allocated;
  history_row_t history[HISTORY_ROWS];
  VTerm *vt;
  VTermScreen *screen;
  allocation_t *allocations;
  jmp_buf oom;
} transcript_t;

struct sl_transcripts {
  transcript_t *items[VIEW_LIMIT];
  sl_transcript_id_t last;
  int count, selected, entered, rows, columns;
  char line[VIEW_COLUMNS * 120 + 256];
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
  if (view->scroll > view->rows + (int)view->count)
    view->scroll = view->rows + (int)view->count;
}

static int clear_history(void *context) {
  transcript_t *view = context;
  while (view->count)
    forget_oldest(view);
  view->scroll = 0;
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
  if (!copy)
    return 0;
  while (view->count == HISTORY_ROWS ||
         view->history_bytes + bytes > HISTORY_BYTES)
    forget_oldest(view);
  row = &view->history[(view->first + view->count++) % HISTORY_ROWS];
  row->cells = copy;
  row->columns = columns;
  memcpy(copy, cells, bytes);
  view->history_bytes += bytes;
  if (view->scroll)
    ++view->scroll;
  return 1;
}

static int property(VTermProp prop, VTermValue *value, void *context) {
  transcript_t *view = context;
  if (prop == VTERM_PROP_ALTSCREEN) {
    view->alternate = value->boolean;
    view->scroll = 0;
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
  view->dirty = 1;
}

static int reset_engine(transcript_t *view) {
  static VTermAllocatorFunctions allocator = {engine_alloc, engine_free};
  static const VTermScreenCallbacks callbacks = {.settermprop = property,
                                                 .sb_pushline = retain_line,
                                                 .sb_clear = clear_history};
  release_engine(view);
  clear_history(view);
  view->columns_offset = view->alternate = 0;
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

int sl_transcript_add(sl_t *self, const char *title, int rows, int columns,
                      sl_transcript_id_t *id) {
  sl_impl_t *impl = self ? self->impl : NULL;
  struct sl_transcripts *views;
  transcript_t *view;
  size_t i;
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
  for (i = 0; i < sizeof(view->title) - 1 && title[i]; ++i)
    view->title[i] = title[i] >= ' ' && title[i] <= '~' ? title[i] : '?';
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

int sl_transcript_write(sl_t *self, sl_transcript_id_t id,
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

int sl_transcript_resize(sl_t *self, sl_transcript_id_t id, int rows,
                         int columns) {
  transcript_t *view = lookup(self, id);
  if (!view || !geometry(rows, columns))
    return SL_ERROR_INVALID;
  if (view->rows == rows && view->columns == columns)
    return SL_OK;
  view->rows = rows;
  view->columns = columns;
  view->columns_offset = 0;
  if (!view->vt)
    return SL_ERROR_NOT_READY;
  if (setjmp(view->oom)) {
    release_engine(view);
    return SL_ERROR_NOMEM;
  }
  vterm_set_size(view->vt, rows, columns);
  view->dirty = 1;
  return SL_OK;
}

int sl_transcript_reset(sl_t *self, sl_transcript_id_t id) {
  transcript_t *view = lookup(self, id);
  return view ? reset_engine(view) : SL_ERROR_INVALID;
}

int sl_transcript_visible(const sl_t *self) {
  struct sl_transcripts *views = registry(self);
  return views && views->selected >= 0;
}

int sl_transcript_leave(sl_t *self) {
  sl_impl_t *impl = self->impl;
  struct sl_transcripts *views = impl->transcripts;
  if (views) {
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

int sl_transcript_remove(sl_t *self, sl_transcript_id_t id) {
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
  if (!views)
    return;
  (void)sl_transcript_leave(self);
  views->selected = -1;
  while (views->count)
    (void)sl_transcript_remove(self, views->items[views->count - 1]->id);
  free(views);
  impl->transcripts = NULL;
}

static int limit(int value, int minimum, int maximum) {
  if (value < minimum)
    return minimum;
  return value > maximum ? maximum : value;
}

/* Return 1 when the viewer consumed the key, 0 for normal editor handling. */
int sl_transcript_key(sl_t *self, int key) {
  sl_impl_t *impl = self->impl;
  struct sl_transcripts *views = impl->transcripts;
  transcript_t *view;
  int page;
  if (!views || !views->count)
    return 0;
  if (key == SL_KEY_CTRL_T &&
      (views->selected >= 0 || (impl->raw_guard && isatty(impl->output_fd)))) {
    if (views->selected + 1 == views->count) {
      if (sl_transcript_leave(self))
        return -1;
    } else {
      views->items[++views->selected]->dirty = 1;
    }
    return 1;
  }
  if (views->selected < 0 || key == SL_KEY_NONE)
    return 0;
  if (key == SL_KEY_ESCAPE || key == SL_KEY_CTRL_C ||
      key == SL_KEY_ALT_BASE + 's' || key == SL_KEY_ALT_BASE + 'S') {
    if (sl_transcript_leave(self))
      return -1;
    return key == SL_KEY_ESCAPE || key == SL_KEY_CTRL_C;
  }
  view = views->items[views->selected];
  page = views->rows > 2 ? views->rows - 2 : 1;
  switch (key) {
  case SL_KEY_UP:
    ++view->scroll;
    break;
  case SL_KEY_DOWN:
    --view->scroll;
    break;
  case SL_KEY_PAGE_UP:
    view->scroll += page;
    break;
  case SL_KEY_PAGE_DOWN:
    view->scroll -= page;
    break;
  case SL_KEY_LEFT:
    --view->columns_offset;
    break;
  case SL_KEY_RIGHT:
    ++view->columns_offset;
    break;
  case SL_KEY_HOME:
    view->scroll = (int)view->count + view->rows;
    break;
  case SL_KEY_END:
    view->scroll = 0;
    break;
  default:
    return 1;
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

static char *cell_style(char *out, const VTermScreenCell *cell) {
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
  *out++ = 'm';
  return out;
}

int sl_transcript_paint(sl_t *self, int rows, int columns) {
  sl_impl_t *impl = self->impl;
  struct sl_transcripts *views = impl->transcripts;
  transcript_t *view = views->items[views->selected];
  sl_tty_guard_t *guard = impl->raw_guard;
  int height = rows - 1, width, history, total, first;
  int line, c, j, status;
  char title[180];
  if (!views->entered || !sl_tty_guard_in_alternate(guard)) {
    if (sl_tty_guard_alternate(guard, 1))
      return -1;
    views->entered = 1;
    view->dirty = 1;
  }
  if (!view->dirty && rows == views->rows && columns == views->columns)
    return 0;
  views->rows = rows;
  views->columns = columns;
  width = limit(columns, 0, VIEW_COLUMNS);
  history = view->alternate ? 0 : (int)view->count;
  total = history + view->rows;
  if (height < 0)
    height = 0;
  view->scroll = limit(view->scroll, 0, total > height ? total - height : 0);
  view->columns_offset =
      limit(view->columns_offset, 0,
            view->columns > width ? view->columns - width : 0);
  first = total > height ? total - height - view->scroll : 0;
  snprintf(title, sizeof(title), "%d/%d %s   Ctrl+T Next   Esc Back",
           views->selected + 1, views->count, view->title);
  line = snprintf(views->line, sizeof(views->line),
                  "\030\033[H\033[0m\033[2J\033[7m%.*s\033[0m", width, title);
  view->dirty = 1;
  status = sl_tty_guard_write_alternate(guard, views->line, (size_t)line);
  if (status)
    return status < 0 ? -1 : 0;
  if (!view->screen) {
    if (height) {
      line = snprintf(views->line, sizeof(views->line), "\033[2;1H%.*s", width,
                      "Transcript unavailable");
      status = sl_tty_guard_write_alternate(guard, views->line, (size_t)line);
    }
    view->dirty = status != 0;
    return status < 0 ? -1 : 0;
  }
  if (height > total)
    height = total;
  for (line = 0; line < height; ++line) {
    int index = first + line;
    char *out = views->line + sprintf(views->line, "\033[%d;1H", line + 2);
    char previous[80] = "";
    for (c = 0; c < width && c + view->columns_offset < view->columns; ++c) {
      VTermScreenCell cell = {0};
      int column = c + view->columns_offset;
      char style[80], *end;
      if (index < history) {
        const history_row_t *row =
            &view->history[(view->first + (size_t)index) % HISTORY_ROWS];
        if (column < row->columns)
          cell = row->cells[column];
      } else {
        VTermPos pos = {index - history, column};
        vterm_screen_get_cell(view->screen, pos, &cell);
      }
      if (cell.chars[0] == UINT32_MAX) {
        if (!c)
          *out++ = ' ';
        continue;
      }
      if (cell.width == 2 && c + 1 == width)
        break;
      end = cell_style(style, &cell);
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
    }
    memcpy(out, "\033[0m", 4);
    status = sl_tty_guard_write_alternate(guard, views->line,
                                          (size_t)(out + 4 - views->line));
    if (status)
      return status < 0 ? -1 : 0;
  }
  view->dirty = 0;
  return 0;
}
