#ifndef SOFTLINE_TEST_TMUX_H
#define SOFTLINE_TEST_TMUX_H
#define _POSIX_C_SOURCE 200809L
#include "terminal_resize_protocol.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct terminal {
  const char *tmux;
  char work[4096], commands[4096], acks[4096], recording[4096];
  char pane[32];
  int command_fd, ack_fd, line_open, expected_prompt;
  int control_fd;
  pid_t control_pid;
  char control_buffer[4096];
  size_t control_at, control_used;
  unsigned long fixture_sequence;
  unsigned char reply[6];
};
struct grid {
  char text[65536];
  char *rows[512];
  int count;
};
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + t.tv_nsec / 1000000000.0;
}
static void delay(double seconds) {
  struct timespec nap;
  nap.tv_sec = (time_t)seconds;
  nap.tv_nsec = (long)((seconds - nap.tv_sec) * 1000000000.0);
  while (nanosleep(&nap, &nap) && errno == EINTR) {
  }
}
/* Each synchronous call is only to this fixture's tmux socket. */
static int command(struct terminal *t, char *output, size_t capacity, ...) {
  const char *args[48], *arg;
  va_list input;
  int pipe_fd[2], status, index = 0, eof = 0, failed = 1;
  pid_t child, waited;
  size_t used = 0;
  ssize_t count;
  struct pollfd descriptor;
  double deadline;
  args[index++] = t->tmux;
  args[index++] = "-S";
  args[index++] = "socket";
  args[index++] = "-f";
  args[index++] = "/dev/null";
  va_start(input, capacity);
  while ((arg = va_arg(input, const char *)) != NULL && index < 47)
    args[index++] = arg;
  va_end(input);
  args[index] = NULL;
  if (pipe(pipe_fd))
    return 1;
  child = fork();
  if (child == 0) {
    close(pipe_fd[0]);
    dup2(pipe_fd[1], 1);
    close(pipe_fd[1]);
    /* Bind relative to owned scratch, including deeply extracted sources. */
    if (chdir(t->work))
      _exit(127);
    execv(t->tmux, (char *const *)args);
    _exit(127);
  }
  close(pipe_fd[1]);
  if (child < 0) {
    close(pipe_fd[0]);
    return 1;
  }
  descriptor.fd = pipe_fd[0];
  descriptor.events = POLLIN;
  deadline = now() + 5;
  do {
    if (poll(&descriptor, 1, 20) < 0)
      break;
    if (descriptor.revents & (POLLIN | POLLHUP)) {
      char discarded[4096];
      count = read(pipe_fd[0], output ? output + used : discarded,
                   output ? capacity - used - 1 : sizeof(discarded));
      if (count == 0) {
        eof = 1;
        break;
      }
      if (count < 0)
        break;
      if (output) {
        used += (size_t)count;
        if (used + 1 == capacity)
          break;
      }
    }
  } while (now() < deadline);
  if (output)
    output[used] = 0;
  waited = waitpid(child, &status, WNOHANG);
  while (eof && waited == 0 && now() < deadline) {
    delay(.001);
    waited = waitpid(child, &status, WNOHANG);
  }
  if (waited == child)
    failed = !eof || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
  else if (waited == 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  close(pipe_fd[0]);
  return failed;
}
static int control_line(struct terminal *t, char *line, size_t capacity,
                        double deadline) {
  size_t used = 0;
  while (used + 1 < capacity) {
    if (t->control_at == t->control_used) {
      struct pollfd wait;
      double left = deadline - now();
      int ready;
      ssize_t count;
      if (left <= 0)
        return 1;
      wait.fd = t->control_fd;
      wait.events = POLLIN;
      ready = poll(&wait, 1, (int)(left * 1000) + 1);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready <= 0)
        return 1;
      count = read(t->control_fd, t->control_buffer, sizeof(t->control_buffer));
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        return 1;
      t->control_at = 0;
      t->control_used = (size_t)count;
    }
    line[used++] = t->control_buffer[t->control_at++];
    if (line[used - 1] == '\n') {
      line[used] = 0;
      return 0;
    }
  }
  return 1;
}

/* Notifications occur outside command replies. Match the frame ID so that
 * a line of captured terminal text cannot accidentally close the reply. */
static int control_reply(struct terminal *t, char *output, size_t capacity) {
  char line[8192], id[96];
  double deadline = now() + 5;
  size_t used = 0;
  do {
    if (control_line(t, line, sizeof(line), deadline) ||
        !strncmp(line, "%exit", 5))
      return 1;
  } while (strncmp(line, "%begin ", 7));
  if (strlen(line + 7) >= sizeof(id))
    return 1;
  strcpy(id, line + 7);
  while (!control_line(t, line, sizeof(line), deadline)) {
    size_t length = strlen(line);
    if (!strncmp(line, "%end ", 5) && !strcmp(line + 5, id)) {
      if (output)
        output[used] = 0;
      return 0;
    }
    if (!strncmp(line, "%error ", 7) && !strcmp(line + 7, id))
      return 1;
    if (output) {
      if (length >= capacity - used)
        return 1;
      memcpy(output + used, line, length);
      used += length;
    }
  }
  return 1;
}

