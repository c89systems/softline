#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include "queue_demo_client.h"
#include "queue_demo_image.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

extern size_t image_fixture_size, image_fixture_reads;
extern int image_fixture_gate, image_fixture_fail;
extern const char *image_fixture_mime;
unsigned char image_fixture_byte(size_t offset);

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "chat image:%d: %s\n", __LINE__, #c);                    \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

static double milliseconds(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static int files(const char *root, int temporary, char *path, size_t capacity,
                 size_t *bytes) {
  DIR *d = opendir(root);
  struct dirent *e;
  struct stat st;
  int count = 0;
  if (!d)
    return -1;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    if ((e->d_name[0] == '.') != temporary)
      continue;
    if (snprintf(path, capacity, "%s/%s", root, e->d_name) >= (int)capacity ||
        stat(path, &st)) {
      closedir(d);
      return -1;
    }
    *bytes = (size_t)st.st_size;
    ++count;
  }
  closedir(d);
  return count;
}
static void remove_files(const char *root) {
  DIR *d = opendir(root);
  struct dirent *e;
  char path[4096];
  if (!d)
    return;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    if (snprintf(path, sizeof(path), "%s/%s", root, e->d_name) <
        (int)sizeof(path))
      (void)unlink(path);
  }
  closedir(d);
}
static int verify_file(const char *path, size_t length) {
  unsigned char bytes[4096];
  size_t at = 0, i;
  ssize_t n;
  struct stat st;
  int fd = open(path, O_RDONLY), failed = 0;
  if (fd < 0)
    return -1;
  if (fstat(fd, &st) || (st.st_mode & 0777) != 0600)
    failed = 1;
  while ((n = read(fd, bytes, sizeof(bytes))) > 0) {
    for (i = 0; i < (size_t)n; ++i)
      if (bytes[i] != image_fixture_byte(at + i))
        failed = 1;
    at += (size_t)n;
  }
  close(fd);
  return failed || n < 0 || at != length ? -1 : 0;
}
static void clear_reply(demo_link_t *link) {
  size_t i;
  for (i = 0; i < link->count; ++i)
    demo_packet_free(link->output + i);
  link->count = link->queued_bytes = 0;
}
static unsigned long reply_kind(demo_link_t *link) {
  demo_packet_t p;
  if (link->count != 1)
    return 0;
  p = link->output[0];
  p.offset = 4;
  return demo_number(&p);
}
static int store_message(demo_image_store_t *s, demo_link_t *link,
                         unsigned long kind, unsigned long id, size_t offset,
                         const char *mime, size_t count, double now) {
  demo_packet_t p = {0};
  unsigned char data[DEMO_IMAGE_CHUNK_MAX];
  size_t i;
  int result;
  clear_reply(link);
  demo_put_number(&p, id);
  if (kind == DEMO_IMAGE_BEGIN)
    demo_put_string(&p, mime);
  else if (kind == DEMO_IMAGE_DATA || kind == DEMO_IMAGE_END)
    demo_put_number(&p, (unsigned long)offset);
  if (kind == DEMO_IMAGE_DATA) {
    for (i = 0; i < count; ++i)
      data[i] = image_fixture_byte(offset + i);
    demo_put_image_bytes(&p, data, count);
  }
  result = demo_image_store_input(s, link, kind, &p, now);
  demo_packet_free(&p);
  return result;
}
static int store_tests(const char *build) {
  demo_image_store_t *s = calloc(1, sizeof(*s));
  demo_link_t link;
  char root[4096] = {0}, path[4096], saved[4096];
  size_t bytes, at, limit;
  unsigned long id = 0;
  int failed = 1, initialized = 0, mode;
  demo_link_init(&link, -1);
  CHECK(s != NULL);
  CHECK(snprintf(root, sizeof(root), "%s/chat-image-store-XXXXXX", build) <
        (int)sizeof(root));
  CHECK(mkdtemp(root) != NULL);
  CHECK(setenv("SOFTLINE_CHAT_IMAGE_DIR", root, 1) == 0);
  CHECK(unsetenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES") == 0);
  CHECK(demo_image_store_init(s) == 0);
  initialized = 1;
  CHECK(demo_image_limit(&limit) == 0 && limit == 33554432u);
  for (mode = 0; mode < 9; ++mode) {
    const char *mime = mode == 1 ? "image/jpeg" : "image/png";
    size_t length = mode == 2 || mode == 3 ? limit : 32771;
    ++id;
    CHECK(store_message(s, &link, DEMO_IMAGE_BEGIN, id, 0, mime, 0, 100) == 0);
    CHECK(reply_kind(&link) == DEMO_IMAGE_READY);
    CHECK(files(root, 1, path, sizeof(path), &bytes) == 1 && bytes == 0);
    CHECK(files(root, 0, path, sizeof(path), &bytes) == 0);
    for (at = 0; at < length;) {
      size_t count = length - at;
      if (count > DEMO_IMAGE_CHUNK_MAX)
        count = DEMO_IMAGE_CHUNK_MAX;
      CHECK(store_message(s, &link, DEMO_IMAGE_DATA, id, at, NULL, count,
                          101) == 0);
      CHECK(reply_kind(&link) == DEMO_IMAGE_READY);
      at += count;
    }
    CHECK(files(root, 1, path, sizeof(path), &bytes) == 1 && bytes == length);
    CHECK(verify_file(path, length) == 0);
    if (mode == 3) {
      CHECK(store_message(s, &link, DEMO_IMAGE_DATA, id, at, NULL, 1, 102) ==
            0);
      CHECK(reply_kind(&link) == DEMO_IMAGE_RESULT);
      CHECK(files(root, 1, path, sizeof(path), &bytes) == 0);
      continue;
    }
    if (mode == 4 || mode == 5) {
      if (mode == 4)
        CHECK(store_message(s, &link, DEMO_IMAGE_ABORT, id, 0, NULL, 0, 102) ==
              0);
      else {
        clear_reply(&link);
        CHECK(demo_image_store_tick(s, &link, 30102) == 0);
        CHECK(reply_kind(&link) == DEMO_IMAGE_RESULT);
      }
      CHECK(files(root, 1, path, sizeof(path), &bytes) == 0);
      continue;
    }
    if (mode == 6) {
      demo_image_store_close(s);
      CHECK(files(root, 1, path, sizeof(path), &bytes) == 0);
      continue;
    }
    CHECK(store_message(s, &link, DEMO_IMAGE_END, id, at, NULL, 0, 102) == 0);
    CHECK(reply_kind(&link) == DEMO_IMAGE_RESULT);
    CHECK(files(root, 0, path, sizeof(path), &bytes) == 1);
    CHECK(strstr(path, mode == 1 ? ".jpeg" : ".png") != NULL);
    CHECK(verify_file(path, length) == 0);
    CHECK(files(root, 1, saved, sizeof(saved), &bytes) == 0);
    if (mode == 7) {
      /* A suspended editor can retain an acknowledged result indefinitely.
       * Its eventual cancellation must still remove the unreferenced file. */
      CHECK(demo_image_store_tick(s, &link, 1000000) == 0);
      CHECK(store_message(s, &link, DEMO_IMAGE_ABORT, id, 0, NULL, 0, 103) ==
            0);
      CHECK(access(path, F_OK) != 0);
    } else {
      if (mode == 8)
        demo_image_store_close(s);
      else
        CHECK(store_message(s, &link, DEMO_IMAGE_DONE, id, 0, NULL, 0, 103) ==
              0);
      CHECK(verify_file(path, length) == 0);
      CHECK(unlink(path) == 0);
    }
  }
  /* Server policy is independent of the frontend, and validates offsets/EOF. */
  CHECK(setenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES", "32", 1) == 0);
  CHECK(demo_image_store_init(s) == 0);
  for (mode = 0; mode < 4; ++mode) {
    ++id;
    CHECK(store_message(s, &link, DEMO_IMAGE_BEGIN, id, 0, "image/png", 0,
                        200) == 0);
    CHECK(store_message(s, &link, DEMO_IMAGE_DATA, id, mode == 0 ? 1 : 0, NULL,
                        mode == 1 ? 33 : 32, 201) == 0);
    if (mode < 2)
      CHECK(reply_kind(&link) == DEMO_IMAGE_RESULT);
    else {
      CHECK(reply_kind(&link) == DEMO_IMAGE_READY);
      CHECK(store_message(s, &link, DEMO_IMAGE_END, id, mode == 2 ? 31 : 32,
                          NULL, 0, 202) == 0);
      CHECK(reply_kind(&link) == DEMO_IMAGE_RESULT);
      if (mode == 3) {
        CHECK(files(root, 0, path, sizeof(path), &bytes) == 1);
        CHECK(verify_file(path, 32) == 0);
        demo_image_store_close(s);
        CHECK(unlink(path) == 0);
      }
    }
    CHECK(files(root, 1, path, sizeof(path), &bytes) == 0);
  }
  CHECK(setenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES", "67108864", 1) == 0);
  CHECK(demo_image_limit(&bytes) == 0 && bytes == 67108864u);
  CHECK(setenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES", "0", 1) == 0);
  CHECK(demo_image_limit(&bytes) == 0 && bytes == limit);
  CHECK(setenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES", "-1", 1) == 0);
  CHECK(demo_image_limit(&bytes) != 0);
  CHECK(setenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES", "2147483648", 1) == 0);
  CHECK(demo_image_limit(&bytes) != 0);
  CHECK(unsetenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES") == 0);
  CHECK(demo_image_store_init(s) == 0);
  ++id;
  CHECK(store_message(s, &link, DEMO_IMAGE_BEGIN, id, 0, "text/plain", 0,
                      1000) == 0);
  CHECK(reply_kind(&link) == DEMO_IMAGE_RESULT);
  CHECK(files(root, 1, path, sizeof(path), &bytes) == 0);
  ++id;
  CHECK(store_message(s, &link, DEMO_IMAGE_BEGIN, id, 0, "image/png", 0,
                      1000) == 0);
  for (mode = 0; mode < 4; ++mode)
    CHECK(store_message(s, &link, DEMO_IMAGE_DATA, id, (size_t)mode, NULL, 1,
                        30000.0 + mode * 29000.0) == 0);
  clear_reply(&link);
  CHECK(demo_image_store_tick(s, &link, 121000) == 0);
  CHECK(reply_kind(&link) == DEMO_IMAGE_RESULT);
  CHECK(files(root, 1, path, sizeof(path), &bytes) == 0);
  ++id;
  CHECK(store_message(s, &link, DEMO_IMAGE_BEGIN, id, 0, "image/png", 0,
                      1000) == 0);
  CHECK(store_message(s, &link, DEMO_IMAGE_DATA, id, 0, NULL, 32, 1001) == 0);
  CHECK(files(root, 1, path, sizeof(path), &bytes) == 1);
  CHECK(unlink(path) == 0); /* Failed publication must never return a path. */
  CHECK(store_message(s, &link, DEMO_IMAGE_END, id, 32, NULL, 0, 1002) == 0);
  CHECK(reply_kind(&link) == DEMO_IMAGE_RESULT);
  CHECK(files(root, 0, path, sizeof(path), &bytes) == 0);
  failed = 0;
cleanup:
  if (initialized)
    demo_image_store_close(s);
  free(s);
  demo_link_close(&link);
  if (*root) {
    remove_files(root);
    (void)rmdir(root);
  }
  return failed;
}

struct observation {
  sl_image_paste_callback_t callback;
  void *context;
  int begin, data, end, done, aborts;
  sl_t *editor;
  int notifications;
  char reason[160];
};
static int host_image_event(void *context, int kind, const char *text) {
  struct observation *o = context;
  if (kind < DEMO_IMAGE_STARTED || kind > DEMO_IMAGE_FAILED)
    return SL_OK;
  ++o->notifications;
  if (kind == DEMO_IMAGE_FAILED)
    (void)snprintf(o->reason, sizeof(o->reason), "%s", text ? text : "");
  /* This must run after the acquisition callback, when host UI calls work. */
  return sl_set_status_message(o->editor, "HOST OWNS STATUS");
}
static int observe(void *context, const sl_image_paste_event_t *e) {
  struct observation *o = context;
  int result = o->callback(o->context, e);
  if (e->operation == SL_IMAGE_PASTE_BEGIN)
    ++o->begin;
  if (e->operation == SL_IMAGE_PASTE_DATA && result == SL_OK)
    ++o->data;
  if (e->operation == SL_IMAGE_PASTE_END)
    ++o->end;
  if (e->operation == SL_IMAGE_PASTE_DONE)
    ++o->done;
  if (e->operation == SL_IMAGE_PASTE_ABORT)
    ++o->aborts;
  return result;
}
static pid_t server(const char *executable, const char *root, const char *limit,
                    unsigned short *port) {
  int output[2], status;
  pid_t child;
  char line[128];
  FILE *stream;
  struct pollfd ready;
  unsigned int number;
  if (pipe(output))
    return -1;
  child = fork();
  if (!child) {
    close(output[0]);
    if (dup2(output[1], STDOUT_FILENO) < 0 || chdir(root))
      _exit(2);
    close(output[1]);
    unsetenv("SOFTLINE_CHAT_IMAGE_DIR");
    if (limit)
      setenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES", limit, 1);
    else
      unsetenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES");
    execl(executable, executable, "0", (char *)NULL);
    _exit(127);
  }
  close(output[1]);
  ready.fd = output[0];
  ready.events = POLLIN;
  ready.revents = 0;
  stream = fdopen(output[0], "r");
  status = child > 0 && stream && poll(&ready, 1, 3000) == 1 &&
           fgets(line, sizeof(line), stream) &&
           sscanf(line, "LISTEN 127.0.0.1 %u", &number) == 1;
  if (stream)
    fclose(stream);
  else
    close(output[0]);
  if (!status) {
    if (child > 0) {
      kill(child, SIGKILL);
      waitpid(child, NULL, 0);
    }
    return -1;
  }
  *port = (unsigned short)number;
  return child;
}
static void stop_view(sl_t *sl, void *context) {
  (void)context;
  (void)sl_cancel(sl);
}
static int default_status_unchanged(sl_t *sl, int master) {
  char output[8192];
  size_t used = 0;
  ssize_t count;
  if (sl_set_idle_callback(sl, stop_view, NULL) != SL_OK ||
      sl_readline(sl, "> ") != NULL || fcntl(master, F_SETFL, O_NONBLOCK))
    return -1;
  while (used + 1 < sizeof(output)) {
    count = read(master, output + used, sizeof(output) - used - 1);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    used += (size_t)count;
  }
  output[used] = 0;
  return strstr(output, "HOST OWNS STATUS") &&
                 !strstr(output, "image upload") &&
                 !strstr(output, "server byte limit")
             ? 0
             : -1;
}
static int network_case(const char *executable, const char *build, int mode) {
  char root[4096] = {0}, path[4096], expected[8192];
  size_t bytes = 0, reads;
  unsigned short port = 0;
  pid_t child = -1;
  int failed = 1, output = -1, master = -1, i;
  double deadline;
  demo_client_t *client = NULL;
  sl_t *sl = NULL;
  sl_config_t config;
  sl_image_paste_id_t id;
  struct observation observation;
  sl_queue_backend_info_t info;
  memset(&observation, 0, sizeof(observation));
  CHECK(snprintf(root, sizeof(root), "%s/chat-image-network-XXXXXX", build) <
        (int)sizeof(root));
  CHECK(mkdtemp(root) != NULL);
  child =
      server(executable, root, mode == 3 || mode == 8 ? "16384" : NULL, &port);
  CHECK(child > 0);
  CHECK(unsetenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES") == 0);
  client = demo_client_create("127.0.0.1", port);
  CHECK(client != NULL);
  CHECK(openpty(&master, &output, NULL, NULL, NULL) == 0);
  CHECK(setenv("TERM", "xterm-256color", 1) == 0);
  sl_config_init(&config);
  config.input_fd = config.output_fd = output;
  config.prompt_queue = 1;
  config.queue_backend = demo_client_backend(client);
  config.queue_backend_context = client;
  config.image_paste_path_template = "/proc/softline-must-not-stage/*";
  CHECK(demo_client_image_config(client, &config) == SL_OK);
  if (mode == 7)
    config.image_paste_host_timeout_ms = 30;
  observation.callback = config.image_paste_callback;
  observation.context = config.image_paste_context;
  config.image_paste_callback = observe;
  config.image_paste_context = &observation;
  sl = sl_create_with_config(&config);
  CHECK(sl != NULL && demo_client_attach(client, sl) == SL_OK);
  deadline = milliseconds() + 3000;
  do {
    CHECK(demo_client_service(client, 1) == SL_OK);
    CHECK(sl_queue_backend_info(sl, &info) == SL_OK);
  } while (!info.synchronized && milliseconds() < deadline);
  CHECK(info.synchronized);
  if (mode == 8) {
    observation.editor = sl;
    demo_client_events(client, host_image_event, &observation);
  }
  CHECK(sl_set_status_message(sl, "HOST OWNS STATUS") == SL_OK);
  CHECK(sl_set_buffer(sl, "draft ") == SL_OK);
  image_fixture_size = 65541;
  image_fixture_reads = 0;
  image_fixture_fail = 0;
  image_fixture_gate = 1;
  image_fixture_mime = mode == 1 ? "image/jpeg" : "image/png";
  CHECK(sl->image_paste_start(sl, &id) == SL_OK);
  deadline = milliseconds() + 3000;
  do {
    CHECK(sl->image_paste_pump(sl) == SL_OK);
    CHECK(demo_client_service(client, 1) == SL_OK);
    i = files(root, 1, path, sizeof(path), &bytes);
  } while ((i != 1 || !bytes) && !observation.aborts &&
           milliseconds() < deadline);
  CHECK(i == 1 && bytes == DEMO_IMAGE_CHUNK_MAX && !observation.end);
  CHECK(verify_file(path, bytes) == 0);
  CHECK(!strcmp(sl_buffer(sl), "draft "));
  CHECK(files(root, 0, path, sizeof(path), &bytes) == 0);
  CHECK(sl_insert(sl, "tail") == SL_OK);
  if (mode == 5) {
    CHECK(kill(child, SIGTERM) == 0);
    CHECK(waitpid(child, NULL, 0) == child);
    child = -1;
    deadline = milliseconds() + 3000;
    do {
      (void)demo_client_service(client, 1);
    } while (!observation.aborts && milliseconds() < deadline);
    CHECK(observation.aborts == 1 && !observation.done);
    CHECK(!strcmp(sl_buffer(sl), "draft tail"));
    CHECK(files(root, 1, path, sizeof(path), &bytes) == 0);
  } else if (mode == 2 || mode == 4) {
    if (mode == 2)
      CHECK(sl->image_paste_cancel(sl, id) == SL_OK);
    else {
      sl_destroy(sl);
      sl = NULL;
    }
    deadline = milliseconds() + 3000;
    do {
      if (sl)
        CHECK(demo_client_service(client, 1) == SL_OK);
      i = files(root, 1, path, sizeof(path), &bytes);
    } while (i && milliseconds() < deadline);
    CHECK(i == 0 && observation.aborts == 1 && !observation.done);
    if (sl)
      CHECK(!strcmp(sl_buffer(sl), "draft tail"));
  } else {
    /* A stopped server bounds accepted chunks despite a fast producer. */
    CHECK(kill(child, SIGSTOP) == 0);
    image_fixture_gate = 0;
    for (i = 0; i < 100; ++i) {
      CHECK(sl->image_paste_pump(sl) == SL_OK);
      CHECK(demo_client_service(client, 0) == SL_OK);
    }
    reads = image_fixture_reads;
    CHECK(reads <= 3 && !observation.end);
    for (i = 0; i < 100; ++i)
      CHECK(sl->image_paste_pump(sl) == SL_OK);
    CHECK(image_fixture_reads == reads);
    if (mode != 7)
      CHECK(kill(child, SIGCONT) == 0);
    deadline = milliseconds() + 5000;
    do {
      if (mode == 6 && observation.end) {
        sl_image_paste_wait_t wait;
        CHECK(sl->image_paste_prepare(sl, &wait) == SL_OK);
        if (wait.timeout_ms == 0) {
          CHECK(!strcmp(sl_buffer(sl), "draft tail"));
          CHECK(sl->image_paste_cancel(sl, id) == SL_OK);
          break;
        }
      }
      if (mode != 6 || !observation.end)
        CHECK(sl->image_paste_pump(sl) == SL_OK);
      CHECK(demo_client_service(client, 1) == SL_OK);
    } while (!observation.done && !observation.aborts &&
             milliseconds() < deadline);
    if (mode == 3 || mode == 6 || mode == 7 || mode == 8) {
      CHECK(kill(child, SIGCONT) == 0);
      CHECK(observation.aborts == 1 && !observation.done);
      CHECK(!strcmp(sl_buffer(sl), "draft tail"));
      deadline = milliseconds() + 3000;
      do {
        CHECK(demo_client_service(client, 1) == SL_OK);
        i = files(root, 0, path, sizeof(path), &bytes) +
            files(root, 1, path, sizeof(path), &bytes);
      } while (i && milliseconds() < deadline);
      CHECK(i == 0);
      if (mode == 8)
        CHECK(observation.notifications >= 2 &&
              strstr(observation.reason, "server byte limit"));
      CHECK(files(root, 0, path, sizeof(path), &bytes) == 0);
    } else {
      CHECK(observation.done == 1 && !observation.aborts &&
            observation.end == 1);
      CHECK(files(root, 0, path, sizeof(path), &bytes) == 1);
      CHECK(verify_file(path, image_fixture_size) == 0);
      CHECK(snprintf(expected, sizeof(expected), "draft %stail", path) <
            (int)sizeof(expected));
      CHECK(!strcmp(sl_buffer(sl), expected));
      CHECK(sl_prompt_queue_count(sl) == 0); /* Upload never submits a turn. */
      CHECK(sl->image_paste_complete(sl, id, path, NULL) == SL_ERROR_INVALID);
      for (i = 0; i < 4; ++i)
        CHECK(demo_client_service(client, 1) == SL_OK);
      CHECK(verify_file(path, image_fixture_size) == 0);
    }
    CHECK(files(root, 1, path, sizeof(path), &bytes) == 0);
  }
  if (sl)
    CHECK(default_status_unchanged(sl, master) == 0);
  failed = 0;
cleanup:
  if (sl)
    sl_destroy(sl);
  demo_client_destroy(client);
  if (output >= 0)
    close(output);
  if (master >= 0)
    close(master);
  if (child > 0) {
    (void)kill(child, SIGCONT);
    (void)kill(child, SIGTERM);
    (void)waitpid(child, NULL, 0);
  }
  if (*root) {
    remove_files(root);
    (void)rmdir(root);
  }
  return failed;
}
int main(int argc, char **argv) {
  int mode;
  if (argc == 3 && !strcmp(argv[1], "store"))
    return store_tests(argv[2]);
  if (argc != 3)
    return 2;
  for (mode = 0; mode < 9; ++mode)
    if (network_case(argv[1], argv[2], mode))
      return 1;
  return 0;
}
