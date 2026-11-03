#include "softline_surface.h"
#include "softline_unicode.h"
#include <softline/softline.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>

#define SL_ROW(surface)                                                        \
  ((surface)->terminal_rows - 1 - (surface)->producer_below)

struct sl_surface {
  int fd;
  int width;
  int height;
  int terminal_rows;
  int terminal_columns;
  int native_stream;
  int producer_cursor_live;
  int producer_below;
  int history_rows; /* Bounded viewport-shift estimate, not terminal history.
                     * -1 assumes history can fill exposed rows. */
  int tracking;
  int write_line_control;
  unsigned int attributes;
  int foreground[4];
  int background[4];
  int col;
  size_t line_cells;
  size_t line_extent;
  int ascii_line;
  int line_open;
  int parser; /* 0 text, 1 ESC, 2 CSI, 3..7 OSC 8 */
  int osc8_enabled, osc8_open;
  size_t osc_len, uri_len;
  unsigned long osc_cp, osc_min;
  unsigned int osc_need;
  char *hyperlink; /* Active producer control only; no label/transcript. */
  char csi[128];
  size_t csi_len;
  char utf8[4];
  unsigned int utf8_len;
  unsigned int utf8_need;
};

static int sl_surface_terminal_size(int fd, struct winsize *terminal) {
  if (ioctl(fd, TIOCGWINSZ, terminal) != 0)
    return -1;
  if (terminal->ws_col == 0)
    terminal->ws_col = 80;
  if (terminal->ws_row == 0)
    terminal->ws_row = 24;
  return 0;
}

static int sl_surface_write_all(int fd, const char *bytes, size_t length) {
  while (length > 0) {
    ssize_t written;
    written = write(fd, bytes, length);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return -1;
    bytes += written;
    length -= (size_t)written;
  }
  return 0;
}

/* Local scalar cell accounting never asks the terminal for a feed endpoint.
 * Unicode cluster shaping and transcript reflow remain terminal-owned. */
static int sl_surface_put_cells(sl_surface_t *surface, int cells) {
  if (!surface->tracking || cells == 0)
    return 0;
  if (cells > surface->width)
    cells = surface->width;
  if (surface->col + cells > surface->width) {
    surface->col = 0;
    if (SL_ROW(surface) + 1 < surface->height)
      surface->producer_below--;
    else
      sl_surface_native_history_shift(surface, -1);
  }
  surface->col += cells;
  surface->line_cells += (size_t)cells;
  if (surface->line_cells > surface->line_extent)
    surface->line_extent = surface->line_cells;
  surface->line_open = 1;
  return 0;
}

static int sl_surface_parse_sgr(sl_surface_t *surface) {
  int params[32];
  size_t count;
  size_t i;
  int value;
  count = 0;
  value = 0;
  if (surface->csi_len == 0)
    params[count++] = 0;
  for (i = 0; i < surface->csi_len; i++) {
    unsigned char ch;
    ch = (unsigned char)surface->csi[i];
    if (ch == ';') {
      if (count >= sizeof(params) / sizeof(params[0]))
        return -2;
      params[count++] = value;
      value = 0;
    } else if (ch >= '0' && ch <= '9') {
      if (value > 100000)
        return -2;
      value = value * 10 + (int)(ch - '0');
    } else {
      return -2;
    }
  }
  if (surface->csi_len > 0) {
    if (count >= sizeof(params) / sizeof(params[0]))
      return -2;
    params[count++] = value;
  }
  for (i = 0; i < count; i++) {
    int code;
    code = params[i];
    if (code == 0 || code == 1 || code == 2 || code == 3 || code == 4 ||
        code == 7 || code == 9 || code == 22 || code == 23 || code == 24 ||
        code == 27 || code == 29 || (code >= 30 && code <= 37) || code == 39 ||
        (code >= 40 && code <= 47) || code == 49 ||
        (code >= 90 && code <= 97) || (code >= 100 && code <= 107))
      continue;
    if (code == 38 || code == 48) {
      if (i + 2 < count && params[i + 1] == 5 && params[i + 2] <= 255)
        i += 2;
      else if (i + 4 < count && params[i + 1] == 2 && params[i + 2] <= 255 &&
               params[i + 3] <= 255 && params[i + 4] <= 255)
        i += 4;
      else
        return -2;
    } else
      return -2;
  }
  if (!surface->tracking)
    return 0;
  for (i = 0; i < count; i++) {
    int code = params[i];
    int *color = code == 38 || code == 39 || (code >= 30 && code <= 37) ||
                         (code >= 90 && code <= 97)
                     ? surface->foreground
                     : surface->background;
    if (code == 0) {
      surface->attributes = 0;
      memset(surface->foreground, 0, sizeof(surface->foreground));
      memset(surface->background, 0, sizeof(surface->background));
    } else if (code == 1 || code == 2 || code == 3 || code == 4 || code == 7 ||
               code == 9) {
      surface->attributes |= 1u << code;
    } else if (code == 22) {
      surface->attributes &= ~((1u << 1) | (1u << 2));
    } else if (code == 23 || code == 24 || code == 27 || code == 29) {
      surface->attributes &= ~(1u << (code - 20));
    } else if (code == 39 || code == 49) {
      memset(color, 0, 4 * sizeof(*color));
    } else if (code == 38 || code == 48) {
      color[0] = params[++i];
      color[1] = params[++i];
      if (color[0] == 2) {
        color[2] = params[++i];
        color[3] = params[++i];
      }
    } else {
      color[0] = 1;
      color[1] = code;
    }
  }
  return 0;
}

