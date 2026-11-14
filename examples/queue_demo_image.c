#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include "queue_demo_image.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int demo_image_limit(size_t *limit) {
  const char *value = getenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES");
  char *end;
  unsigned long number;
  *limit = 32u * 1024u * 1024u;
  if (!value)
    return 0;
  if (*value < '0' || *value > '9')
    return -1;
  errno = 0;
  number = strtoul(value, &end, 10);
  if (errno || *end || number > INT_MAX)
    return -1;
  if (number)
    *limit = (size_t)number;
  return 0;
}
int demo_image_store_init(demo_image_store_t *s) {
  const char *directory = getenv("SOFTLINE_CHAT_IMAGE_DIR");
  struct stat info;
  char *resolved;
  size_t length, i;
  memset(s, 0, sizeof(*s));
  s->fd = -1;
  if (demo_image_limit(&s->limit)) {
    fprintf(stderr,
            "SOFTLINE_CHAT_IMAGE_MAX_BYTES must be 0..%d (0 selects 32 MiB)\n",
            INT_MAX);
    return -1;
  }
  resolved = realpath(directory ? directory : ".", NULL);
  if (!resolved) {
    perror("SOFTLINE_CHAT_IMAGE_DIR (default: server working directory)");
    return -1;
  }
  length = strlen(resolved);
  if (length + 64 >= sizeof(s->directory) || stat(resolved, &info) ||
      !S_ISDIR(info.st_mode)) {
    fprintf(stderr, "SOFTLINE_CHAT_IMAGE_DIR must be an existing directory "
                    "with a bounded absolute path\n");
    free(resolved);
    return -1;
  }
  for (i = 0; i < length; ++i)
    if ((unsigned char)resolved[i] < 32 || resolved[i] == 127) {
      fprintf(stderr,
              "SOFTLINE_CHAT_IMAGE_DIR must not contain control characters\n");
      free(resolved);
      return -1;
    }
  memcpy(s->directory, resolved, length + 1);
  free(resolved);
  return 0;
}
static void release(demo_image_store_t *s, int keep) {
  if (s->fd >= 0)
    (void)close(s->fd);
  s->fd = -1;
  if (*s->temporary)
    (void)unlink(s->temporary);
  if (s->published && !keep)
    (void)unlink(s->path);
  s->id = 0;
  s->received = 0;
  s->published = 0;
  s->temporary[0] = s->path[0] = 0;
}
void demo_image_store_close(demo_image_store_t *s) {
  /* An acknowledgement can be lost after the editor inserted its path.
   * Retain published files on disconnect rather than dangling that reference.
   */
  release(s, 1);
}
static int reply(demo_link_t *link, unsigned long kind, unsigned long id,
                 size_t offset, const char *path, const char *reason) {
  demo_packet_t p = {0};
  int result;
  demo_put_number(&p, kind);
  demo_put_number(&p, id);
  if (kind == DEMO_IMAGE_READY)
    demo_put_number(&p, (unsigned long)offset);
  else {
    demo_put_string(&p, path);
    demo_put_string(&p, reason);
  }
  result = demo_link_queue(link, &p);
  demo_packet_free(&p);
  return result;
}
static int failure(demo_image_store_t *s, demo_link_t *link,
                   const char *reason) {
  unsigned long id = s->id;
  release(s, 0);
  return reply(link, DEMO_IMAGE_RESULT, id, 0, NULL, reason);
}
int demo_image_store_input(demo_image_store_t *s, demo_link_t *link,
                           unsigned long kind, demo_packet_t *p, double now) {
  unsigned long id = demo_number(p);
  if (p->failed || !id)
    return -1;
  if (kind == DEMO_IMAGE_BEGIN) {
    const char *mime = demo_string(p);
    const char *extension;
    size_t length;
    if (p->failed || p->offset != p->length || s->id || !mime)
      return -1;
    if (!strcmp(mime, "image/png"))
      extension = ".png";
    else if (!strcmp(mime, "image/jpeg"))
      extension = ".jpeg";
    else
      return reply(link, DEMO_IMAGE_RESULT, id, 0, NULL,
                   "server accepts PNG and JPEG images only");
    s->id = id;
    s->started = s->progress = now;
    (void)snprintf(s->temporary, sizeof(s->temporary),
                   "%s/.softline-image-XXXXXX", s->directory);
    s->fd = mkstemp(s->temporary);
    if (s->fd < 0) {
      s->temporary[0] = 0;
      return failure(s, link, "server could not create image");
    }
    if (fcntl(s->fd, F_SETFD, FD_CLOEXEC))
      return failure(s, link, "server could not protect image descriptor");
    length = strlen(s->temporary);
    (void)snprintf(s->path, sizeof(s->path), "%s/softline-image-%s%s",
                   s->directory, s->temporary + length - 6, extension);
    return reply(link, DEMO_IMAGE_READY, id, 0, NULL, NULL);
  }
  if (kind == DEMO_IMAGE_ABORT || kind == DEMO_IMAGE_DONE) {
    if (p->offset != p->length)
      return -1;
    if (id == s->id) {
      if (kind == DEMO_IMAGE_DONE && !s->published)
        return -1;
      release(s, kind == DEMO_IMAGE_DONE);
    }
    return 0;
  }
  if (id != s->id) {
    /* Ignore already rejected/cancelled traffic still in the TCP stream. */
    return 0;
  }
  if (kind == DEMO_IMAGE_DATA) {
    unsigned long offset = demo_number(p);
    size_t length, at = 0;
    const unsigned char *bytes = demo_image_bytes(p, &length);
    if (p->failed || s->published || offset != s->received)
      return failure(s, link, "invalid image chunk or offset");
    if (length > s->limit - s->received)
      return failure(s, link, "image exceeds server byte limit");
    while (at < length) {
      ssize_t written = write(s->fd, bytes + at, length - at);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        return failure(s, link, "server image write failed");
      at += (size_t)written;
    }
    s->received += length;
    s->progress = now;
    return reply(link, DEMO_IMAGE_READY, id, s->received, NULL, NULL);
  }
  if (kind == DEMO_IMAGE_END) {
    unsigned long length = demo_number(p);
    int failed;
    if (p->failed || p->offset != p->length || s->published ||
        length != s->received || !length)
      return failure(s, link, "invalid image end or byte count");
    failed = fsync(s->fd);
    if (close(s->fd))
      failed = 1;
    s->fd = -1;
    /* link publishes atomically without overwriting an existing destination. */
    if (failed || linkat(AT_FDCWD, s->temporary, AT_FDCWD, s->path, 0))
      return failure(s, link, "server image publication failed");
    s->published = 1;
    if (unlink(s->temporary))
      return failure(s, link, "server image publication failed");
    s->temporary[0] = 0;
    s->progress = now;
    return reply(link, DEMO_IMAGE_RESULT, id, 0, s->path, NULL);
  }
  return -1;
}
double demo_image_store_due(const demo_image_store_t *s) {
  double due = s->progress + 30000;
  if (due > s->started + 120000)
    due = s->started + 120000;
  /* Once storage was acknowledged, the originating editor may be suspended.
   * Keep this one bounded record until DONE, ABORT or disconnect. */
  return s->id && !s->published ? due : 0;
}
int demo_image_store_tick(demo_image_store_t *s, demo_link_t *link,
                          double now) {
  double due = demo_image_store_due(s);
  if (!due || now < due)
    return 0;
  return failure(s, link, "server image upload timed out");
}
