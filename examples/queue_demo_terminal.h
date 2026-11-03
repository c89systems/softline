#ifndef SOFTLINE_DEMO_TERMINAL_H
#define SOFTLINE_DEMO_TERMINAL_H
#include "../src/softline_pty.h"
#include "queue_demo_wire.h"
#include <netinet/in.h>
/* Demo protocol only: transport/security remain outside libsoftline. */
typedef struct demo_terminal_server {
  struct demo_terminal_server *next;
  sl_pty_t pty;
  char input[4096];
  size_t input_len;
  int eof, ready;
} demo_terminal_server_t;
int demo_terminal_remote(sl_t *editor, const struct sockaddr_in *address,
                         int *exit_status);
int demo_terminal_server_input(demo_terminal_server_t *server,
                               demo_link_t *link, unsigned long kind,
                               demo_packet_t *packet);
int demo_terminal_server_pump(demo_terminal_server_t *server,
                              demo_link_t *link);
#endif