static int sl_surface_byte(sl_surface_t *surface, unsigned char byte,
                           int newline_returns) {
  unsigned long cp;
  unsigned int i;
  if (surface->parser >= 3) {
    int state = surface->parser;
    if (++surface->osc_len > SL_OSC8_MAX_BYTES)
      return -2;
    if (state == 3) {
      if (byte != '8')
        return -2;
      surface->parser = 4;
    } else if (state == 4) {
      if (byte != ';')
        return -2;
      surface->parser = 5;
    } else if (state == 5) {
      if (byte == ';')
        surface->parser = 6;
      else if (byte < 0x20 || byte >= 0x7f)
        return -2;
    } else if (state == 7) {
      if (byte != '\\')
        return -2;
      surface->parser = 0;
      surface->osc8_open = surface->uri_len != 0;
    } else if (surface->osc_need) {
      if ((byte & 0xc0u) != 0x80u)
        return -2;
      surface->osc_cp = (surface->osc_cp << 6) | (byte & 0x3fu);
      if (!--surface->osc_need &&
          (surface->osc_cp < surface->osc_min ||
           (surface->osc_cp >= 0x80 && surface->osc_cp <= 0x9f) ||
           (surface->osc_cp >= 0xd800 && surface->osc_cp <= 0xdfff) ||
           surface->osc_cp > 0x10ffff))
        return -2;
      ++surface->uri_len;
    } else if (byte == 0x07 || byte == 0x1b) {
      surface->parser = byte == 0x07 ? 0 : 7;
      if (!surface->parser)
        surface->osc8_open = surface->uri_len != 0;
    } else if (byte >= 0x20 && byte < 0x7f)
      ++surface->uri_len;
    else if (byte >= 0xc2 && byte <= 0xf4) {
      surface->osc_need = byte < 0xe0 ? 1 : byte < 0xf0 ? 2 : 3;
      surface->osc_min = byte < 0xe0 ? 0x80 : byte < 0xf0 ? 0x800 : 0x10000;
      surface->osc_cp = byte & (byte < 0xe0 ? 0x1f : byte < 0xf0 ? 0x0f : 7);
      ++surface->uri_len;
    } else
      return -2;
    return 0;
  }
  if (surface->parser == 1) {
    if (byte == ']' && surface->osc8_enabled) {
      surface->parser = 3;
      surface->osc_len = 2;
      surface->uri_len = surface->osc_need = 0;
      return 0;
    }
    if (byte != '[') {
      surface->parser = 0;
      return -2;
    }
    surface->parser = 2;
    surface->csi_len = 0;
    return 0;
  }
  if (surface->parser == 2) {
    if (byte == 'm') {
      surface->parser = 0;
      return sl_surface_parse_sgr(surface);
    }
    if (byte != ';' && (byte < '0' || byte > '9')) {
      surface->parser = 0;
      return -2;
    }
    if (surface->csi_len >= sizeof(surface->csi)) {
      surface->parser = 0;
      return -2;
    }
    surface->csi[surface->csi_len++] = (char)byte;
    return 0;
  }
  if (surface->utf8_need != 0) {
    if ((byte & 0xc0u) != 0x80u || surface->utf8_len >= sizeof(surface->utf8)) {
      surface->utf8_need = 0;
      surface->utf8_len = 0;
      return -2;
    }
    surface->utf8[surface->utf8_len++] = (char)byte;
    if (surface->utf8_len < surface->utf8_need)
      return 0;
    cp = (unsigned char)surface->utf8[0] & (surface->utf8_need == 2   ? 0x1fu
                                            : surface->utf8_need == 3 ? 0x0fu
                                                                      : 0x07u);
    for (i = 1; i < surface->utf8_need; i++)
      cp = (cp << 6) | ((unsigned char)surface->utf8[i] & 0x3fu);
    if ((surface->utf8_need == 2 && cp < 0x80ul) ||
        (surface->utf8_need == 3 && cp < 0x800ul) ||
        (surface->utf8_need == 4 && cp < 0x10000ul) ||
        (cp >= 0x80ul && cp <= 0x9ful) || (cp >= 0xd800ul && cp <= 0xdffful) ||
        cp > 0x10fffful) {
      surface->utf8_need = 0;
      surface->utf8_len = 0;
      return -2;
    }
    surface->utf8_need = 0;
    surface->utf8_len = 0;
    if (surface->tracking) {
      surface->ascii_line = 0;
      sl_surface_put_cells(surface, sl_codepoint_width(cp));
    }
    return 0;
  }
  if (byte == 0x1bu) {
    surface->parser = 1;
    return 0;
  }
  if (byte == '\n') {
    if (surface->tracking) {
      surface->write_line_control = 1;
      if (newline_returns)
        surface->col = 0;
      else if (surface->col >= surface->width)
        surface->col = surface->width - 1;
      surface->line_cells = (size_t)surface->col;
      surface->line_extent = surface->line_cells;
      surface->ascii_line = 1;
      surface->line_open = 0;
      if (SL_ROW(surface) + 1 < surface->height)
        surface->producer_below--;
      else
        sl_surface_native_history_shift(surface, -1);
    }
    return 0;
  }
  if (byte == '\r') {
    if (surface->tracking) {
      surface->write_line_control = 1;
      /* CR returns to the start of the current physical row. Earlier soft
       * wraps still belong to this logical line after terminal reflow. */
      surface->line_cells = surface->line_cells >= (size_t)surface->col
                                ? surface->line_cells - (size_t)surface->col
                                : 0;
      surface->col = 0;
    }
    return 0;
  }
  if (byte == '\t') {
    if (surface->tracking) {
      int next = surface->col + 8 - (surface->col % 8);
      if (next >= surface->width)
        next = surface->width - 1;
      surface->ascii_line = 0;
      /* HT cancels pending wrap, so a full-row endpoint can move back one. */
      surface->line_cells =
          surface->line_cells - (size_t)surface->col + (size_t)next;
      if (surface->line_cells > surface->line_extent)
        surface->line_extent = surface->line_cells;
      surface->col = next;
    }
    return 0;
  }
  if (byte < 32u || byte == 127u)
    return -2;
  if (byte < 128u) {
    return sl_surface_put_cells(surface, 1);
  }
  if (byte >= 0xc2u && byte <= 0xdfu)
    surface->utf8_need = 2;
  else if (byte >= 0xe0u && byte <= 0xefu)
    surface->utf8_need = 3;
  else if (byte >= 0xf0u && byte <= 0xf4u)
    surface->utf8_need = 4;
  else
    return -2;
  surface->utf8[0] = (char)byte;
  surface->utf8_len = 1;
  return 0;
}

