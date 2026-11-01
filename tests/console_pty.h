#ifndef SOFTLINE_TEST_CONSOLE_PTY_H
#define SOFTLINE_TEST_CONSOLE_PTY_H
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct editor {
  pid_t child;
  int master;
  char bytes[262144];
  size_t length, read_limit;
  int rows, columns, row, column, top, bottom;
  char escape[128], printed[65536];
  uint16_t print_rows[65536], print_columns[65536];
  unsigned char print_output[65536];
  size_t escape_length, printed_length;
};
static const char *executable;
static const char *root;
static int c_chat;
#include "remote_chat_owner.h"
static int test_rows = 8, test_columns = 40, test_echo;
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + t.tv_nsec / 1000000000.0;
}
static void close_editor(struct editor *e) {
  int status;
  if (e->child > 0) {
    pid_t foreground = tcgetpgrp(e->master);
    if (foreground > 0 && foreground != e->child)
      kill(-foreground, SIGTERM);
    if (kill(-e->child, SIGTERM) != 0)
      kill(e->child, SIGTERM);
    waitpid(e->child, &status, 0);
    e->child = -1;
  }
  if (e->master >= 0) {
    close(e->master);
    e->master = -1;
  }
  stop_remote_owner();
}
static int start(struct editor *e, const char *relative, const char *arg1,
                 const char *arg2) {
  int slave;
  struct winsize size;
  struct termios attributes;
  char script[4096];
  memset(e, 0, sizeof(*e));
  e->master = -1;
  e->child = -1;
  memset(&size, 0, sizeof(size));
  size.ws_row = (unsigned short)test_rows;
  size.ws_col = (unsigned short)test_columns;
  e->rows = test_rows;
  e->columns = test_columns;
  e->bottom = test_rows - 1;
  snprintf(script, sizeof(script), "%s/%s", root, relative);
  if (start_remote_owner())
    return 1;
  if (openpty(&e->master, &slave, NULL, NULL, &size))
    return 1;
  if (!test_echo && tcgetattr(slave, &attributes) == 0) {
    attributes.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(slave, TCSANOW, &attributes);
  }
  e->child = fork();
  if (e->child == 0) {
    close(e->master);
    setsid();
    ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
      close(slave);
    chdir(root);
    if (c_chat && remote_owner)
      execl(executable, executable, "127.0.0.1", remote_owner_port,
            (char *)NULL);
    else if (c_chat)
      execl(executable, executable, (char *)NULL);
    else if (arg2)
      execl(executable, executable, script, arg1, arg2, (char *)NULL);
    else if (arg1)
      execl(executable, executable, script, arg1, (char *)NULL);
    else
      execl(executable, executable, script, (char *)NULL);
    _exit(127);
  }
  close(slave);
  return e->child < 0;
}
/* Only cursor/margin controls used by these ASCII shell interactions are
 * modeled. Graphic output/reflow assertions use the real VTE adapter. */
