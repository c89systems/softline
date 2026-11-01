#ifndef SOFTLINE_TEST_REMOTE_CHAT_OWNER_H
#define SOFTLINE_TEST_REMOTE_CHAT_OWNER_H
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
static const char *remote_owner;
static pid_t remote_owner_pid;
static char remote_owner_port[16];
static int start_remote_owner_at(const char *requested_port) {
  int ready[2];
  FILE *stream;
  char line[128];
  unsigned int port;
  if (!remote_owner || remote_owner_pid > 0)
    return 0;
  if (pipe(ready))
    return 1;
  remote_owner_pid = fork();
  if (!remote_owner_pid) {
    setenv("SOFTLINE_CHAT_TEST_SIGNALS", "1", 1);
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    execl(remote_owner, remote_owner, requested_port, (char *)NULL);
    _exit(127);
  }
  close(ready[1]);
  stream = fdopen(ready[0], "r");
  if (!stream || !fgets(line, sizeof(line), stream) ||
      sscanf(line, "LISTEN 127.0.0.1 %u", &port) != 1) {
    if (stream)
      fclose(stream);
    else
      close(ready[0]);
    return 1;
  }
  fclose(stream);
  snprintf(remote_owner_port, sizeof(remote_owner_port), "%u", port);
  return 0;
}
static int start_remote_owner(void) { return start_remote_owner_at("0"); }
static void stop_remote_owner(void) {
  int status;
  if (remote_owner_pid > 0) {
    kill(remote_owner_pid, SIGCONT);
    kill(remote_owner_pid, SIGTERM);
    waitpid(remote_owner_pid, &status, 0);
    remote_owner_pid = 0;
  }
}
#endif