sl_surface_t *sl_surface_create_validator(int fd) {
  sl_surface_t *surface = calloc(1, sizeof(*surface));
  if (surface)
    surface->fd = fd;
  return surface;
}

int sl_surface_is_native(const sl_surface_t *surface) {
  return surface && surface->native_stream;
}

int sl_surface_native_restore_region(sl_surface_t *surface) {
  char seq[48];
  int count;
  if (!sl_surface_is_native(surface) || surface->height < 2)
    return -1;
  count = snprintf(seq, sizeof(seq), "\033[1;%dr", surface->height);
  return count > 0 && count < (int)sizeof(seq)
             ? sl_surface_write_all(surface->fd, seq, (size_t)count)
             : -1;
}

/* Address the physical bottom before moving up. CUD inside DECSTBM cannot
 * cross its bottom margin, so returning to the prompt needs this anchor. */
static int sl_surface_cursor(char *seq, size_t capacity, int below, int col) {
  int count;
  count = snprintf(seq, capacity, "\033[65535;%dH", col + 1);
  if (below > 0 && count > 0 && (size_t)count < capacity)
    count += snprintf(seq + count, capacity - (size_t)count, "\033[%dA", below);
  return count > 0 && (size_t)count < capacity ? count : -1;
}

static int sl_surface_style(const sl_surface_t *surface, char *seq,
                            size_t capacity) {
  int count = snprintf(seq, capacity, "\033[0");
  int i;
  for (i = 1; i <= 9; i++) {
    if (surface->attributes & (1u << i))
      count += snprintf(seq + count, capacity - (size_t)count, ";%d", i);
  }
  for (i = 0; i < 2; i++) {
    const int *color = i == 0 ? surface->foreground : surface->background;
    if (color[0] == 1)
      count += snprintf(seq + count, capacity - (size_t)count, ";%d", color[1]);
    else if (color[0] == 5)
      count += snprintf(seq + count, capacity - (size_t)count, ";%d;5;%d",
                        i == 0 ? 38 : 48, color[1]);
    else if (color[0] == 2)
      count += snprintf(seq + count, capacity - (size_t)count, ";%d;2;%d;%d;%d",
                        i == 0 ? 38 : 48, color[1], color[2], color[3]);
  }
  if (count <= 0 || (size_t)count + 1 >= capacity)
    return -1;
  seq[count++] = 'm';
  return count;
}

