#ifndef SOFTLINE_WWW_MOCK_H
#define SOFTLINE_WWW_MOCK_H
#include "softline/www.h"
/* Example/test listener only. Never installed or linked into libsoftline. */
typedef struct demo_www demo_www_t;
typedef struct demo_www_peer demo_www_peer_t;
#define DEMO_WWW_FDS 17
#include <poll.h>
typedef void (*demo_www_closed_t)(void *, demo_www_peer_t *);
demo_www_t *demo_www_open(const sl_www_config_t *, unsigned short,
                          demo_www_closed_t);
void demo_www_close(demo_www_t *);
unsigned short demo_www_port(const demo_www_t *);
const char *demo_www_epoch(const demo_www_t *);
void demo_www_enable_terminal(demo_www_t *);
int demo_www_terminal_enabled(const demo_www_t *);
/* Conversation-wide policy, default off; both setters may change during chat.
 * External handoffs are a host-owned snapshot (e.g. native TUI sessions),
 * added to browser sessions tracked by this listener. Neither pauses work. */
void demo_www_set_terminal_prompt_lock(demo_www_t *, int enabled);
void demo_www_set_terminal_handoffs(demo_www_t *, size_t active);
void demo_www_set_terminal_state(demo_www_t *, size_t active, int enabled);
void demo_www_terminal_status(const demo_www_t *, size_t *active, int *enabled);
int demo_www_terminal_lock_environment(int *enabled);
int demo_www_timeout(const demo_www_t *, int);
int demo_www_poll(demo_www_t *, int timeout_ms);
/* Append to a host's poll set; dispatch with poll(..., 0) after readiness. */
int demo_www_descriptors(demo_www_t *, struct pollfd *, size_t);
int demo_www_attach(demo_www_t *, sl_t *);
void demo_www_peer_data(demo_www_peer_t *, void *);
void *demo_www_peer_get(demo_www_peer_t *);
int demo_www_reply(demo_www_peer_t *, const char *, const sl_www_field_t *,
                   size_t);
/* Host-owned bounded replay log (16 MiB/8192 events), never transcript storage
 * in libsoftline. Live production does not wait for a slow browser. */
int demo_www_publish(demo_www_t *, const char *, const sl_www_field_t *,
                     size_t);
void demo_www_text(demo_www_t *, const char *id, const char *role, const void *,
                   size_t);
void demo_www_begin(demo_www_t *, const char *id, const char *role);
void demo_www_end(demo_www_t *, const char *id);
/* Base64 is used only for bounded Markdown byte chunks so split UTF-8 remains
 * valid JSON. Browser image frames carry raw binary bytes in the other
 * direction. */
size_t demo_www_base64(const unsigned char *, size_t, char *);
#endif