static int start_control(struct terminal *t, const char *session) {
  int fd[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd))
    return 1;
  t->control_pid = fork();
  if (t->control_pid == 0) {
    close(fd[0]);
    close(t->command_fd);
    close(t->ack_fd);
    if (chdir(t->work) || dup2(fd[1], STDIN_FILENO) < 0 ||
        dup2(fd[1], STDOUT_FILENO) < 0)
      _exit(127);
    close(fd[1]);
    execl(t->tmux, t->tmux, "-S", "socket", "-f", "/dev/null", "-C",
          "attach-session", "-E", "-f", "ignore-size,no-output", "-t", session,
          (const char *)NULL);
    _exit(127);
  }
  close(fd[1]);
  if (t->control_pid < 0) {
    close(fd[0]);
    return 1;
  }
  t->control_fd = fd[0];
  return control_reply(t, NULL, 0);
}

static int control_request(struct terminal *t, const char *request,
                           char *output, size_t capacity) {
  size_t left = strlen(request);
  while (left) {
    ssize_t sent = send(t->control_fd, request, left, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      return 1;
    request += sent;
    left -= (size_t)sent;
  }
  return control_reply(t, output, capacity);
}

static int query(struct terminal *t, const char *format, char *output,
                 size_t capacity) {
  char request[256];
  size_t length;
  /* All query formats and pane names are literals owned by the fixtures. */
  if (snprintf(request, sizeof(request), "display-message -p -t '%s' '%s'\n",
               t->pane, format) >= (int)sizeof(request) ||
      control_request(t, request, output, capacity))
    return 1;
  length = strlen(output);
  while (length && output[length - 1] == '\n')
    output[--length] = 0;
  return 0;
}
static int grid_rows(struct grid *g, char *line) {
  char *end;
  g->count = 0;
  while (*line && g->count < 512) {
    g->rows[g->count++] = line;
    end = strchr(line, '\n');
    if (!end)
      break;
    *end = 0;
    line = end + 1;
  }
  return *line && g->count == 512;
}
static int capture(struct terminal *t, struct grid *g, int history,
                   int joined) {
  char request[128];
  snprintf(request, sizeof(request), "capture-pane -p -t '%s' %s -S %s\n",
           t->pane, joined ? "-J" : "", history ? "-" : "0");
  return control_request(t, request, g->text, sizeof(g->text)) ||
         grid_rows(g, g->text);
}
static int nonblank(const char *text) {
  while (*text) {
    if (!isspace((unsigned char)*text))
      return 1;
    ++text;
  }
  return 0;
}
static int last_row(const struct grid *g) {
  int row;
  for (row = g->count - 1; row >= 0; --row)
    if (g->rows[row][0])
      return row;
  return -1;
}
static void compact(const char *text, char *output) {
  while (*text) {
    if (!isspace((unsigned char)*text))
      *output++ = *text;
    ++text;
  }
  *output = 0;
}
static void flatten(const struct grid *g, char *output, int trim_space) {
  int row;
  output[0] = 0;
  for (row = 0; row < g->count; ++row) {
    if (trim_space)
      compact(g->rows[row], output + strlen(output));
    else
      strcat(output, g->rows[row]);
  }
}
static int owned_row(const struct grid *g, int thinking) {
  int row;
  for (row = 0; row < g->count; ++row)
    if ((thinking && !strncmp(g->rows[row], "! Thinking", 10)) ||
        (thinking ? !strncmp(g->rows[row], "+ fixture", 9)
                  : g->rows[row][0] == '+') ||
        !strncmp(g->rows[row], "Q 1.", 4))
      return row;
  return g->count;
}
static void quoted(FILE *file, const char *text) {
  fputc('\'', file);
  while (*text) {
    if (*text == '\'')
      fputs("'\\''", file);
    else
      fputc(*text, file);
    ++text;
  }
  fputc('\'', file);
}
static int shell_path(char *output, size_t capacity, const char *prefix,
                      const char *path) {
  size_t used = strlen(prefix);
  if (used + 3 >= capacity)
    return 1;
  memcpy(output, prefix, used);
  output[used++] = '\'';
  while (*path) {
    if (used + 6 >= capacity)
      return 1;
    if (*path == '\'') {
      memcpy(output + used, "'\\''", 4);
      used += 4;
    } else
      output[used++] = *path;
    ++path;
  }
  output[used++] = '\'';
  output[used] = 0;
  return 0;
}
static int prepare(struct terminal *t, const char *tmux, const char *build,
                   const char *pane) {
  memset(t, 0, sizeof(*t));
  t->tmux = tmux;
  t->command_fd = t->ack_fd = -1;
  t->control_fd = -1;
  t->expected_prompt = -1;
  if (snprintf(t->work, sizeof(t->work), "%s/r-XXXXXX", build) >=
          (int)sizeof(t->work) ||
      !mkdtemp(t->work))
    return 1;
  if (strlen(t->work) + 17 >= sizeof(t->commands))
    return 1;
  strcpy(t->commands, t->work);
  strcat(t->commands, "/commands");
  strcpy(t->acks, t->work);
  strcat(t->acks, "/acks");
  strcpy(t->recording, t->work);
  strcat(t->recording, "/output.bin");
  snprintf(t->pane, sizeof(t->pane), "%s:0.0", pane);
  if (mkfifo(t->commands, 0600) || mkfifo(t->acks, 0600))
    return 1;
  t->command_fd = open(t->commands, O_RDWR | O_NONBLOCK);
  t->ack_fd = open(t->acks, O_RDWR | O_NONBLOCK);
  return t->command_fd < 0 || t->ack_fd < 0;
}
static void cleanup(struct terminal *t) {
  if (t->control_fd >= 0)
    close(t->control_fd);
  if (t->control_pid > 0) {
    kill(t->control_pid, SIGTERM);
    while (waitpid(t->control_pid, NULL, 0) < 0 && errno == EINTR) {
    }
  }
  command(t, NULL, 0, "kill-server", (const char *)NULL);
  if (t->command_fd >= 0)
    close(t->command_fd);
  if (t->ack_fd >= 0)
    close(t->ack_fd);
  unlink(t->commands);
  unlink(t->acks);
  unlink(t->recording);
  {
    char script[4096];
    strcpy(script, t->work);
    strcat(script, "/runner");
    unlink(script);
  }
  rmdir(t->work);
}
static int launch(struct terminal *t, const char *session, const char *fixture,
                  int document, int prefilled, int width, int height,
                  int producer, int chat_delay_ms) {
  char script[4096], shell_command[8192], w[16], h[16], doc[16];
  FILE *file;
  int row;
  strcpy(script, t->work);
  strcat(script, "/runner");
  file = fopen(script, "w");
  if (!file)
    return 1;
  fputs("#!/usr/bin/env bash\nset -euo pipefail\n", file);
  if (prefilled)
    for (row = 0; row < 100; ++row)
      fprintf(file, "printf 'existing terminal row %03d\\n'\n", row);
  fputs("exec ", file);
  if (chat_delay_ms)
    fprintf(file,
            "env SOFTLINE_CHAT_CHAR_MS=%d SOFTLINE_PROMPT_THEME=plain "
            "SOFTLINE_CHAT_APPROVAL_DEMO=0 ",
            chat_delay_ms);
  quoted(file, fixture);
  if (!chat_delay_ms) {
    fputc(' ', file);
    quoted(file, t->commands);
    fputc(' ', file);
    quoted(file, t->acks);
    snprintf(doc, sizeof(doc), "%d", document);
    fputc(' ', file);
    quoted(file, doc);
    if (producer)
      fputs(" --producer-only", file);
  }
  fputc('\n', file);
  if (fclose(file))
    return 1;
  /* Work paths come from mkdtemp below the trusted repository. Quote for the
   * shell rather than treating a serialized string as shell escaping. */
  if (shell_path(shell_command, sizeof(shell_command), "exec bash ", script))
    return 1;
  snprintf(w, sizeof(w), "%d", width);
  snprintf(h, sizeof(h), "%d", height);
  if (command(t, NULL, 0, "new-session", "-d", "-s", session, "-x", w, "-y", h,
              shell_command, (const char *)NULL) ||
      command(t, NULL, 0, "set-option", "-w", "remain-on-exit", "on",
              (const char *)NULL) ||
      command(t, NULL, 0, "set-option", "-w", "remain-on-exit-format", "",
              (const char *)NULL))
    return 1;
  if (!producer) {
    if (shell_path(shell_command, sizeof(shell_command), "cat > ",
                   t->recording))
      return 1;
    if (command(t, NULL, 0, "pipe-pane", "-t", t->pane, shell_command,
                (const char *)NULL))
      return 1;
  }
  return start_control(t, session);
}
static int resize_window(struct terminal *t, int width, int height) {
  char w[16], h[16], window[32];
  snprintf(w, sizeof(w), "%d", width);
  snprintf(h, sizeof(h), "%d", height);
  snprintf(window, sizeof(window), "%.*s:0",
           (int)(strchr(t->pane, ':') - t->pane), t->pane);
  return command(t, NULL, 0, "resize-window", "-t", window, "-x", w, "-y", h,
                 (const char *)NULL);
}
static int wait_tty(struct terminal *t, int width, int height) {
  char path[4096];
  struct winsize size;
  int fd, failed = 1;
  double deadline = now() + 4;
  if (query(t, "#{pane_tty}", path, sizeof(path)))
    return 1;
  fd = open(path, O_RDONLY | O_NOCTTY);
  if (fd < 0)
    return 1;
  do {
    if (!ioctl(fd, TIOCGWINSZ, &size) && size.ws_col == width &&
        size.ws_row == height) {
      failed = 0;
      break;
    }
    delay(.02);
  } while (now() < deadline);
  close(fd);
  return failed;
}
static int ack(struct terminal *t) {
  size_t used = 0;
  ssize_t count;
  struct pollfd fd;
  double deadline = now() + 4;
  fd.fd = t->ack_fd;
  fd.events = POLLIN;
  while (used < 6 && now() < deadline) {
    if (poll(&fd, 1, 20) < 0)
      return 1;
    if (fd.revents & POLLIN) {
      count = read(fd.fd, t->reply + used, 6 - used);
      if (count > 0)
        used += (size_t)count;
      else if (count < 0 && errno != EAGAIN)
        return 1;
    }
  }
  t->line_open = t->reply[5] != 0;
  if (used != 6)
    return 1;
  {
    char expected[64], actual[64];
    snprintf(expected, sizeof(expected), TEST_FRAME_TITLE,
             ++t->fixture_sequence);
    /* The FIFO ACK proves the write returned. The title proves that tmux
     * has parsed that write and all the preceding bytes in the pane. */
    do {
      if (query(t, "#{pane_title}", actual, sizeof(actual)))
        return 1;
      if (!strcmp(actual, expected))
        return 0;
      delay(.001);
    } while (now() < deadline);
    fprintf(stderr, "tmux did not parse fixture frame %lu\n",
            t->fixture_sequence);
  }
  return 1;
}
static int exchange(struct terminal *t, char byte, int width, int height,
                    const char *draft_tail) {
  struct grid g;
  char cursor[128];
  int row, pane_height, visible;
  double deadline;
  if (write(t->command_fd, &byte, 1) != 1 || ack(t))
    return -1;
  if (width && (t->reply[1] * 256 + t->reply[2] != width ||
                t->reply[3] * 256 + t->reply[4] != height))
    return 1;
  deadline = now() + 4;
  do {
    if (query(t, "#{cursor_y},#{pane_height},#{cursor_flag}", cursor,
              sizeof(cursor)) ||
        capture(t, &g, 0, 0))
      return -1;
    if (sscanf(cursor, "%d,%d,%d", &row, &pane_height, &visible) != 3)
      return -1;
    if (row == (t->expected_prompt < 0 ? last_row(&g) : t->expected_prompt) &&
        visible == 1 && last_row(&g) >= 0) {
      const char *tail = g.rows[last_row(&g)];
      size_t length = strlen(tail), needed = strlen(draft_tail);
      if (length >= needed && !strcmp(tail + length - needed, draft_tail))
        return 0;
    }
    delay(.02);
  } while (now() < deadline);
  fprintf(stderr, "Prompt displaced after %c: %s\n", byte, cursor);
  return -1;
}
static size_t recording_size(struct terminal *t) {
  struct stat s;
  return stat(t->recording, &s) ? 0 : (size_t)s.st_size;
}
static int recording(struct terminal *t, size_t offset, char *bytes,
                     size_t capacity) {
  FILE *file = fopen(t->recording, "rb");
  size_t count;
  if (!file) {
    bytes[0] = 0;
    return 0;
  }
  if (fseek(file, (long)offset, SEEK_SET)) {
    fclose(file);
    return 1;
  }
  count = fread(bytes, 1, capacity - 1, file);
  bytes[count] = 0;
  if (ferror(file) || count == capacity - 1) {
    fclose(file);
    return 1;
  }
  fclose(file);
  return 0;
}
static int recorded_frame(struct terminal *t, size_t offset, char *bytes,
                          size_t capacity) {
  char marker[64];
  double deadline = now() + 4;
  snprintf(marker, sizeof(marker), TEST_FRAME_MARKER, t->fixture_sequence);
  do {
    if (recording(t, offset, bytes, capacity))
      return 1;
    if (strstr(bytes, marker))
      return 0;
    delay(.001);
  } while (now() < deadline);
  fprintf(stderr, "tmux did not record fixture frame %lu\n",
          t->fixture_sequence);
  return 1;
}
#endif
