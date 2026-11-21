#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c, m)                                                            \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s\n", m);                                              \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

static int run_case(const char *fixture, int feed, int resized) {
  int master = -1, slave = -1, markers[2] = {-1, -1};
  pid_t child = -1;
  struct winsize size;
  struct pollfd descriptors[2];
  char fd_text[32], resize_text[8], data[65536];
  char *output = NULL;
  size_t used = 0, offset = 0, length, glyphs = 0;
  int phase = 0, probes = 0, status = 0, result = 1, done = 0;
  double deadline, started = 0;
  ssize_t count;
  char *request, *begin, *end, *next;
  memset(&size, 0, sizeof(size));
  size.ws_row = (unsigned short)(feed ? 24 : 5);
  size.ws_col = (unsigned short)(feed ? 80 : 20);
  CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0, "openpty failed");
  CHECK(pipe(markers) == 0, "marker pipe failed");
  output = (char *)calloc(1, 1024 * 1024);
  CHECK(output != NULL, "capture allocation failed");
  snprintf(fd_text, sizeof(fd_text), "%d", markers[1]);
  snprintf(resize_text, sizeof(resize_text), "%d", resized);
  child = fork();
  CHECK(child >= 0, "fork failed");
  if (child == 0) {
    close(master);
    close(markers[0]);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
      close(slave);
    if (feed)
      execl(fixture, fixture, fd_text, resize_text, (char *)NULL);
    else
      execl(fixture, fixture, (char *)NULL);
    _exit(127);
  }
  close(slave);
  slave = -1;
  close(markers[1]);
  markers[1] = -1;
  descriptors[0].fd = markers[0];
  descriptors[0].events = POLLIN;
  descriptors[1].fd = master;
  descriptors[1].events = POLLIN;
  deadline = now() + 8.0;
  while (!done && now() < deadline) {
    int ready = poll(descriptors, 2, 10);
    CHECK(ready >= 0 || errno == EINTR, "poll failed");
    if (descriptors[0].revents & POLLIN) {
      count = read(markers[0], data, sizeof(data));
      if (count > 0) {
        ssize_t index;
        for (index = 0; index < count; ++index) {
          if (data[index] == 'S') {
            phase = 1;
            started = now();
            CHECK(write(master, "typed\r", 6) == 6, "typing failed");
          }
          if (data[index] == 'D') {
            phase = 2;
            CHECK(now() - started < 0.5, "feed contains a timed wait");
          }
        }
      }
    }
    if (descriptors[1].revents & (POLLIN | POLLHUP)) {
      count = read(master, data, sizeof(data));
      if (count == 0 || (count < 0 && errno == EIO)) {
        done = 1;
        break;
      }
      CHECK(count >= 0 || errno == EINTR, "PTY read failed");
      if (count > 0) {
        CHECK(used + (size_t)count < 1024 * 1024,
              "fixture capture exceeded bound");
        memcpy(output + used, data, (size_t)count);
        used += (size_t)count;
        while ((request = strstr(output + offset, "\033[6n")) != NULL) {
          const char *reply;
          offset = (size_t)(request - output) + 4;
          ++probes;
          CHECK(!feed || phase != 1, "feed requested a cursor round trip");
          reply =
              feed ? "\033[24;1R" : (probes == 1 ? "\033[1;3R" : "\033[5;3R");
          length = strlen(reply);
          CHECK(write(master, reply, length) == (ssize_t)length,
                "cursor reply failed");
        }
      }
    }
  }
  CHECK(done, "terminal fixture did not finish before deadline");
  CHECK(waitpid(child, &status, 0) == child, "child wait failed");
  child = -1;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "terminal fixture failed");
  if (feed) {
    CHECK(phase == 2 && started > 0 && probes > 0,
          "feed/probing coverage missing");
    next = output;
    while ((next = strstr(next, "\342\224\200")) != NULL) {
      ++glyphs;
      next += 3;
    }
    CHECK(glyphs == 128, "producer bytes were lost");
  } else {
    CHECK(probes == 2,
          "late scroll ownership must use exactly two ordinary probes");
    CHECK(strstr(output, "one\r\ntwo\r\nthree\r\nfour\r\n") != NULL,
          "finite output changed");
    CHECK(strstr(output, "\033[1;4r") != NULL,
          "late prompt did not acquire the scroll region");
  }
  begin = output;
  while ((begin = strstr(begin, "\033]777;feed-start\007")) != NULL) {
    begin += strlen("\033]777;feed-start\007");
    end = strstr(begin, "\033]777;feed-end\007");
    CHECK(end != NULL, "unterminated feed marker");
    request = strstr(begin, "\033[6n");
    CHECK(request == NULL || request >= end,
          "feed requested cursor position inside its output frame");
    begin = end + strlen("\033]777;feed-end\007");
  }
  result = 0;
cleanup:
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  if (master >= 0)
    close(master);
  if (slave >= 0)
    close(slave);
  if (markers[0] >= 0)
    close(markers[0]);
  if (markers[1] >= 0)
    close(markers[1]);
  free(output);
  return result;
}

