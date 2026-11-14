#include "softline_resolver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct sl_lookup {
  ares_channel_t *channel;
  unsigned int scope;
  struct pollfd *fds;
  size_t count;
  int failed;
  int done;
  struct ares_addrinfo *addresses;
};

static int64_t sl_resolve_now(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void sl_resolve_socket(void *data, ares_socket_t fd, int readable,
                              int writable) {
  sl_lookup_t *lookup = (sl_lookup_t *)data;
  struct pollfd *grown;
  size_t i;
  for (i = 0; i < lookup->count && lookup->fds[i].fd != fd; ++i)
    ;
  if (!readable && !writable) {
    if (i < lookup->count)
      lookup->fds[i] = lookup->fds[--lookup->count];
    return;
  }
  if (i == lookup->count) {
    if (lookup->count == 16) {
      lookup->failed = 1;
      return;
    }
    grown = (struct pollfd *)realloc(lookup->fds,
                                     (lookup->count + 1) * sizeof(*grown));
    if (!grown) {
      lookup->failed = 1;
      return;
    }
    lookup->fds = grown;
    lookup->count++;
  }
  lookup->fds[i].fd = fd;
  lookup->fds[i].events =
      (short)((readable ? POLLIN : 0) | (writable ? POLLOUT : 0));
  lookup->fds[i].revents = 0;
}

static void sl_resolve_done(void *data, int status, int timeouts,
                            struct ares_addrinfo *addresses) {
  sl_lookup_t *lookup = (sl_lookup_t *)data;
  (void)timeouts;
  lookup->done = 1;
  if (status == ARES_SUCCESS)
    lookup->addresses = addresses;
  else
    ares_freeaddrinfo(addresses);
}

sl_lookup_t *sl_resolve_start(const char *host, const char *port,
                              const sl_resolve_config_t *config) {
  struct ares_options options;
  struct ares_addrinfo_hints hints;
  sl_lookup_t *lookup;
  struct in6_addr literal_address;
  char literal[INET6_ADDRSTRLEN], *end;
  const char *zone;
  unsigned long value;
  unsigned int scope = 0;
  int mask;
  /* c-ares parses bare IPs. Preserve DISPLAY's IPv6 interface qualifiers
   * using the kernel interface index, without libc hostname/NSS APIs. */
  zone = strchr(host, '%');
  if (zone) {
    size_t length = (size_t)(zone - host);
    if (!length || length >= sizeof(literal) || !zone[1])
      return NULL;
    memcpy(literal, host, length);
    literal[length] = '\0';
    if (inet_pton(AF_INET6, literal, &literal_address) != 1)
      return NULL;
    errno = 0;
    value = strtoul(zone + 1, &end, 10);
    if (zone[1] >= '0' && zone[1] <= '9' && *end == '\0') {
      if (errno || value > UINT_MAX)
        return NULL;
      scope = (unsigned int)value;
    } else {
      scope = if_nametoindex(zone + 1);
      if (!scope)
        return NULL;
    }
    host = literal;
  }
  lookup = calloc(1, sizeof(*lookup));
  if (!lookup)
    return NULL;
  lookup->scope = scope;
  memset(&options, 0, sizeof(options));
  /* Always use hosts before DNS; arbitrary NSS modules are outside this
   * private resolver's contract. X11 services are numeric, never NSS names. */
  options.lookups = (char *)"fb";
  options.sock_state_cb = sl_resolve_socket;
  options.sock_state_cb_data = lookup;
  mask = ARES_OPT_LOOKUPS | ARES_OPT_SOCK_STATE_CB;
  if (config) {
    if (config->hosts_path) {
      options.hosts_path = (char *)config->hosts_path;
      mask |= ARES_OPT_HOSTS_FILE;
    }
    if (config->resolvconf_path) {
      options.resolvconf_path = (char *)config->resolvconf_path;
      mask |= ARES_OPT_RESOLVCONF;
    }
  }
  if (ares_init_options(&lookup->channel, &options, mask) != ARES_SUCCESS)
    goto fail;
  if (config && config->servers &&
      ares_set_servers_ports_csv(lookup->channel, config->servers) !=
          ARES_SUCCESS)
    goto fail;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = ARES_AI_NUMERICSERV;
  ares_getaddrinfo(lookup->channel, host, port, &hints, sl_resolve_done,
                   lookup);
  return lookup;
fail:
  sl_resolve_destroy(lookup);
  return NULL;
}

int sl_resolve_prepare(sl_lookup_t *lookup, struct pollfd *fds, size_t capacity,
                       int *timeout_ms) {
  struct timeval maximum, timeout, *wait;
  if (!lookup || lookup->failed || lookup->count > capacity)
    return -1;
  if (lookup->done) {
    *timeout_ms = 0;
    return 0;
  }
  maximum.tv_sec = 10;
  maximum.tv_usec = 0;
  wait = ares_timeout(lookup->channel, &maximum, &timeout);
  *timeout_ms = (int)(wait->tv_sec * 1000 + (wait->tv_usec + 999) / 1000);
  if (lookup->count)
    memcpy(fds, lookup->fds, lookup->count * sizeof(*fds));
  return (int)lookup->count;
}

int sl_resolve_step(sl_lookup_t *lookup) {
  ares_fd_events_t events[16];
  size_t count, i;
  if (!lookup || lookup->failed)
    return -1;
  if (lookup->done)
    return lookup->addresses ? 1 : -1;
  count = lookup->count;
  if (poll(lookup->fds, (nfds_t)count, 0) < 0 && errno != EINTR)
    return -1;
  for (i = 0; i < count; ++i) {
    events[i].fd = lookup->fds[i].fd;
    events[i].events = ARES_FD_EVENT_NONE;
    if (lookup->fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
      events[i].events |= ARES_FD_EVENT_READ;
    if (lookup->fds[i].revents & POLLOUT)
      events[i].events |= ARES_FD_EVENT_WRITE;
  }
  if (ares_process_fds(lookup->channel, events, count,
                       ARES_PROCESS_FLAG_NONE) != ARES_SUCCESS ||
      lookup->failed)
    return -1;
  return lookup->done ? (lookup->addresses ? 1 : -1) : 0;
}

struct ares_addrinfo *sl_resolve_take(sl_lookup_t *lookup) {
  struct ares_addrinfo *addresses = lookup->addresses;
  struct ares_addrinfo_node *item;
  lookup->addresses = NULL;
  for (item = addresses ? addresses->nodes : NULL; item; item = item->ai_next)
    if (lookup->scope && item->ai_family == AF_INET6)
      ((struct sockaddr_in6 *)item->ai_addr)->sin6_scope_id = lookup->scope;
  return addresses;
}

void sl_resolve_destroy(sl_lookup_t *lookup) {
  if (!lookup)
    return;
  if (lookup->channel)
    ares_destroy(lookup->channel);
  ares_freeaddrinfo(lookup->addresses);
  free(lookup->fds);
  free(lookup);
}

int sl_resolve(const char *host, const char *port, int64_t deadline_ms,
               const sl_resolve_config_t *config,
               struct ares_addrinfo **addresses) {
  struct pollfd fds[16];
  sl_lookup_t *lookup;
  int count, timeout, result = -1;
  int64_t remaining, now;
  *addresses = NULL;
  now = sl_resolve_now();
  if (now < 0 || now >= deadline_ms)
    return -1;
  lookup = sl_resolve_start(host, port, config);
  while (lookup) {
    now = sl_resolve_now();
    if (now < 0 || now >= deadline_ms)
      break;
    result = sl_resolve_step(lookup);
    if (result != 0)
      break;
    remaining = deadline_ms - sl_resolve_now();
    if (remaining <= 0 ||
        (count = sl_resolve_prepare(lookup, fds, 16, &timeout)) < 0)
      break;
    if (remaining < timeout)
      timeout = (int)remaining;
    if (poll(fds, (nfds_t)count, timeout) < 0 && errno != EINTR)
      break;
  }
  if (result == 1)
    *addresses = sl_resolve_take(lookup);
  sl_resolve_destroy(lookup);
  return *addresses ? 0 : -1;
}

void sl_resolve_free(struct ares_addrinfo *addresses) {
  ares_freeaddrinfo(addresses);
}