sl_surface_t *sl_surface_create_native(int fd, int width, int height, int row,
                                       int col, int history_rows) {
  sl_surface_t *surface = sl_surface_create_validator(fd);
  struct winsize terminal;
  char seq[128];
  int count, shift;
  if (!surface)
    return NULL;
  surface->fd = fd;
  surface->width = width;
  surface->height = height;
  surface->native_stream = 1;
  surface->col = col;
  surface->ascii_line = 1;
  if (sl_surface_terminal_size(fd, &terminal) != 0)
    goto fail;
  surface->terminal_rows = terminal.ws_row;
  surface->terminal_columns = terminal.ws_col;
  /* Row zero is only a fallback estimate of empty history, never proof: ED
   * followed by CUP preserves real terminal scrollback. An input-owned resize
   * observes the actual cursor; a feed cannot resolve this ambiguity without
   * a terminal wait. See the unobserved retained-frame resize review exception
   * in docs/softline-mdf-stream-design.md. No scrollback is kept here. */
  surface->history_rows = history_rows < 0 && row == 0 ? 0 : history_rows;
  shift = row >= height ? row - height + 1 : 0;
  surface->producer_below = surface->terminal_rows - 1 - row + shift;
  if (shift > 0) {
    sl_surface_native_history_shift(surface, -shift);
    count = snprintf(seq, sizeof(seq), "\033[r\033[%dS", shift);
    if (sl_surface_write_all(fd, seq, (size_t)count) != 0)
      goto fail;
  }
  /* A full-height producer already owns the hardware cursor. Margin and
   * cursor commands would discard pending wrap from earlier terminal text. */
  if (height != surface->terminal_rows) {
    count = sl_surface_cursor(seq, sizeof(seq), surface->producer_below, col);
    if (sl_surface_native_restore_region(surface) != 0 || count < 0 ||
        sl_surface_write_all(fd, seq, (size_t)count) != 0)
      goto fail;
  }
  if (sl_surface_write_all(fd, "\033[0m", 4) != 0)
    goto fail;
  surface->line_cells = (size_t)col;
  surface->line_extent = surface->line_cells;
  surface->line_open = col > 0;
  surface->producer_cursor_live = 1;
  return surface;
fail:
  surface->native_stream = 0;
  sl_surface_destroy(surface);
  return NULL;
}

void sl_surface_native_position(const sl_surface_t *surface, int *row,
                                int *col) {
  if (row)
    *row = SL_ROW(surface);
  if (col)
    *col = surface->col;
}

int sl_surface_native_exit_advances(const sl_surface_t *surface) {
  return surface->col > 0 || surface->line_open;
}

/* Apply an observed bottom-relative cursor delta. For an input cursor the
 * caller excludes reflow inside its prompt. Transcript cells stay terminal
 * owned; no text is retained or moved here. */
