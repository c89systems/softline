#ifndef SOFTLINE_TEST_CLIPBOARD_H
#define SOFTLINE_TEST_CLIPBOARD_H
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct target {
  const char *name;
  unsigned flags, info;
};
struct clipboard_api {
  int (*init)(void *, void *);
  void *(*atom)(const char *, int);
  void *(*get)(void *);
  int (*offer)(void *, const struct target *, unsigned,
               void (*)(void *, void *, unsigned, void *),
               void (*)(void *, void *), void *);
  void (*text)(void *, const char *, int);
  void (*selection)(void *, void *, int, const void *, int);
  int (*iterate)(void *, int);
};
static struct clipboard_api gtk;
static void *clipboard;
static double now(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec + time.tv_nsec / 1000000000.0;
}
static void pump(void) {
  while (gtk.iterate(NULL, 0)) {
  }
}
static int symbol(void *library, const char *name, void *output, size_t size) {
  void *value = dlsym(library, name);
  if (!value || size != sizeof(value)) {
    fprintf(stderr, "Clipboard test symbol unavailable: %s\n", name);
    return 1;
  }
  memcpy(output, &value, size);
  return 0;
}
#define BIND(library, member, name)                                            \
  do {                                                                         \
    if (symbol(library, name, &gtk.member, sizeof(gtk.member)))                \
      return 1;                                                                \
  } while (0)
