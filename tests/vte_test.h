#ifndef SOFTLINE_TEST_VTE_H
#define SOFTLINE_TEST_VTE_H
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Small native adapter for the real GTK/VTE test engine. It owns no product
 * parser: snapshots come from VTE, exactly as the terminal displays them. */
struct api {
  void (*init)(void *, void *);
  void *(*window)(int);
  void *(*terminal)(void);
  void (*add)(void *, void *);
  void (*show)(void *);
  void (*destroy)(void *);
  void (*resize)(void *, int, int);
  int (*width)(void *);
  int (*height)(void *);
  long (*char_width)(void *);
  long (*char_height)(void *);
  long (*columns)(void *);
  long (*rows)(void *);
  const char *(*title)(void *);
  void *(*adjustment)(void *);
  double (*adjustment_value)(void *);
  void (*size)(void *, long, long);
  void (*feed)(void *, const char *, long);
  void (*send)(void *, const char *, long);
  void *(*pty)(int, void *, void *);
  void *(*get_pty)(void *);
  void (*set_pty)(void *, void *);
  void (*cursor)(void *, long *, long *);
  char *(*text)(void *, long, long, long, long, void *, void *, void *);
  void (*free)(void *);
  void (*unref)(void *);
  int (*iterate)(void *, int);
};
static struct api v;
static int errors;

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

/* Polling callers own their readiness condition and deadline. One event-loop
 * turn must not impose a 30 ms settling delay on every successful check. */
static void pump_once(void) {
  struct timespec nap;
  int dispatched = 0;
  while (v.iterate(NULL, 0))
    dispatched = 1;
  nap.tv_sec = 0;
  nap.tv_nsec = 1000000;
  if (!dispatched)
    nanosleep(&nap, NULL);
}

static void pump(void) {
  double deadline = now() + 0.03;
  do {
    pump_once();
  } while (now() < deadline);
}

static int bind_symbol(void *library, const char *name, void *slot,
                       size_t size) {
  void *symbol = dlsym(library, name);
  if (!symbol || size != sizeof(symbol)) {
    fprintf(stderr, "VTE test symbol unavailable: %s (%s)\n", name, dlerror());
    return 1;
  }
  memcpy(slot, &symbol, size);
  return 0;
}
#define BIND(l, f, n)                                                          \
  do {                                                                         \
    if (bind_symbol(l, n, &v.f, sizeof(v.f)))                                  \
      return 1;                                                                \
  } while (0)
static int load(void) {
  void *gtk = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
  void *glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
  void *gobject = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
  void *vte = dlopen("libvte-2.91.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!gtk || !glib || !gobject || !vte) {
    fprintf(stderr, "Prepared GTK/VTE runtime unavailable: %s\n", dlerror());
    return 1;
  }
  BIND(gtk, init, "gtk_init");
  BIND(gtk, window, "gtk_window_new");
  BIND(gtk, add, "gtk_container_add");
  BIND(gtk, show, "gtk_widget_show_all");
  BIND(gtk, destroy, "gtk_widget_destroy");
  BIND(gtk, resize, "gtk_window_resize");
  BIND(gtk, width, "gtk_widget_get_allocated_width");
  BIND(gtk, height, "gtk_widget_get_allocated_height");
  BIND(gtk, adjustment, "gtk_scrollable_get_vadjustment");
  BIND(gtk, adjustment_value, "gtk_adjustment_get_value");
  BIND(vte, terminal, "vte_terminal_new");
  BIND(vte, char_width, "vte_terminal_get_char_width");
  BIND(vte, char_height, "vte_terminal_get_char_height");
  BIND(vte, columns, "vte_terminal_get_column_count");
  BIND(vte, rows, "vte_terminal_get_row_count");
  BIND(vte, title, "vte_terminal_get_window_title");
  BIND(vte, size, "vte_terminal_set_size");
  BIND(vte, feed, "vte_terminal_feed");
  BIND(vte, send, "vte_terminal_feed_child");
  BIND(vte, pty, "vte_pty_new_foreign_sync");
  BIND(vte, get_pty, "vte_terminal_get_pty");
  BIND(vte, set_pty, "vte_terminal_set_pty");
  BIND(vte, cursor, "vte_terminal_get_cursor_position");
  BIND(vte, text, "vte_terminal_get_text_range");
  BIND(glib, free, "g_free");
  BIND(glib, iterate, "g_main_context_iteration");
  BIND(gobject, unref, "g_object_unref");
  v.init(NULL, NULL);
  return 0;
}