void sl_surface_native_prompt_reflow(sl_surface_t *surface, int extra_rows) {
  if (sl_surface_is_native(surface))
    surface->producer_below += extra_rows;
}

int sl_surface_native_reflow_rows(const sl_surface_t *surface, int width) {
  if (!sl_surface_is_native(surface) || !surface->ascii_line ||
      surface->line_extent == 0 || width < 1)
    return 0;
  return (int)((surface->line_extent - 1) / (size_t)width) -
         (int)((surface->line_extent - 1) / (size_t)surface->width);
}

int sl_surface_native_history_rows(const sl_surface_t *surface) {
  return surface->history_rows;
}

int sl_surface_native_history_growth(const sl_surface_t *surface, int rows) {
  return surface->history_rows >= 0 && surface->history_rows < rows
             ? surface->history_rows
             : rows;
}

void sl_surface_native_history_shift(sl_surface_t *surface, int rows) {
  if (surface->history_rows < 0)
    return;
  surface->history_rows -= rows;
  if (surface->history_rows < 0)
    surface->history_rows = 0;
  if (surface->history_rows > 65535)
    surface->history_rows = 65535;
}

/* SGR alone must not consume a pending wrap, nor may a following LF wrap
 * twice. Find the first text character without modifying producer bytes. */
static int sl_surface_continues_row(const char *bytes, size_t length) {
  size_t i = 0;
  while (i < length) {
    unsigned char ch = (unsigned char)bytes[i++];
    if (ch == 27) {
      if (i < length && bytes[i++] == ']') {
        while (i < length) {
          ch = (unsigned char)bytes[i++];
          if (ch == 7)
            break;
          if (ch == 27 && i < length && bytes[i] == '\\') {
            ++i;
            break;
          }
        }
      } else {
        while (i < length && bytes[i++] != 'm')
          ;
      }
      continue;
    }
    return ch != '\n' && ch != '\r' && ch != '\t';
  }
  return 0;
}

/* Width reflow can invalidate a clipped endpoint. Reset it at that boundary,
 * even if no feed follows before growth, rather than reviving a stale row
 * inside terminal-owned history. Height-only history positions remain valid
 * until a feed needs to address them. */
static void sl_surface_clip_producer(sl_surface_t *surface) {
  if (SL_ROW(surface) >= 0 || surface->producer_cursor_live)
    return;
  /* Continue an unfinished line at the top; start a new line by the prompt.
   * Discard only physical measurements: clipping consumes no newline. */
  surface->producer_below = surface->line_open
                                ? surface->terminal_rows - 1
                                : surface->terminal_rows - surface->height;
  surface->col = 0;
  surface->line_cells = 0;
  surface->line_extent = 0;
  surface->ascii_line = 1;
}