static int consume(struct editor *e, const char *bytes, size_t length) {
  size_t index;
  if (e->rows == 0)
    return 0;
  for (index = 0; index < length; ++index) {
    unsigned char byte = (unsigned char)bytes[index];
    if (e->escape_length) {
      int values[8] = {0}, count, n = 0;
      char *parameter, *next;
      if (e->escape_length + 1 >= sizeof(e->escape))
        return -1;
      e->escape[e->escape_length++] = (char)byte;
      if (e->escape_length == 2 && byte != '[') {
        if (byte == 'D' && e->row < e->bottom)
          ++e->row;
        e->escape_length = 0;
      } else if (e->escape_length > 2 && byte >= 0x40 && byte <= 0x7e) {
        e->escape[e->escape_length - 1] = 0;
        parameter = e->escape + 2;
        if (*parameter != '?') {
          do {
            values[n++] = (int)strtol(parameter, &next, 10);
            parameter = *next == ';' ? next + 1 : next;
          } while (*parameter && n < 8);
          count = values[0] ? values[0] : 1;
          if (byte == 'H' || byte == 'f') {
            e->row = count <= e->rows ? count - 1 : e->rows - 1;
            e->column = values[1] ? values[1] - 1 : 0;
            if (e->column >= e->columns)
              e->column = e->columns - 1;
          } else if (byte == 'A') {
            e->row -= count;
            if (e->row < 0)
              e->row = 0;
          } else if (byte == 'B') {
            e->row += count;
            if (e->row >= e->rows)
              e->row = e->rows - 1;
          } else if (byte == 'C') {
            e->column += count;
            if (e->column >= e->columns)
              e->column = e->columns - 1;
          } else if (byte == 'D') {
            e->column -= count;
            if (e->column < 0)
              e->column = 0;
          } else if (byte == 'r') {
            e->top = count - 1;
            e->bottom = values[1] ? values[1] - 1 : e->rows - 1;
            e->row = e->column = 0;
          } else if (byte == 'n' && count == 6) {
            char reply[64];
            int size =
                snprintf(reply, sizeof(reply), "\033[%d;%dR", e->row + 1,
                         e->column < e->columns ? e->column + 1 : e->columns);
            if (write(e->master, reply, (size_t)size) != size)
              return -1;
          }
        }
        e->escape_length = 0;
      }
    } else if (byte == 27) {
      e->escape[0] = 27;
      e->escape_length = 1;
    } else if (byte == 13) {
      e->column = 0;
    } else if (byte == 10) {
      int limit =
          e->row >= e->top && e->row <= e->bottom ? e->bottom : e->rows - 1;
      if (e->row < limit)
        ++e->row;
    } else if (byte >= 32 && byte < 127) {
      size_t at = e->printed_length;
      if (at + 1 >= sizeof(e->printed))
        return -1;
      if (e->column >= e->columns) {
        if (e->row < e->bottom)
          ++e->row;
        e->column = 0;
      }
      e->printed[at] = (char)byte;
      e->print_rows[at] = (uint16_t)e->row;
      e->print_columns[at] = (uint16_t)e->column;
      e->print_output[at] = (unsigned char)(e->row <= e->bottom);
      e->printed[++e->printed_length] = 0;
      ++e->column;
    }
  }
  return 0;
}
static int pump(struct editor *e, int timeout) {
  struct pollfd descriptor;
  size_t capacity;
  ssize_t bytes;
  descriptor.fd = e->master;
  descriptor.events = POLLIN;
  if (poll(&descriptor, 1, timeout) <= 0)
    return 0;
  if (e->length + 1 == sizeof(e->bytes)) {
    fprintf(stderr, "Console PTY capture exhausted\n");
    return -1;
  }
  capacity = sizeof(e->bytes) - e->length - 1;
  if (e->read_limit && capacity > e->read_limit)
    capacity = e->read_limit;
  bytes = read(e->master, e->bytes + e->length, capacity);
  if (bytes > 0) {
    if (consume(e, e->bytes + e->length, (size_t)bytes))
      return -1;
    e->length += (size_t)bytes;
    e->bytes[e->length] = 0;
    return 0;
  }
  return bytes == 0 || errno == EIO ? 1 : -1;
}
static int contains(struct editor *e, const char *needle, size_t offset) {
  char plain[262144];
  size_t i = offset, used = 0;
  if (strchr(needle, '\033'))
    return strstr(e->bytes + offset, needle) != NULL;
  while (i < e->length && used + 1 < sizeof(plain)) {
    if (e->bytes[i] == '\033') {
      ++i;
      if (i < e->length && e->bytes[i] == '[') {
        ++i;
        while (i < e->length && !((unsigned char)e->bytes[i] >= 0x40 &&
                                  (unsigned char)e->bytes[i] <= 0x7e))
          ++i;
        if (i < e->length)
          ++i;
      } else if (i < e->length)
        ++i;
    } else
      plain[used++] = e->bytes[i++];
  }
  plain[used] = 0;
  return strstr(plain, needle) != NULL;
}
static int wait_for(struct editor *e, const char *needle, size_t offset) {
  double deadline = now() + 10;
  do {
    if (contains(e, needle, offset))
      return 0;
    if (pump(e, 20))
      break;
  } while (now() < deadline);
  fprintf(stderr, "Missing Console PTY marker: %s\n%s\n", needle, e->bytes);
  return 1;
}
static int send_bytes(struct editor *e, const char *bytes) {
  size_t length = strlen(bytes);
  return write(e->master, bytes, length) != (ssize_t)length;
}
static int finish(struct editor *e) {
  double deadline = now() + 10;
  int status, drained;
  pid_t result;
  do {
    result = waitpid(e->child, &status, WNOHANG);
    if (result == e->child) {
      e->child = -1;
      do {
        drained = pump(e, 20);
        if (drained)
          return drained < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
      } while (now() < deadline);
      fprintf(stderr, "Console capture did not close after exit\n");
      return 1;
    }
    pump(e, 20);
  } while (now() < deadline);
  fprintf(stderr, "Console editor failed to exit\n%s\n", e->bytes);
  return 1;
}
#define REQUIRE(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "Console PTY assertion failed: %s\n%s\n", #c, e.bytes);  \
      failed = 1;                                                              \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
static int records(const char *path, const char *expected) {
  char bytes[4096];
  size_t length;
  FILE *file = fopen(path, "rb");
  if (!file)
    return !*expected;
  length = fread(bytes, 1, sizeof(bytes) - 1, file);
  if (ferror(file) || length == sizeof(bytes) - 1) {
    fclose(file);
    return 0;
  }
  fclose(file);
  bytes[length] = 0;
  return !strcmp(bytes, expected);
}
static int wait_records(struct editor *e, const char *path,
                        const char *expected) {
  double deadline = now() + 2;
  do {
    if (records(path, expected))
      return 0;
    pump(e, 20);
  } while (now() < deadline);
  fprintf(stderr, "Chat history was not durable at acceptance: %s\n", path);
  return 1;
}
static int plain_input(struct editor *e, const char *bytes, const char *theme) {
  int input[2], output[2];
  char script[4096];
  memset(e, 0, sizeof(*e));
  e->child = -1;
  e->master = -1;
  if (pipe(input))
    return 1;
  if (pipe(output)) {
    close(input[0]);
    close(input[1]);
    return 1;
  }
  snprintf(script, sizeof(script), "%s/examples/chat.lua", root);
  e->child = fork();
  if (e->child == 0) {
    setsid();
    close(input[1]);
    close(output[0]);
    dup2(input[0], 0);
    dup2(output[1], 1);
    dup2(output[1], 2);
    close(input[0]);
    close(output[1]);
    chdir(root);
    setenv("SOFTLINE_PROMPT_THEME", theme, 1);
    if (c_chat)
      execl(executable, executable, (char *)NULL);
    else
      execl(executable, executable, script, (char *)NULL);
    _exit(127);
  }
  close(input[0]);
  close(output[1]);
  e->master = output[0];
  if (write(input[1], bytes, strlen(bytes)) != (ssize_t)strlen(bytes)) {
    close(input[1]);
    return 1;
  }
  close(input[1]);
  return e->child < 0 || finish(e);
}
#endif