static int load(void) {
  void *g = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
  void *d = dlopen("libgdk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
  void *l = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!g || !d || !l) {
    fprintf(stderr, "Clipboard GTK runtime unavailable: %s\n", dlerror());
    return 1;
  }
  BIND(g, init, "gtk_init_check");
  BIND(d, atom, "gdk_atom_intern");
  BIND(g, get, "gtk_clipboard_get");
  BIND(g, offer, "gtk_clipboard_set_with_data");
  BIND(g, text, "gtk_clipboard_set_text");
  BIND(g, selection, "gtk_selection_data_set");
  BIND(l, iterate, "g_main_context_iteration");
  if (!gtk.init(NULL, NULL))
    return 1;
  clipboard = gtk.get(gtk.atom("CLIPBOARD", 0));
  return clipboard == NULL;
}
struct image {
  unsigned char *bytes;
  size_t length;
  const char *mime;
  void *atom;
  unsigned deliveries;
};
static int image_load(struct image *image, const char *path, const char *mime) {
  FILE *file = fopen(path, "rb");
  long size;
  memset(image, 0, sizeof(*image));
  image->mime = mime;
  if (!file)
    return 1;
  if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET)) {
    fclose(file);
    return 1;
  }
  image->bytes = (unsigned char *)malloc((size_t)size + 1);
  image->length = (size_t)size;
  if (!image->bytes ||
      fread(image->bytes, 1, image->length, file) != image->length) {
    fclose(file);
    free(image->bytes);
    image->bytes = NULL;
    return 1;
  }
  fclose(file);
  return 0;
}
static void deliver(void *owner, void *selection, unsigned info, void *data) {
  struct image *image = (struct image *)data;
  (void)owner;
  (void)info;
  ++image->deliveries;
  gtk.selection(selection, image->atom, 8, image->bytes, (int)image->length);
}
static void released(void *owner, void *data) {
  (void)owner;
  (void)data;
}
static int offer(struct image *image) {
  struct target target;
  target.name = image->mime;
  target.flags = target.info = 0;
  image->atom = gtk.atom(image->mime, 0);
  if (!gtk.offer(clipboard, &target, 1, deliver, released, image))
    return 1;
  pump();
  return 0;
}
struct result {
  char bytes[8192], *line, *error;
};
static int run_case(const char *fixture, const char *base, const char *keys,
                    const char *mode, struct result *result) {
  struct winsize size;
  struct pollfd descriptors[2];
  int master = -1, slave = -1, output[2] = {-1, -1}, failed = 1, status,
      sent = 0;
  pid_t child = -1, waited;
  size_t length = 0, terminal_length = 0;
  ssize_t count;
  char descriptor[32], terminal[16384], *end;
  double deadline;
  memset(result, 0, sizeof(*result));
  memset(&size, 0, sizeof(size));
  size.ws_row = 12;
  size.ws_col = 80;
  if (openpty(&master, &slave, NULL, NULL, &size) || pipe(output))
    goto cleanup;
  snprintf(descriptor, sizeof(descriptor), "%d", output[1]);
  child = fork();
  if (child == 0) {
    close(master);
    close(output[0]);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
      close(slave);
    setenv("TERM", "xterm-256color", 1);
    if (base)
      setenv("XDG_CACHE_HOME", base, 1);
    if (mode)
      execl(fixture, fixture, descriptor, mode, (char *)NULL);
    else
      execl(fixture, fixture, descriptor, (char *)NULL);
    _exit(127);
  }
  if (child < 0)
    goto cleanup;
  close(slave);
  slave = -1;
  close(output[1]);
  output[1] = -1;
  descriptors[0].fd = master;
  descriptors[0].events = POLLIN;
  descriptors[1].fd = output[0];
  descriptors[1].events = POLLIN;
  deadline = now() + 20;
  terminal[0] = 0;
  do {
    pump();
    if (poll(descriptors, 2, 10) < 0)
      goto cleanup;
    if (descriptors[0].revents & POLLIN) {
      if (terminal_length + 1 == sizeof(terminal))
        goto cleanup;
      count = read(master, terminal + terminal_length,
                   sizeof(terminal) - terminal_length - 1);
      if (count > 0) {
        terminal_length += (size_t)count;
        terminal[terminal_length] = 0;
      }
      if (!sent && strstr(terminal, "> ")) {
        if (write(master, keys, strlen(keys)) != (ssize_t)strlen(keys))
          goto cleanup;
        sent = 1;
      }
    }
    if (descriptors[1].revents & POLLIN) {
      if (length + 1 == sizeof(result->bytes))
        goto cleanup;
      count = read(output[0], result->bytes + length,
                   sizeof(result->bytes) - length - 1);
      if (count > 0) {
        length += (size_t)count;
        result->bytes[length] = 0;
      }
    }
    waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      child = -1;
      break;
    }
    if (waited < 0)
      goto cleanup;
  } while (now() < deadline);
  if (child > 0 || !sent || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    goto cleanup;
  while ((count = read(output[0], result->bytes + length,
                       sizeof(result->bytes) - length - 1)) > 0)
    length += (size_t)count;
  result->bytes[length] = 0;
  result->line = result->bytes;
  end = strchr(result->line, '\n');
  if (!end)
    goto cleanup;
  *end = 0;
  result->error = end + 1;
  end = strchr(result->error, '\n');
  if (!end || end[1])
    goto cleanup;
  *end = 0;
  failed = 0;
cleanup:
  if (failed)
    fprintf(stderr, "Clipboard fixture failed or stalled\n");
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  if (master >= 0)
    close(master);
  if (slave >= 0)
    close(slave);
  if (output[0] >= 0)
    close(output[0]);
  if (output[1] >= 0)
    close(output[1]);
  return failed;
}
static int bytes_equal(const char *path, const struct image *image) {
  FILE *file = fopen(path, "rb");
  unsigned char buffer[8192];
  size_t offset = 0, count;
  if (!file)
    return 0;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (offset + count > image->length ||
        memcmp(buffer, image->bytes + offset, count)) {
      fclose(file);
      return 0;
    }
    offset += count;
  }
  count = ferror(file);
  fclose(file);
  return !count && offset == image->length;
}
static int secure_mode(const char *path, unsigned mode) {
  struct stat metadata;
  return !stat(path, &metadata) && (metadata.st_mode & 0777) == mode;
}
static int xid(const char *name) {
  unsigned timestamp = 0, index;
  const char *alphabet = "0123456789abcdefghijklmnopqrstuv", *digit;
  if (strlen(name) != 20)
    return 0;
  for (index = 0; index < 20; ++index) {
    digit = strchr(alphabet, name[index]);
    if (!digit)
      return 0;
    if (index < 6)
      timestamp = (timestamp << 5) | (unsigned)(digit - alphabet);
    else if (index == 6)
      timestamp = (timestamp << 2) | ((unsigned)(digit - alphabet) >> 3);
  }
  {
    unsigned current = (unsigned)time(NULL);
    return (timestamp >= current ? timestamp - current : current - timestamp) <
           60;
  }
}
static int image_path(const struct result *result, const char *prefix,
                      const char *suffix, const struct image *image,
                      const char *directory, char *output) {
  size_t begin = strlen(prefix), end = strlen(result->line),
         tail = strlen(suffix), length;
  char parent[4096], identifier[32], *separator, *extension;
  if (strncmp(result->line, prefix, begin) || end < begin + tail ||
      strcmp(result->line + end - tail, suffix))
    return 1;
  length = end - begin - tail;
  if (length >= 4096)
    return 1;
  memcpy(output, result->line + begin, length);
  output[length] = 0;
  strcpy(parent, output);
  separator = strrchr(parent, '/');
  if (!separator || output[0] != '/')
    return 1;
  *separator = 0;
  if (strcmp(parent, directory) || !secure_mode(output, 0600) ||
      !secure_mode(parent, 0700) || !bytes_equal(output, image))
    return 1;
  extension = strrchr(separator + 1, '.');
  if (!extension || (size_t)(extension - separator - 1) >= sizeof(identifier))
    return 1;
  memcpy(identifier, separator + 1, (size_t)(extension - separator - 1));
  identifier[extension - separator - 1] = 0;
  return !xid(identifier);
}
#endif
