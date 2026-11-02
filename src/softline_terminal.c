#include "softline_internal.h"
#include "softline_pty.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>

#define TERMINAL_CHUNK 4096u

typedef struct terminal_relay {
  char input[TERMINAL_CHUNK], output[TERMINAL_CHUNK];
  size_t input_len, output_len;
  int beginning, tilde;
} terminal_relay_t;

void sl_terminal_reap(sl_impl_t *impl) {
  sl_pty_t **at = &impl->terminal_children;
  while (*at) {
    sl_pty_t *p = *at;
    if (sl_pty_reap(p)) {
      *at = p->next;
      free(p);
    } else
      at = &p->next;
  }
}

void sl_terminal_cleanup(sl_impl_t *impl) {
  struct timespec delay;
  int remaining = 100;
  delay.tv_sec = 0;
  delay.tv_nsec = 1000000;
  while (impl->terminal_children && remaining--) {
    sl_terminal_reap(impl);
    if (impl->terminal_children)
      (void)nanosleep(&delay, NULL);
  }
  while (impl->terminal_children) {
    sl_pty_t *p = impl->terminal_children;
    impl->terminal_children = p->next;
    free(p);
  }
}

static int preserve_input(sl_impl_t *impl, const char *bytes, size_t length) {
  char *data;
  if (!length)
    return SL_OK;
  data = realloc(impl->pending_input, impl->pending_input_len + length);
  if (!data)
    return SL_ERROR_NOMEM;
  impl->pending_input = data;
  memmove(data + length, data, impl->pending_input_len);
  memcpy(data, bytes, length);
  impl->pending_input_len += length;
  impl->pending_input_cap = impl->pending_input_len;
  return SL_OK;
}
static int input_bytes(sl_impl_t *impl, terminal_relay_t *r, const char *bytes,
                       size_t length) {
  size_t i;
  int overflow = 0;
  for (i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)bytes[i];
    if (r->tilde) {
      r->tilde = 0;
      if (c == '.') {
        int status = preserve_input(impl, bytes + i + 1, length - i - 1);
        return status == SL_OK ? SL_TERMINAL_ESCAPED : status;
      }
      if (r->input_len < sizeof(r->input))
        r->input[r->input_len++] = '~';
      else
        overflow = 1;
      r->beginning = 0;
      if (c == '~')
        continue;
    } else if (r->beginning && c == '~') {
      r->tilde = 1;
      continue;
    }
    if (r->input_len < sizeof(r->input))
      r->input[r->input_len++] = (char)c;
    else
      overflow = 1;
    r->beginning = c == '\r' || c == '\n';
  }
  return overflow ? SL_ERROR_FULL : SL_OK;
}
static unsigned int terminal_events(short value) {
  unsigned int events = 0;
  if (value & POLLIN)
    events |= SL_WATCH_READ;
  if (value & POLLOUT)
    events |= SL_WATCH_WRITE;
  if (value & (POLLERR | POLLNVAL))
    events |= SL_WATCH_ERROR;
  if (value & POLLHUP)
    events |= SL_WATCH_HANGUP;
  return events;
}
static int call_adapter(sl_terminal_callback_t callback, void *context,
                        sl_terminal_event_t *event) {
  int status = callback(context, event);
  return status > SL_OK ? SL_ERROR_INVALID : status;
}
static int relay_loop(sl_impl_t *impl, sl_terminal_callback_t callback,
                      void *context, terminal_relay_t *r) {
  sl_terminal_event_t e;
  struct winsize size;
  unsigned int rows = 0, columns = 0, events = 0;
  int eof = 0, opened = 0, status = SL_OK;
  r->beginning = 1;
  memset(&e, 0, sizeof(e));
  if (ioctl(impl->output_fd, TIOCGWINSZ, &size) || !size.ws_row || !size.ws_col)
    return SL_ERROR_IO;
  rows = size.ws_row;
  columns = size.ws_col;
  e.operation = SL_TERMINAL_OPEN;
  e.rows = rows;
  e.columns = columns;
  opened = 1;
  status = call_adapter(callback, context, &e);
  while (status == SL_OK) {
    struct pollfd fds[3];
    char bytes[256];
    ssize_t n;
    int ready, timeout, adapter_ready;
    if (!ioctl(impl->output_fd, TIOCGWINSZ, &size) && size.ws_row &&
        size.ws_col && (rows != size.ws_row || columns != size.ws_col)) {
      rows = size.ws_row;
      columns = size.ws_col;
      memset(&e, 0, sizeof(e));
      e.operation = SL_TERMINAL_RESIZE;
      e.rows = rows;
      e.columns = columns;
      status = call_adapter(callback, context, &e);
      if (status != SL_OK)
        break;
    }
    memset(&e, 0, sizeof(e));
    e.operation = SL_TERMINAL_STEP;
    e.rows = rows;
    e.columns = columns;
    e.fd = -1;
    e.timeout_ms = -1;
    e.events = events;
    status = call_adapter(callback, context, &e);
    if (status != SL_OK)
      break;
    if (e.fd < -1 || e.timeout_ms < -1 || (e.ready != 0 && e.ready != 1) ||
        (e.interests & ~(SL_WATCH_READ | SL_WATCH_WRITE))) {
      status = SL_ERROR_INVALID;
      break;
    }
    adapter_ready = e.ready;
    fds[2].fd = e.fd;
    fds[2].events = (e.interests & SL_WATCH_READ ? POLLIN : 0) |
                    (e.interests & SL_WATCH_WRITE ? POLLOUT : 0);
    if (r->output_len == sizeof(r->output) && adapter_ready)
      fds[2].events &= ~POLLIN;
    if (!fds[2].events)
      fds[2].fd = -1;
    timeout = e.timeout_ms < 0 || e.timeout_ms > 50 ? 50 : e.timeout_ms;
    if (adapter_ready && r->input_len) {
      memset(&e, 0, sizeof(e));
      e.operation = SL_TERMINAL_WRITE;
      e.data = r->input;
      e.length = r->input_len;
      status = call_adapter(callback, context, &e);
      if (status == SL_OK && (!e.transferred || e.transferred > r->input_len))
        status = SL_ERROR_INVALID;
      if (status == SL_OK) {
        r->input_len -= e.transferred;
        memmove(r->input, r->input + e.transferred, r->input_len);
        timeout = 0;
      } else if (status == SL_ERROR_NOT_READY)
        status = SL_OK;
      if (status != SL_OK)
        break;
    }
    if (adapter_ready && !eof && r->output_len < sizeof(r->output)) {
      memset(&e, 0, sizeof(e));
      e.operation = SL_TERMINAL_READ;
      e.data = r->output + r->output_len;
      e.length = sizeof(r->output) - r->output_len;
      status = call_adapter(callback, context, &e);
      if (e.transferred > e.length)
        status = SL_ERROR_INVALID;
      if (status == SL_OK) {
        r->output_len += e.transferred;
        eof = !e.transferred;
        timeout = 0;
      } else if (status == SL_ERROR_NOT_READY)
        status = SL_OK;
      if (status != SL_OK)
        break;
    }
    if (r->output_len) {
      n = write(impl->output_fd, r->output, r->output_len);
      if (n > 0) {
        r->output_len -= (size_t)n;
        memmove(r->output, r->output + n, r->output_len);
        timeout = 0;
      } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR) {
        status = SL_ERROR_IO;
        break;
      }
    }
    if (eof && !r->output_len)
      break;
    if (impl->pending_input_len) {
      char byte = impl->pending_input[0];
      --impl->pending_input_len;
      memmove(impl->pending_input, impl->pending_input + 1,
              impl->pending_input_len);
      status = input_bytes(impl, r, &byte, 1);
      continue;
    }
    fds[0].fd = impl->input_fd;
    fds[0].events = POLLIN;
    fds[1].fd = impl->output_fd;
    fds[1].events = r->output_len ? POLLOUT : 0;
    fds[0].revents = fds[1].revents = fds[2].revents = 0;
    ready = poll(fds, 3, timeout);
    if (ready < 0 && errno != EINTR) {
      status = SL_ERROR_IO;
      break;
    }
    events = terminal_events(fds[2].revents);
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      n = read(impl->input_fd, bytes, sizeof(bytes));
      if (n > 0)
        status = input_bytes(impl, r, bytes, (size_t)n);
      else if (!n ||
               (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        status = SL_ERROR_IO;
    }
  }
  if (opened) {
    int closed;
    memset(&e, 0, sizeof(e));
    e.operation = SL_TERMINAL_CLOSE;
    closed = call_adapter(callback, context, &e);
    if (status == SL_OK && closed != SL_OK)
      status = closed;
  }
  return status;
}
int sl_terminal_handoff(sl_t *self, sl_terminal_callback_t callback,
                        void *context) {
  sl_impl_t *impl = self ? self->impl : NULL;
  terminal_relay_t *relay;
  struct termios original, raw, original_output, raw_output;
  int input_flags, output_flags, stream, status, restore;
  if (!impl || !callback || impl->active_readline || impl->terminal_active ||
      impl->history_busy || impl->watch_callback_depth ||
      sl_backend_callback_active(impl))
    return SL_ERROR_INVALID;
  if (!isatty(impl->input_fd) || !isatty(impl->output_fd))
    return SL_ERROR_UNSUPPORTED;
  relay = calloc(1, sizeof(*relay));
  if (!relay)
    return SL_ERROR_NOMEM;
  stream = impl->output_stream_active;
  status = stream ? sl_output_stream_end(self) : SL_OK;
  if (status != SL_OK) {
    free(relay);
    return status;
  }
  impl->terminal_active = 1;
  input_flags = fcntl(impl->input_fd, F_GETFL);
  output_flags = fcntl(impl->output_fd, F_GETFL);
  status = SL_ERROR_IO;
  if (tcgetattr(impl->input_fd, &original) == 0 &&
      tcgetattr(impl->output_fd, &original_output) == 0) {
    raw = original;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL |
                     IXON | INPCK);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (input_flags >= 0 && output_flags >= 0 &&
        tcsetattr(impl->input_fd, TCSANOW, &raw) == 0 &&
        fcntl(impl->input_fd, F_SETFL, input_flags | O_NONBLOCK) == 0 &&
        fcntl(impl->output_fd, F_SETFL, output_flags | O_NONBLOCK) == 0 &&
        tcgetattr(impl->output_fd, &raw_output) == 0) {
      raw_output.c_oflag &= ~OPOST;
      if (tcsetattr(impl->output_fd, TCSANOW, &raw_output) == 0)
        status = relay_loop(impl, callback, context, relay);
    }
    if (tcsetattr(impl->output_fd, TCSANOW, &original_output))
      status = SL_ERROR_IO;
    restore = tcsetattr(impl->input_fd, TCSANOW, &original);
    if (restore)
      status = SL_ERROR_IO;
  }
  if (input_flags >= 0 && fcntl(impl->input_fd, F_SETFL, input_flags))
    status = SL_ERROR_IO;
  if (output_flags >= 0 && fcntl(impl->output_fd, F_SETFL, output_flags))
    status = SL_ERROR_IO;
  impl->terminal_active = 0;
  free(relay);
  if (stream && sl_output_stream_begin(self) != SL_OK)
    status = SL_ERROR_IO;
  return status;
}
int sl_terminal_local(sl_t *self, const char *shell, int *exit_status) {
  sl_impl_t *impl = self ? self->impl : NULL;
  sl_pty_t *pty;
  int status;
  if (exit_status)
    *exit_status = -1;
  if (!impl)
    return SL_ERROR_INVALID;
  if (!shell || !*shell)
    shell = getenv("SHELL");
  if (!shell || !*shell)
    shell = "/bin/sh";
  pty = calloc(1, sizeof(*pty));
  if (!pty)
    return SL_ERROR_NOMEM;
  pty->fd = -1;
  pty->exec_fd = -1;
  pty->shell = shell;
  pty->status = -1;
  status = sl_terminal_handoff(self, sl_pty_callback, pty);
  (void)sl_pty_reap(pty);
  if (exit_status)
    *exit_status = pty->status;
  if (pty->pid) {
    pty->next = impl->terminal_children;
    impl->terminal_children = pty;
  } else
    free(pty);
  sl_terminal_reap(impl);
  return status;
}
