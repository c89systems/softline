#include "softline_clipboard.h"
#include "softline_internal.h"

#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>

#define SL_IMAGE_PATH_MAX 4096u
struct sl_image {
  sl_image_paste_source_t source;
  sl_image_paste_id_t serial, id;
  size_t anchor, offset;
  int phase; /* 0 idle, 1 source, 2 paused, 3 backend, 4 result. */
  int64_t started, progress;
  sl_image_paste_event_t chunk;
  char *path;
  char filename[256];
  const char *media_type;
  int begun;
};

static int64_t image_now(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static sl_impl_t *image_impl(sl_t *self) {
  sl_impl_t *impl = self ? self->impl : NULL;
  return impl && !impl->image_callback_active ? impl : NULL;
}

static void image_error(sl_impl_t *impl, const char *reason) {
  size_t i = 0;
  if (!reason)
    reason = "image upload failed";
  while (i + 1 < sizeof(impl->error) && reason[i]) {
    unsigned char ch = (unsigned char)reason[i];
    impl->error[i] = ch >= 32 && ch < 127 ? (char)ch : '?';
    ++i;
  }
  impl->error[i] = '\0';
}

static int image_callback(sl_impl_t *impl, sl_image_paste_event_t *event) {
  int status;
  event->id = impl->image->id;
  impl->image_callback_active = 1;
  status = impl->image_callback(impl->image_context, event);
  impl->image_callback_active = 0;
  return status;
}

static void image_source_close(sl_impl_t *impl) {
  sl_image_paste_source_t source = impl->image->source;
  memset(&impl->image->source, 0, sizeof(impl->image->source));
  if (source.close) {
    impl->image_callback_active = 1;
    source.close(source.context);
    impl->image_callback_active = 0;
  }
}
static void image_clear(sl_impl_t *impl) {
  sl_image_t *image = impl->image;
  image_source_close(impl);
  free(image->path);
  image->path = NULL;
  image->phase = 0;
  image->id = 0;
  image->begun = 0;
  image->media_type = NULL;
  image->filename[0] = 0;
  memset(&image->chunk, 0, sizeof(image->chunk));
}

void sl_image_invalidate(sl_t *self, const char *reason) {
  sl_impl_t *impl = image_impl(self);
  sl_image_t *image;
  sl_image_paste_event_t event;
  char diagnostic[SL_ERROR_LEN];
  if (!impl || !sl_image_pending(impl))
    return;
  image = impl->image;
  image_error(impl, reason);
  (void)snprintf(diagnostic, sizeof(diagnostic), "%s", impl->error);
  memset(&event, 0, sizeof(event));
  event.operation = SL_IMAGE_PASTE_ABORT;
  event.offset = image->offset;
  event.media_type = image->media_type;
  event.filename = image->filename[0] ? image->filename : NULL;
  event.reason = diagnostic;
  /* Close acquisition before notifying the host. No callbacks may reenter. */
  image_source_close(impl);
  (void)image_callback(impl, &event);
  image_clear(impl);
}

int sl_image_pending(const sl_impl_t *impl) {
  return impl && impl->image && impl->image->phase;
}

int sl_image_start_source(sl_t *self, const sl_image_paste_source_t *source,
                          sl_image_paste_id_t *id) {
  sl_impl_t *impl = image_impl(self);
  sl_image_t *image;
  int64_t now = image_now();
  if (id)
    *id = 0;
  if (!impl || !id || !source || !source->next || !source->prepare ||
      !source->close || !impl->image_callback || impl->disable_image_paste ||
      sl_image_pending(impl) || impl->history_search_active || now < 0)
    return SL_ERROR_INVALID;
  if (!impl->image) {
    impl->image = calloc(1, sizeof(*impl->image));
    if (!impl->image)
      return SL_ERROR;
  }
  image = impl->image;
  if (image->serial == ULONG_MAX)
    return SL_ERROR_INVALID;
  image->source = *source;
  image->id = ++image->serial;
  image->anchor = impl->cursor;
  image->offset = 0;
  image->phase = 1;
  image->started = image->progress = now;
  *id = image->id;
  return SL_OK;
}

static int clipboard_next(void *context, sl_image_paste_event_t *event) {
  return sl_clipboard_next(context, event);
}
static int clipboard_prepare(void *context, sl_image_paste_wait_t *wait) {
  struct pollfd fds[SL_IMAGE_PASTE_MAX_FDS];
  int count, i;
  memset(wait, 0, sizeof(*wait));
  count = sl_clipboard_prepare(context, fds, SL_IMAGE_PASTE_MAX_FDS,
                               &wait->timeout_ms);
  if (count < 0)
    return SL_ERROR;
  wait->count = (size_t)count;
  for (i = 0; i < count; ++i) {
    wait->fds[i].fd = fds[i].fd;
    wait->fds[i].events =
        (unsigned int)((fds[i].events & POLLIN ? SL_WATCH_READ : 0) |
                       (fds[i].events & POLLOUT ? SL_WATCH_WRITE : 0));
  }
  return SL_OK;
}
static const char *clipboard_error(void *context) {
  return sl_clipboard_error_text(context);
}
static void clipboard_close(void *context) { sl_clipboard_close(context); }
int sl_image_start(sl_t *self, sl_image_paste_id_t *id) {
  sl_impl_t *impl = image_impl(self);
  sl_image_paste_source_t source;
  int result;
  if (!impl || !id || !impl->image_callback || impl->disable_image_paste ||
      sl_image_pending(impl))
    return SL_ERROR_INVALID;
  memset(&source, 0, sizeof(source));
  source.context = sl_clipboard_open(impl->image_limit);
  if (!source.context) {
    image_error(impl, sl_clipboard_error_text(NULL));
    return SL_ERROR;
  }
  source.next = clipboard_next;
  source.prepare = clipboard_prepare;
  source.error = clipboard_error;
  source.close = clipboard_close;
  result = sl_image_start_source(self, &source, id);
  if (result != SL_OK)
    clipboard_close(source.context);
  return result;
}

static int image_timeout(sl_impl_t *impl, int64_t now) {
  sl_image_t *image = impl->image;
  int64_t total, progress;
  int limit =
      image->phase == 1 ? impl->image_source_timeout : impl->image_host_timeout;
  if (now < 0 || now < image->started || now < image->progress)
    return 0;
  total = impl->image_total_timeout - (now - image->started);
  progress = limit - (now - image->progress);
  if (total <= 0 || progress <= 0)
    return 0;
  return (int)(total < progress ? total : progress);
}

int sl_image_prepare(sl_t *self, sl_image_paste_wait_t *wait) {
  sl_impl_t *impl = image_impl(self);
  int result = SL_OK, deadline;
  if (!impl || !wait)
    return SL_ERROR_INVALID;
  memset(wait, 0, sizeof(*wait));
  wait->timeout_ms = -1;
  if (!sl_image_pending(impl))
    return SL_OK;
  if (impl->image->phase == 4) {
    wait->timeout_ms = impl->readline_suspended || impl->edit.open ? -1 : 0;
    return SL_OK;
  }
  deadline = image_timeout(impl, image_now());
  if (impl->image->phase == 1) {
    size_t i;
    impl->image_callback_active = 1;
    result = impl->image->source.prepare(impl->image->source.context, wait);
    impl->image_callback_active = 0;
    if (result != SL_OK || wait->count > SL_IMAGE_PASTE_MAX_FDS ||
        wait->timeout_ms < -1)
      return SL_ERROR;
    for (i = 0; i < wait->count; ++i)
      if (wait->fds[i].fd < 0 || !wait->fds[i].events ||
          (wait->fds[i].events & ~(SL_WATCH_READ | SL_WATCH_WRITE |
                                   SL_WATCH_HANGUP | SL_WATCH_ERROR)))
        return SL_ERROR;
  }
  if (wait->timeout_ms < 0 || deadline < wait->timeout_ms)
    wait->timeout_ms = deadline;
  return SL_OK;
}

int sl_image_pump(sl_t *self) {
  sl_impl_t *impl = image_impl(self);
  sl_image_t *image;
  sl_image_paste_event_t event;
  int result, status;
  if (!impl)
    return SL_ERROR_INVALID;
  if (!sl_image_pending(impl))
    return SL_OK;
  image = impl->image;
  if (image->phase != 4 && !image_timeout(impl, image_now())) {
    sl_image_invalidate(self, image->phase == 1 ? "image source timed out"
                                                : "image upload timed out");
    return SL_OK;
  }
  if (image->phase == 4) {
    if (impl->readline_suspended || impl->edit.open)
      return SL_OK;
    /* Retire the anchor before editing so its own insertion cannot cancel it.
     */
    image->phase = 0;
    result = sl_image_insert_result(self, image->anchor, image->path);
    image->phase = 4;
    if (result != SL_OK)
      sl_image_invalidate(
          self, "uploaded image path exceeds draft limit or edit failed");
    else {
      memset(&event, 0, sizeof(event));
      event.operation = SL_IMAGE_PASTE_DONE;
      event.offset = image->offset;
      event.media_type = image->chunk.media_type;
      event.filename = image->chunk.filename;
      (void)image_callback(impl, &event);
      image_clear(impl);
    }
    return SL_OK;
  }
  if (image->phase != 1)
    return SL_OK;
  if (image->chunk.operation == SL_IMAGE_PASTE_DATA)
    event = image->chunk;
  else {
    memset(&event, 0, sizeof(event));
    impl->image_callback_active = 1;
    result = image->source.next(image->source.context, &event);
    impl->image_callback_active = 0;
    if (result < 0) {
      char reason[SL_ERROR_LEN];
      const char *text;
      impl->image_callback_active = 1;
      text = image->source.error ? image->source.error(image->source.context)
                                 : "image source failed";
      (void)snprintf(reason, sizeof(reason), "%s",
                     text ? text : "image source failed");
      impl->image_callback_active = 0;
      sl_image_invalidate(self, reason);
      return SL_OK;
    }
    if (!result)
      return SL_OK;
  }
  if (event.operation == SL_IMAGE_PASTE_BEGIN && !image->begun &&
      event.media_type &&
      (!strcmp(event.media_type, "image/png") ||
       !strcmp(event.media_type, "image/jpeg"))) {
    image->media_type =
        !strcmp(event.media_type, "image/png") ? "image/png" : "image/jpeg";
    (void)snprintf(image->filename, sizeof(image->filename), "%s",
                   event.filename ? event.filename : "image");
    image->begun = 1;
  } else if (!image->begun ||
             (event.operation != SL_IMAGE_PASTE_DATA &&
              event.operation != SL_IMAGE_PASTE_END) ||
             (event.operation == SL_IMAGE_PASTE_DATA &&
              (!event.data || !event.length ||
               event.length > SL_IMAGE_PASTE_CHUNK_MAX))) {
    sl_image_invalidate(self, "invalid image source event");
    return SL_OK;
  }
  if (event.offset != image->offset) {
    sl_image_invalidate(self, "invalid image source offset");
    return SL_OK;
  }
  event.media_type = image->media_type;
  event.filename = image->filename;
  image->chunk = event;
  if (event.operation == SL_IMAGE_PASTE_DATA &&
      (event.length > impl->image_limit - image->offset)) {
    sl_image_invalidate(self, "clipboard image exceeds byte limit");
    return SL_OK;
  }
  status = image_callback(impl, &event);
  if (event.operation == SL_IMAGE_PASTE_DATA && status == SL_ERROR_NOT_READY) {
    image->phase = 2;
    image->progress = image_now();
    return SL_OK;
  }
  if (status != SL_OK) {
    sl_image_invalidate(self, "image upload callback rejected transfer");
    return SL_OK;
  }
  image->progress = image_now();
  if (event.operation == SL_IMAGE_PASTE_DATA)
    image->offset += event.length;
  image->chunk.operation = 0;
  if (event.operation == SL_IMAGE_PASTE_END) {
    image->phase = 3;
    image_source_close(impl);
  }
  return SL_OK;
}

int sl_image_resume(sl_t *self, sl_image_paste_id_t id) {
  sl_impl_t *impl = image_impl(self);
  if (!impl || !id || !impl->image || impl->image->id != id ||
      impl->image->phase != 2)
    return SL_ERROR_INVALID;
  if (!image_timeout(impl, image_now())) {
    sl_image_invalidate(self, "image upload timed out");
    return SL_ERROR_INVALID;
  }
  impl->image->phase = 1;
  impl->image->progress = image_now();
  return SL_OK;
}

int sl_image_complete(sl_t *self, sl_image_paste_id_t id, const char *path,
                      const char *reason) {
  sl_impl_t *impl = image_impl(self);
  size_t length = 0;
  if (!impl || !id || !impl->image || impl->image->id != id ||
      impl->image->phase != 3)
    return SL_ERROR_INVALID;
  if (!image_timeout(impl, image_now())) {
    sl_image_invalidate(self, "image upload timed out");
    return SL_ERROR_INVALID;
  }
  if (!path) {
    sl_image_invalidate(self, reason);
    return SL_OK;
  }
  if (reason || path[0] != '/')
    return SL_ERROR_INVALID;
  while (length < SL_IMAGE_PATH_MAX && path[length]) {
    unsigned char ch = (unsigned char)path[length];
    if (ch < 32 || ch == 127)
      return SL_ERROR_INVALID;
    ++length;
  }
  if (length == SL_IMAGE_PATH_MAX || length < 2)
    return SL_ERROR_INVALID;
  if (!sl_image_valid_path(path))
    return SL_ERROR_INVALID;
  impl->image->path = malloc(length + 1);
  if (!impl->image->path)
    return SL_ERROR;
  memcpy(impl->image->path, path, length + 1);
  impl->image->phase = 4;
  return SL_OK;
}

int sl_image_cancel(sl_t *self, sl_image_paste_id_t id) {
  sl_impl_t *impl = image_impl(self);
  if (!impl || !id || !sl_image_pending(impl) || impl->image->id != id)
    return SL_ERROR_INVALID;
  sl_image_invalidate(self, "image upload cancelled");
  return SL_OK;
}

void sl_image_changed(sl_t *self, size_t at, size_t removed, size_t inserted) {
  sl_impl_t *impl = image_impl(self);
  size_t anchor;
  if (!sl_image_pending(impl))
    return;
  anchor = impl->image->anchor;
  if (removed && at <= anchor && removed > anchor - at) {
    sl_image_invalidate(self, "image upload cancelled: insertion removed");
    return;
  }
  /* A zero-width anchor stays before typing at its own position. */
  if (at < anchor)
    impl->image->anchor = anchor - removed + inserted;
}

void sl_image_destroy(sl_t *self) {
  sl_impl_t *impl = image_impl(self);
  if (!impl)
    return;
  sl_image_invalidate(self, "image upload cancelled: editor destroyed");
  free(impl->image);
  impl->image = NULL;
}

int sl_image_paste_start(sl_t *s, sl_image_paste_id_t *id) {
  return s && s->image_paste_start ? s->image_paste_start(s, id)
                                   : SL_ERROR_INVALID;
}
int sl_image_paste_start_source(sl_t *s, const sl_image_paste_source_t *source,
                                sl_image_paste_id_t *id) {
  return s && s->image_paste_start_source
             ? s->image_paste_start_source(s, source, id)
             : SL_ERROR_INVALID;
}
int sl_image_paste_prepare(sl_t *s, sl_image_paste_wait_t *w) {
  return s && s->image_paste_prepare ? s->image_paste_prepare(s, w)
                                     : SL_ERROR_INVALID;
}
int sl_image_paste_pump(sl_t *s) {
  return s && s->image_paste_pump ? s->image_paste_pump(s) : SL_ERROR_INVALID;
}
int sl_image_paste_resume(sl_t *s, sl_image_paste_id_t id) {
  return s && s->image_paste_resume ? s->image_paste_resume(s, id)
                                    : SL_ERROR_INVALID;
}
int sl_image_paste_complete(sl_t *s, sl_image_paste_id_t id, const char *p,
                            const char *r) {
  return s && s->image_paste_complete ? s->image_paste_complete(s, id, p, r)
                                      : SL_ERROR_INVALID;
}
int sl_image_paste_cancel(sl_t *s, sl_image_paste_id_t id) {
  return s && s->image_paste_cancel ? s->image_paste_cancel(s, id)
                                    : SL_ERROR_INVALID;
}