int sl_surface_native_write(sl_surface_t *surface, const char *bytes,
                            size_t length, int prompt_row, int prompt_col) {
  struct iovec parts[4];
  sl_surface_t next;
  char prefix[192], suffix[64];
  int count, position, style_count, suffix_count, first, newline_returns;
  struct termios attributes;
  size_t i, osc_start = 0;
  char *new_link = NULL;
  int link_changed = 0;
  if (!sl_surface_is_native(surface))
    return -1;
  if (tcgetattr(surface->fd, &attributes) != 0)
    return -1;
  /* Track the line discipline without changing the producer's bytes. */
  newline_returns =
      (attributes.c_oflag & OPOST) && (attributes.c_oflag & ONLCR);
  sl_surface_clip_producer(surface);
  count = 0;
  if (!surface->producer_cursor_live || prompt_row >= 0) {
    /* Resize can reset margins after its PTY size is already observable.
     * Establish the output region in every batch that positions its cursor. */
    count = snprintf(prefix, sizeof(prefix), "\033[1;%dr", surface->height);
    if (count <= 0 || (size_t)count >= sizeof(prefix))
      return -1;
    position = sl_surface_cursor(
        prefix + count, sizeof(prefix) - (size_t)count, surface->producer_below,
        surface->col < surface->width ? surface->col : surface->width - 1);
    if (position < 0)
      return -1;
    count += position;
    /* Cursor addressing cancels pending wrap. Resume on the following row when
     * a printable continuation follows a completely filled row. */
    if (surface->col == surface->width &&
        sl_surface_continues_row(bytes, length)) {
      memcpy(prefix + count, "\r\033D", 3);
      count += 3;
      surface->line_cells = 0;
      surface->line_extent = 0;
      surface->ascii_line = 1;
      surface->line_open = 0;
    }
  }
  style_count =
      sl_surface_style(surface, prefix + count, sizeof(prefix) - (size_t)count);
  if (style_count < 0)
    return -1;
  count += style_count;
  suffix_count = 0;
  next = *surface;
  next.write_line_control = 0;
  sl_surface_reset_partial(&next);
  next.tracking = 1;
  for (i = 0; i < length; i++) {
    int state = next.parser;
    if (state == 1 && bytes[i] == ']')
      osc_start = i - 1;
    if (sl_surface_byte(&next, (unsigned char)bytes[i], newline_returns) != 0)
      goto failed;
    if (state >= 6 && !next.parser) {
      free(new_link);
      new_link = NULL;
      link_changed = 1;
      if (next.osc8_open) {
        size_t size = i - osc_start + 1;
        new_link = malloc(size + 1);
        if (!new_link)
          goto failed;
        memcpy(new_link, bytes + osc_start, size);
        new_link[size] = '\0';
      }
    }
  }
  if (link_changed ? new_link != NULL : surface->hyperlink != NULL) {
    memcpy(suffix, "\033]8;;\033\\", 7);
    suffix_count = 7;
  }
  memcpy(suffix + suffix_count, "\033[0m", 4);
  suffix_count += 4;
  if (prompt_row >= 0) {
    int position = sl_surface_cursor(
        suffix + suffix_count, sizeof(suffix) - (size_t)suffix_count,
        surface->terminal_rows - 1 - prompt_row, prompt_col);
    if (position < 0)
      goto failed;
    suffix_count += position;
  }
  parts[0].iov_base = prefix;
  parts[0].iov_len = (size_t)count;
  parts[1].iov_base = surface->hyperlink;
  parts[1].iov_len = surface->hyperlink ? strlen(surface->hyperlink) : 0;
  parts[2].iov_base = (void *)bytes;
  parts[2].iov_len = length;
  parts[3].iov_base = suffix;
  parts[3].iov_len = (size_t)suffix_count;
  first = 0;
  while (first < 4) {
    ssize_t written = writev(surface->fd, parts + first, 4 - first);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      surface->producer_cursor_live = 0;
      goto failed;
    }
    while (first < 4 && (size_t)written >= parts[first].iov_len) {
      written -= (ssize_t)parts[first].iov_len;
      first++;
    }
    if (first < 4) {
      parts[first].iov_base = (char *)parts[first].iov_base + written;
      parts[first].iov_len -= (size_t)written;
    }
  }
  surface->producer_cursor_live = prompt_row < 0;
  surface->producer_below = next.producer_below;
  surface->history_rows = next.history_rows;
  surface->col = next.col;
  surface->line_cells = next.line_cells;
  surface->line_extent = next.line_extent;
  surface->ascii_line = next.ascii_line;
  surface->line_open = next.line_open;
  surface->write_line_control = next.write_line_control;
  surface->attributes = next.attributes;
  memcpy(surface->foreground, next.foreground, sizeof(next.foreground));
  memcpy(surface->background, next.background, sizeof(next.background));
  if (link_changed) {
    free(surface->hyperlink);
    surface->hyperlink = new_link;
  }
  return 0;
failed:
  free(new_link);
  return -1;
}

int sl_surface_native_finish(sl_surface_t *surface, int prompt_row) {
  char seq[96];
  int count, position;
  if (!sl_surface_is_native(surface))
    return -1;
  memcpy(seq, "\033[r\033[0m", 7);
  count = 7;
  position = sl_surface_cursor(seq + count, sizeof(seq) - (size_t)count,
                               prompt_row >= 0
                                   ? surface->terminal_rows - 1 - prompt_row
                                   : surface->producer_below,
                               0);
  if (position < 0)
    return -1;
  count += position;
  if (prompt_row < 0 && sl_surface_native_exit_advances(surface))
    seq[count++] = '\n';
  memcpy(seq + count, "\033[?25h", 6);
  count += 6;
  if (sl_surface_write_all(surface->fd, seq, (size_t)count) != 0)
    return -1;
  if (prompt_row < 0 && sl_surface_native_exit_advances(surface) &&
      SL_ROW(surface) >= surface->terminal_rows - 1)
    sl_surface_native_history_shift(surface, -1);
  surface->native_stream = 0;
  surface->osc8_open = 0;
  return 0;
}

