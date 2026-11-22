#ifndef SOFTLINE_RESOLVER_H
#define SOFTLINE_RESOLVER_H

/* Private Linux X11 resolver. Bundled c-ares supplies hosts, DNS and literal
 * addresses without libc NSS. Callers own results until sl_resolve_free(). */
#include "softline_cares_namespace.h"
#include <ares.h>
#include <poll.h>
#include <stdint.h>

/* Optional private injection for deterministic hosts/DNS integration tests.
 * Production passes NULL and uses /etc/hosts and /etc/resolv.conf. */
typedef struct {
  const char *hosts_path;
  const char *resolvconf_path;
  const char *servers;
} sl_resolve_config_t;

typedef struct sl_lookup sl_lookup_t;
sl_lookup_t *sl_resolve_start(const char *host, const char *port,
                              const sl_resolve_config_t *config);
int sl_resolve_prepare(sl_lookup_t *, struct pollfd *, size_t, int *);
int sl_resolve_step(sl_lookup_t *);
struct ares_addrinfo *sl_resolve_take(sl_lookup_t *);
void sl_resolve_destroy(sl_lookup_t *);

int sl_resolve(const char *host, const char *port, int64_t deadline_ms,
               const sl_resolve_config_t *config,
               struct ares_addrinfo **addresses);
void sl_resolve_free(struct ares_addrinfo *addresses);

#endif
