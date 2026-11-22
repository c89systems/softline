#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "web_terminal.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
typedef struct capture {
  demo_web_terminal_t *terminal;
  char output[256];
  size_t length;
  int ready, credits;
} capture_t;
static double milliseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}
static int capture(void *context, int binary, const void *bytes,
                   size_t length) {
  capture_t *c = context;
  if (binary) {
    if (length >= sizeof(c->output) - c->length)
      return SL_ERROR_FULL;
    memcpy(c->output + c->length, bytes, length);
    c->length += length;
    c->output[c->length] = 0;
  } else {
    char control[128];
    if (length >= sizeof(control))
      return SL_ERROR_FULL;
    memcpy(control, bytes, length);
    control[length] = 0;
    if (strstr(control, "\"type\":\"ready\""))
      c->ready = 1;
    if (strstr(control, "\"type\":\"input_credit\""))
      ++c->credits;
  }
  return SL_OK;
}
static int control(capture_t *c, const char *text) {
  return demo_web_terminal_message(c->terminal, 0, text, strlen(text));
}
static int destroy(capture_t *c) {
  double until = milliseconds() + 2000;
  while (c->terminal) {
    int status = demo_web_terminal_destroy(c->terminal);
    if (status != SL_ERROR_NOT_READY) {
      c->terminal = NULL;
      return status;
    }
    if (milliseconds() > until)
      return -1;
    (void)poll(NULL, 0, 1);
  }
  return 0;
}
static int fixture(void) {
  struct termios attributes;
  if (tcgetattr(STDIN_FILENO, &attributes))
    return 1;
  attributes.c_lflag &= (tcflag_t) ~(ECHO | ICANON);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes))
    return 1;
  signal(SIGHUP, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  printf("PID:%ld\nREADY\n", (long)getpid());
  fflush(stdout);
  for (;;)
    pause();
  return 0;
}
int main(int argc, char **argv) {
  capture_t c;
  char input[4096];
  long child = 0;
  int failed = 1, blocked = 0, chunk, status;
  double until, before;
  if (argc == 2 && !strcmp(argv[1], "-i"))
    return fixture();
  memset(&c, 0, sizeof(c));
  memset(input, 'x', sizeof(input));
#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      fprintf(stderr, "web terminal line %d: %s\n", __LINE__, #value);         \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
  CHECK(!setenv("SHELL", argv[0], 1));
  c.terminal = demo_web_terminal_create(capture, &c);
  CHECK(c.terminal);
  CHECK(demo_web_terminal_message(c.terminal, 1, input, 1) == SL_ERROR_INVALID);
  CHECK(control(&c, "action=open&rows=24&columns=80") == SL_OK);
  CHECK(control(&c, "action=open&rows=24&columns=80") == SL_ERROR_INVALID);
  until = milliseconds() + 2000;
  while ((!c.ready || !strstr(c.output, "READY")) && milliseconds() < until) {
    CHECK(demo_web_terminal_pump(c.terminal) == 0);
    (void)poll(NULL, 0, 1);
  }
  CHECK(c.ready && c.credits == 1 && sscanf(c.output, "PID:%ld", &child) == 1 &&
        child > 0);
  CHECK(control(&c, "action=resize&rows=0&columns=80") == SL_ERROR_INVALID);
  CHECK(control(&c, "action=ack&bytes=999999") == SL_ERROR_INVALID);
  for (chunk = 0; chunk < 64; ++chunk) {
    int credits = c.credits;
    CHECK(demo_web_terminal_message(c.terminal, 1, input, sizeof(input)) ==
          SL_OK);
    until = milliseconds() + 50;
    while (c.credits == credits && milliseconds() < until) {
      CHECK(demo_web_terminal_pump(c.terminal) == 0);
      (void)poll(NULL, 0, 1);
    }
    if (c.credits == credits) {
      blocked = 1;
      break;
    }
  }
  CHECK(blocked);
  CHECK(demo_web_terminal_message(c.terminal, 1, input, 1) == SL_ERROR_INVALID);
  before = milliseconds();
  demo_web_terminal_close(c.terminal);
  CHECK(milliseconds() - before < 100);
  CHECK(!destroy(&c));
  errno = 0;
  CHECK(waitpid((pid_t)child, &status, WNOHANG) == -1 && errno == ECHILD);
  CHECK(!setenv("SHELL", "/softline-missing-terminal-shell", 1));
  c.terminal = demo_web_terminal_create(capture, &c);
  CHECK(c.terminal);
  CHECK(control(&c, "action=open&rows=24&columns=80") == SL_OK);
  until = milliseconds() + 2000;
  do {
    status = demo_web_terminal_pump(c.terminal);
    if (status)
      break;
    (void)poll(NULL, 0, 1);
  } while (milliseconds() < until);
  CHECK(status < 0);
  CHECK(!destroy(&c));
  failed = 0;
cleanup:
  if (destroy(&c))
    failed = 1;
  return failed;
}