void sl_surface_destroy(sl_surface_t *surface) {
  if (!surface)
    return;
  if (surface->native_stream)
    (void)sl_surface_native_finish(surface, -1);
  else if (surface->osc8_open)
    (void)sl_surface_write_all(surface->fd, "\033]8;;\033\\", 7);
  free(surface->hyperlink);
  free(surface);
}

int sl_surface_matches(const sl_surface_t *surface, int width, int height) {
  struct winsize terminal;
  return surface && surface->width == width && surface->height == height &&
         sl_surface_terminal_size(surface->fd, &terminal) == 0 &&
         surface->terminal_rows == (int)terminal.ws_row &&
         surface->terminal_columns == (int)terminal.ws_col;
}

void sl_surface_geometry(const sl_surface_t *surface, int *width, int *height,
                         int *terminal_rows) {
  if (!surface)
    return;
  if (width)
    *width = surface->width;
  if (height)
    *height = surface->height;
  if (terminal_rows)
    *terminal_rows = surface->terminal_rows;
}

int sl_surface_native_resize_pending(const sl_surface_t *surface) {
  struct winsize terminal;
  return surface && surface->native_stream &&
         surface->width == surface->terminal_columns &&
         sl_surface_terminal_size(surface->fd, &terminal) == 0 &&
         ((int)terminal.ws_col != surface->terminal_columns ||
          (int)terminal.ws_row != surface->terminal_rows);
}

int sl_surface_resize(sl_surface_t *surface, int width, int height) {
  struct winsize terminal;
  char seq[64];
  int shift, count, previous_height, retain_cursor, width_changed;
  if (!sl_surface_is_native(surface) || width < 1 || height < 2 ||
      sl_surface_terminal_size(surface->fd, &terminal) != 0)
    return -1;
  if (surface->width == width && surface->height == height &&
      surface->terminal_rows == (int)terminal.ws_row &&
      surface->terminal_columns == (int)terminal.ws_col)
    return 0;
  /* A full-height region follows native terminal resize. Keep its live
   * producer cursor and pending wrap; no margin command is necessary. */
  retain_cursor = surface->producer_cursor_live &&
                  surface->height == surface->terminal_rows &&
                  height == (int)terminal.ws_row;
  width_changed = surface->width != width;
  previous_height =
      surface->height + (int)terminal.ws_row - surface->terminal_rows;
  if (previous_height < 2)
    previous_height = 2;
  if (previous_height > (int)terminal.ws_row)
    previous_height = terminal.ws_row;
  surface->terminal_rows = terminal.ws_row;
  surface->terminal_columns = terminal.ws_col;
  if (width_changed) {
    /* Deliberate opaque-feed limit: Unicode and tabs have no span or reflow
     * model here. Keep the tracked endpoint, clamping only an offscreen
     * column. Their reflow, and CR tails containing them, are terminal-owned.
     * See docs/softline-mdf-stream-design.md, "Review exception: opaque feed".
     */
    if (surface->ascii_line && surface->line_extent > 0) {
      if (surface->line_extent > surface->line_cells) {
        size_t before_cursor = (surface->line_cells - (size_t)surface->col) /
                               (size_t)surface->width;
        size_t before_end = (surface->line_extent - 1) / (size_t)surface->width;
        size_t after_cursor = surface->line_cells / (size_t)width;
        size_t after_end = (surface->line_extent - 1) / (size_t)width;
        surface->producer_below +=
            (int)(after_end - after_cursor) - (int)(before_end - before_cursor);
      }
      surface->col = (int)(surface->line_cells % (size_t)width);
      if (surface->col == 0 && surface->line_cells > 0 &&
          surface->line_extent == surface->line_cells)
        surface->col = width;
    } else if (surface->col >= width)
      surface->col = width - 1;
  }
  shift = SL_ROW(surface) >= height ? SL_ROW(surface) - height + 1 : 0;
  if (shift > 0) {
    /* Only actual prompt growth reserves additional cells. The physical
     * resize has already moved the terminal's transcript. */
    if (height < previous_height) {
      count = snprintf(seq, sizeof(seq), "\033[1;%dr\033[%dS", previous_height,
                       shift);
      if (count <= 0 || count >= (int)sizeof(seq) ||
          sl_surface_write_all(surface->fd, seq, (size_t)count) != 0)
        return -1;
      sl_surface_native_history_shift(surface, -shift);
    }
    surface->producer_below += shift;
  }
  surface->width = width;
  surface->height = height;
  if (retain_cursor)
    return 0;
  surface->producer_cursor_live = 0;
  if (width_changed)
    sl_surface_clip_producer(surface);
  /* The terminal may reset margins during resize. Reserve the prompt's
   * region immediately, including when no producer bytes follow. */
  return sl_surface_native_restore_region(surface);
}

