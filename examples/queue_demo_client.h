#ifndef SOFTLINE_QUEUE_DEMO_CLIENT_H
#define SOFTLINE_QUEUE_DEMO_CLIENT_H
#include "softline/softline.h"
typedef struct demo_client demo_client_t;
typedef int (*demo_event_fn)(void *context, int kind, const char *text);
void demo_client_events(demo_client_t *client, demo_event_fn handler,
                        void *context);
void demo_client_options(demo_client_t *client, unsigned int delay_us,
                         int approvals, unsigned int interval_ms,
                         unsigned int seed);
int demo_client_idle(demo_client_t *client);
void demo_client_pause(demo_client_t *client);
int demo_client_shell_begin(demo_client_t *client);
int demo_client_service(demo_client_t *client, int timeout_ms);
demo_client_t *demo_client_create(const char *host, unsigned short port);
const sl_queue_backend_t *demo_client_backend(demo_client_t *client);
int demo_client_attach(demo_client_t *client, sl_t *editor);
/* Explicit reconnect resolves original request identities; it never reoffers.
 */
int demo_client_reconnect(demo_client_t *client);
int demo_client_cancel(demo_client_t *client);
void demo_client_destroy(demo_client_t *client);
#endif