static int chat_commands(const char *chat, const char *guard, const char *theme,
                         int prompt_demo) {
  int master = -1, slave = -1, status, result = 1, sent = 0, quit = 0;
  int toggle = !strcmp(theme, "riced"), ready = 0, stopping = 0;
  pid_t child = -1;
  struct winsize size;
  struct termios attributes;
  struct pollfd descriptor;
  char bytes[262144] = {0}, plain[262144];
  size_t used = 0, probe = 0, i, p;
  ssize_t count;
  double deadline;
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0, "openpty failed");
  CHECK(tcgetattr(slave, &attributes) == 0, "termios failed");
  attributes.c_lflag &= (tcflag_t)~ECHO;
  CHECK(tcsetattr(slave, TCSANOW, &attributes) == 0, "termios failed");
  child = fork();
  CHECK(child >= 0, "fork failed");
  if (child == 0) {
    close(master);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    close(slave);
    setenv("SOFTLINE_CHAT_CHAR_MS", toggle ? "20" : "0", 1);
    setenv("SOFTLINE_PROMPT_THEME", theme, 1);
    setenv("LD_PRELOAD", guard, 1);
    setenv("SOFTLINE_CHAT_APPROVAL_DEMO", "0", 1);
    execl(chat, chat, (char *)NULL);
    _exit(127);
  }
  close(slave);
  slave = -1;
  descriptor.fd = master;
  descriptor.events = POLLIN;
  deadline = now() + (prompt_demo ? 12 : 4);
  while (now() < deadline) {
    CHECK(poll(&descriptor, 1, 20) >= 0, "poll failed");
    if (!(descriptor.revents & (POLLIN | POLLHUP)))
      continue;
    CHECK(used + 1 < sizeof(bytes), "chat capture exhausted");
    count = read(master, bytes + used, sizeof(bytes) - used - 1);
    if (count == 0 || (count < 0 && errno == EIO))
      break;
    CHECK(count > 0, "PTY read failed");
    used += (size_t)count;
    bytes[used] = 0;
    while (strstr(bytes + probe, "\033[6n")) {
      probe = (size_t)(strstr(bytes + probe, "\033[6n") - bytes) + 4;
      CHECK(write(master, "\033[24;3R", 7) == 7, "cursor reply failed");
    }
    for (i = 0, p = 0; i < used; ++i) {
      if (bytes[i] == '\033' && i + 1 < used && bytes[i + 1] == '[') {
        i += 2;
        while (i < used && !((unsigned char)bytes[i] >= 0x40 &&
                             (unsigned char)bytes[i] <= 0x7e))
          ++i;
      } else
        plain[p++] = bytes[i];
    }
    plain[p] = 0;
    if (toggle && !ready && strstr(plain, "> ")) {
      static const char command[] = "/stream direct\r";
      CHECK(write(master, command, sizeof(command) - 1) == sizeof(command) - 1,
            "direct mode command failed");
      ready = 1;
    }
    if (prompt_demo && !sent && strstr(plain, "> ")) {
      CHECK(write(master, "/demo\r", 6) == 6, "enable demo failed");
      sent = 1;
    } else if (!prompt_demo && !sent &&
               (toggle ? strstr(plain, "Streaming mode: direct.")
                       : strstr(plain, "> "))) {
      CHECK(write(master, "hello\r", 6) == 6, "chat input failed");
      sent = 1;
    }
    if (prompt_demo && !stopping && strstr(plain, "Try another prompt.")) {
      CHECK(write(master, "/demo\r", 6) == 6, "disable demo failed");
      stopping = 1;
    }
    if (!quit && strstr(plain, prompt_demo ? "Prompt demo off."
                                           : "Try another prompt.")) {
      CHECK(write(master, "/quit\r", 6) == 6, "chat quit failed");
      quit = 1;
    }
  }
  CHECK(now() < deadline, "zero-delay producer stalled");
  CHECK(waitpid(child, &status, 0) == child, "child wait failed");
  child = -1;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "chat producer called sleep or failed");
  CHECK(sent && quit && strstr(bytes, "Goodbye."), "chat coverage missing");
  if (prompt_demo)
    CHECK(strstr(plain, "Summarize the next steps.") &&
              strstr(plain, "Prompt demo on.") && stopping,
          "the demo failed to send a simulated turn");
  result = 0;
cleanup:
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  if (master >= 0)
    close(master);
  if (slave >= 0)
    close(slave);
  if (result)
    fprintf(stderr, "theme=%s\n%s\n", theme, bytes);
  return result;
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "no_sleep") == 0)
    return chat_commands(argv[2], argv[3], "default", 0) ||
           chat_commands(argv[2], argv[3], "riced", 0);
  if (argc == 4 && strcmp(argv[1], "prompt_demo") == 0)
    return chat_commands(argv[2], argv[3], "default", 1);
  if (argc != 3)
    return 2;
  if (strcmp(argv[1], "feed") == 0)
    return run_case(argv[2], 1, 0) || run_case(argv[2], 1, 1);
  if (strcmp(argv[1], "late") == 0)
    return run_case(argv[2], 0, 0);
  return 2;
}