int sl_surface_validate(sl_surface_t *surface, const char *bytes, size_t length,
                        size_t *accepted) {
  size_t i;
  int status;
  if (!surface || (!bytes && length > 0) || !accepted)
    return -2;
  *accepted = 0;
  for (i = 0; i < length; i++) {
    status = sl_surface_byte(surface, (unsigned char)bytes[i], 0);
    if (status != 0) {
      *accepted = i;
      return status;
    }
  }
  *accepted = length;
  return 0;
}

int sl_surface_complete(const sl_surface_t *surface) {
  return surface && surface->parser == 0 && surface->utf8_need == 0;
}

void sl_surface_reset_partial(sl_surface_t *surface) {
  if (!surface)
    return;
  surface->parser = 0;
  surface->csi_len = 0;
  surface->osc_need = 0;
  surface->utf8_len = 0;
  surface->utf8_need = 0;
}

void sl_surface_set_osc8(sl_surface_t *surface, int enabled) {
  if (surface)
    surface->osc8_enabled = enabled;
}

int sl_surface_hyperlink_active(const sl_surface_t *surface) {
  return surface && (surface->native_stream ? surface->hyperlink != NULL
                                            : surface->osc8_open);
}

int sl_surface_native_cursor_live(const sl_surface_t *surface) {
  return sl_surface_is_native(surface) && surface->producer_cursor_live;
}
void sl_surface_native_observe(sl_surface_t *surface, int width, int rows,
                               int row, int col) {
  if (!sl_surface_is_native(surface))
    return;
  if (surface->width == width && surface->terminal_rows == rows) {
    /* The initial producer handoff needs the same logical-column correction
     * as an observed write. A physical resize has already moved the viewport,
     * so its row delta must not be counted as emitted text. */
    sl_surface_native_observe_write(surface, row, col);
    return;
  }
  surface->producer_below = rows - 1 - row;
  /* CPR cannot distinguish a known pending wrap from the final physical cell.
   */
  if (!(surface->width == width && surface->col == width && col == width - 1))
    surface->col = col;
}

void sl_surface_native_observe_write(sl_surface_t *surface, int row, int col) {
  size_t predicted, observed;
  int pending_wrap;
  if (!sl_surface_is_native(surface) || row < 0 ||
      row >= surface->terminal_rows || col < 0 || col >= surface->width)
    return;
  /* CPR reports the final physical cell for a known pending wrap. Preserve
   * that logical column while still correcting any observed row difference. */
  pending_wrap = surface->ascii_line && surface->col == surface->width &&
                 col == surface->width - 1;
  predicted = (size_t)surface->col;
  observed = (size_t)(pending_wrap ? surface->width : col);
  /* After a line control, correct only the trailing line's column. */
  if (!surface->write_line_control && SL_ROW(surface) >= 0) {
    predicted += (size_t)SL_ROW(surface) * (size_t)surface->width;
    observed += (size_t)row * (size_t)surface->width;
  }
  if (surface->ascii_line) {
    size_t previous_line_cells = surface->line_cells;
    if (observed < predicted) {
      size_t removed = predicted - observed;
      surface->line_cells = surface->line_cells >= removed
                                ? surface->line_cells - removed
                                : (size_t)col;
    } else
      surface->line_cells += observed - predicted;
    if (surface->line_extent == previous_line_cells ||
        surface->line_cells > surface->line_extent)
      surface->line_extent = surface->line_cells;
  } else {
    /* An aggregate report locates the cursor, not individual glyphs. Do not
     * infer Unicode widths or reconstruct a terminal-reflowed logical line. */
    surface->line_cells = (size_t)col;
    surface->line_extent = (size_t)col;
  }
  surface->producer_below = surface->terminal_rows - 1 - row;
  surface->col = pending_wrap ? surface->width : col;
}

void sl_surface_native_release_cursor(sl_surface_t *surface) {
  if (sl_surface_is_native(surface))
    surface->producer_cursor_live = 0;
}
