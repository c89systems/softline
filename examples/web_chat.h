#ifndef SOFTLINE_WEB_CHAT_H
#define SOFTLINE_WEB_CHAT_H
#include "www_mock.h"
typedef struct demo_web_chat demo_web_chat_t;
typedef struct demo_web_ops {
  /* A nonzero backend request defers the browser acknowledgement until the
   * editor mirror reports its actual outcome. Local admission leaves zero. */
  int (*submit)(void *, const char *, int steer, sl_queue_request_id_t *);
  int (*control)(void *, const char *action, const char *id,
                 const char *revision);
  int (*image)(void *, sl_t *source, const sl_image_paste_event_t *);
  void (*snapshot)(void *, demo_web_chat_t *);
} demo_web_ops_t;
/* NULL with enabled=0 is normal. errno is set on configuration/listen failure.
 * SOFTLINE_WEB defaults off; SOFTLINE_WEB_PORT defaults to the caller's port.
 */
demo_web_chat_t *demo_web_open(sl_t *, const demo_web_ops_t *, void *,
                               unsigned short default_port, int *enabled);
void demo_web_close(demo_web_chat_t *);
int demo_web_tick(demo_web_chat_t *);
void demo_web_result(demo_web_chat_t *, const sl_queue_result_t *);
int demo_web_timeout(demo_web_chat_t *, int timeout_ms);
unsigned short demo_web_port(demo_web_chat_t *);
demo_www_t *demo_web_listener(demo_web_chat_t *);
void demo_web_queue(demo_web_chat_t *);
void demo_web_queue_begin(demo_web_chat_t *);
void demo_web_queue_entry(demo_web_chat_t *, const char *id,
                          const char *revision, const char *text, int steer,
                          int held);
void demo_web_queue_end(demo_web_chat_t *);
void demo_web_status(demo_web_chat_t *, const char *, int busy);
/* Feed the same raw source bytes as the ANSI renderer, never ANSI output. */
void demo_web_message(demo_web_chat_t *, const char *role, const char *text);
void demo_web_response(demo_web_chat_t *, const void *, size_t);
void demo_web_response_end(demo_web_chat_t *);
#endif