struct screen {
  void *terminal, *window;
};
struct snapshot {
  char text[32768];
  long row, column;
};
static struct screen create(void) {
  struct screen s;
  double deadline = now() + 2;
  s.terminal = v.terminal();
  s.window = v.window(0);
  v.add(s.window, s.terminal);
  v.size(s.terminal, 40, 8);
  v.show(s.window);
  while (v.width(s.terminal) <= 1 || v.height(s.terminal) <= 1) {
    if (now() >= deadline) {
      fprintf(stderr, "VTE window did not receive its initial allocation\n");
      ++errors;
      break;
    }
    pump_once();
  }
  return s;
}
static void resize_geometry(struct screen *s, int columns, int rows) {
  int pixels = v.width(s->terminal) + (columns - (int)v.columns(s->terminal)) *
                                          (int)v.char_width(s->terminal);
  double deadline = now() + 2;
  int height = v.height(s->terminal) + (rows - (int)v.rows(s->terminal)) *
                                           (int)v.char_height(s->terminal);
  v.size(s->terminal, columns, rows);
  v.resize(s->window, pixels, height);
  do {
    pump_once();
  } while ((v.columns(s->terminal) != columns || v.rows(s->terminal) != rows) &&
           now() < deadline);
  if (v.columns(s->terminal) != columns || v.rows(s->terminal) != rows) {
    fprintf(stderr, "VTE resize did not settle\n");
    ++errors;
  }
}
static void resize(struct screen *s, int columns, int rows) {
  resize_geometry(s, columns, rows);
  pump();
}
static struct snapshot snapshot(struct screen *s, int handoff) {
  struct snapshot result;
  long row;
  size_t used = 0;
  memset(&result, 0, sizeof(result));
  v.cursor(s->terminal, &result.column, &result.row);
  for (row = 0; row <= result.row; ++row) {
    char *text = v.text(s->terminal, row, 0, row, v.columns(s->terminal), NULL,
                        NULL, NULL);
    if (text) {
      size_t count = strlen(text);
      if ((!handoff || count > 0) && used + count + 2 < sizeof(result.text)) {
        memcpy(result.text + used, text, count);
        used += count;
        result.text[used++] = '\n';
      }
      v.free(text);
    }
  }
  if (handoff) {
    result.row = -1;
    result.column = -1;
  }
  return result;
}
static struct snapshot transcript(struct screen *s) {
  struct snapshot result;
  long row, last = (long)v.adjustment_value(v.adjustment(s->terminal)) +
                   v.rows(s->terminal);
  size_t used = 0, end = 0;
  memset(&result, 0, sizeof(result));
  v.cursor(s->terminal, &result.column, &result.row);
  for (row = 0; row < last; ++row) {
    char *text = v.text(s->terminal, row, 0, row, v.columns(s->terminal), NULL,
                        NULL, NULL);
    size_t count;
    if (!text) {
      ++errors;
      continue;
    }
    count = strlen(text);
    if (used + count + 2 >= sizeof(result.text)) {
      fprintf(stderr, "Terminal transcript capture exhausted\n");
      ++errors;
      v.free(text);
      break;
    }
    memcpy(result.text + used, text, count);
    used += count;
    result.text[used++] = '\n';
    if (count)
      end = used;
    v.free(text);
  }
  result.text[end] = 0;
  return result;
}
#endif
