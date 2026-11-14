#include "clipboard_test.h"
#include <dirent.h>

struct file_set {
  char names[65536];
  char *paths[512];
  size_t used, count;
};
static int path_order(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int collect_paths(const char *path, struct file_set *set) {
  DIR *directory = opendir(path);
  struct dirent *entry;
  struct stat info;
  char child[4096];
  if (!directory)
    return 1;
  while ((entry = readdir(directory)) != NULL) {
    size_t length;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            (int)sizeof(child) ||
        lstat(child, &info)) {
      closedir(directory);
      return 1;
    }
    length = strlen(child) + 1;
    if (set->used + length > sizeof(set->names) || set->count == 512) {
      closedir(directory);
      return 1;
    }
    set->paths[set->count++] = set->names + set->used;
    memcpy(set->names + set->used, child, length);
    set->used += length;
    if (S_ISDIR(info.st_mode) && collect_paths(child, set)) {
      closedir(directory);
      return 1;
    }
  }
  closedir(directory);
  return 0;
}
static int inventory(const char *path, struct file_set *set) {
  memset(set, 0, sizeof(*set));
  if (collect_paths(path, set))
    return 1;
  qsort(set->paths, set->count, sizeof(set->paths[0]), path_order);
  return 0;
}
static void remove_owned(const char *path) {
  DIR *directory = opendir(path);
  struct dirent *entry;
  struct stat info;
  char child[4096];
  if (!directory)
    return;
  while ((entry = readdir(directory)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            (int)sizeof(child) ||
        lstat(child, &info))
      continue;
    if (S_ISDIR(info.st_mode)) {
      remove_owned(child);
      rmdir(child);
    } else
      unlink(child);
  }
  closedir(directory);
}
static int unchanged(const char *base, const struct file_set *before) {
  struct file_set after;
  size_t index;
  if (inventory(base, &after) || before->count != after.count)
    return 0;
  for (index = 0; index < before->count; ++index)
    if (strcmp(before->paths[index], after.paths[index]))
      return 0;
  return 1;
}
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "Clipboard assertion: %s\nline=%s\nerror=%s\n", #c,      \
              result.line ? result.line : "",                                  \
              result.error ? result.error : "");                               \
      failed = 1;                                                              \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
static int local_cases(const char *fixture, const char *build,
                       const char *inputs) {
  struct image small, large, oversized, jpeg, invalid;
  struct result result;
  char base[4096], input[4096], directory[4096], path[4096], other[4096],
      template[4096], *name, *end;
  const char *tokens[] = {"{{XDG_CACHE_HOME}}", "{{xdg_cache_home}}",
                          "{{HOME}}", "{{home}}", "~"};
  const char *original_display = getenv("DISPLAY");
  char display[256];
  int failed = 0, index;
  struct file_set before;
  memset(&small, 0, sizeof(small));
  memset(&large, 0, sizeof(large));
  memset(&oversized, 0, sizeof(oversized));
  memset(&jpeg, 0, sizeof(jpeg));
  memset(&result, 0, sizeof(result));
  if (snprintf(base, sizeof(base), "%s/clipboard-XXXXXX", build) >=
          (int)sizeof(base) ||
      !mkdtemp(base))
    return 1;
  snprintf(display, sizeof(display), "%s",
           original_display ? original_display : "");
  snprintf(input, sizeof(input), "%s/small.png", inputs);
  CHECK(image_load(&small, input, "image/png") == 0);
  snprintf(input, sizeof(input), "%s/large.png", inputs);
  CHECK(image_load(&large, input, "image/png") == 0);
  snprintf(input, sizeof(input), "%s/oversized.png", inputs);
  CHECK(image_load(&oversized, input, "image/png") == 0);
  snprintf(input, sizeof(input), "%s/sample.jpeg", inputs);
  CHECK(image_load(&jpeg, input, "image/jpeg") == 0);
  CHECK(large.length > 16u * 1024 * 1024 &&
        oversized.length > 32u * 1024 * 1024);
  CHECK(offer(&small) == 0);
  {
    const struct image *images[3];
    size_t item, byte;
    unsigned long hash;
    char expected[80];
    images[0] = &small;
    images[1] = &jpeg;
    images[2] = &large;
    setenv("SOFTLINE_TEST_IMAGE_TEMPLATE", "/proc/softline-no-staging/*", 1);
    CHECK(inventory(base, &before) == 0);
    for (item = 0; item < 3; ++item) {
      CHECK(offer((struct image *)images[item]) == 0);
      hash = 2166136261UL;
      for (byte = 0; byte < images[item]->length; ++byte)
        hash = ((hash ^ images[item]->bytes[byte]) * 16777619UL) & 0xffffffffUL;
      snprintf(expected, sizeof(expected), "%lu:%lu",
               (unsigned long)images[item]->length, hash);
      CHECK(run_case(fixture, base, "ab\002\026", "adapter-large", &result) ==
            0);
      CHECK(!strcmp(result.line, "a/backend/imageb") &&
            !strcmp(result.error, expected));
      CHECK(unchanged(base, &before));
      if (getenv("SOFTLINE_TEST_LUA")) {
        CHECK(run_case(fixture, base, "ab\002\026", "lua-adapter", &result) ==
              0);
        CHECK(!strcmp(result.line, "a/backend/imageb") &&
              !strcmp(result.error, expected));
        CHECK(unchanged(base, &before));
      }
    }
    CHECK(offer(&large) == 0);
    CHECK(run_case(fixture, base, "draft\026", "adapter", &result) == 0);
    CHECK(!strcmp(result.line, "draft") && strstr(result.error, "byte limit") &&
          unchanged(base, &before));
    unsetenv("SOFTLINE_TEST_IMAGE_TEMPLATE");
    CHECK(offer(&small) == 0);
  }
  setenv("SSH_CONNECTION", "client 123 server 22", 1);
  CHECK(run_case(fixture, base, "ab\002\026\r", NULL, &result) == 0);
  snprintf(directory, sizeof(directory), "%s/softline", base);
  CHECK(image_path(&result, "a", "b", &small, directory, path) == 0 &&
        !*result.error);
  CHECK(access(path, F_OK) == 0);
  unsetenv("SSH_CONNECTION");
  setenv("HOME", base, 1);
  for (index = 0; index < 5; ++index) {
    snprintf(template, sizeof(template), "%s/captures/*", tokens[index]);
    setenv("SOFTLINE_TEST_IMAGE_TEMPLATE", template, 1);
    CHECK(run_case(fixture, base, "\026\r", NULL, &result) == 0);
    snprintf(directory, sizeof(directory), "%s/captures", base);
    CHECK(image_path(&result, "", "", &small, directory, path) == 0 &&
          !*result.error);
  }
  setenv("SOFTLINE_TEST_IMAGE_TEMPLATE", "{{XDG_CACHE_HOME}}/unused/*", 1);
  setenv("SOFTLINE_TEST_IMAGE_SETTER", "{{XDG_CACHE_HOME}}/deep/*/image", 1);
  CHECK(run_case(fixture, base, "\026\r", "setter", &result) == 0);
  strcpy(path, result.line);
  name = strrchr(path, '/');
  CHECK(name && !strcmp(name + 1, "image.png"));
  *name = 0;
  name = strrchr(path, '/');
  CHECK(name && xid(name + 1));
  snprintf(directory, sizeof(directory), "%s/deep", base);
  *name = 0;
  CHECK(!strcmp(path, directory));
  CHECK(bytes_equal(result.line, &small) && !*result.error);
  strcpy(path, result.line);
  *strrchr(path, '/') = 0;
  CHECK(secure_mode(path, 0700));
  setenv("SOFTLINE_TEST_IMAGE_TEMPLATE", "{{XDG_CACHE_HOME}}/deep/*/image", 1);
  CHECK(inventory(base, &before) == 0);
  CHECK(run_case(fixture, base, "draft\026\r", "limit", &result) == 0);
  CHECK(!strcmp(result.line, "draft") && strstr(result.error, "line limit") &&
        unchanged(base, &before));
  setenv("SOFTLINE_TEST_IMAGE_TEMPLATE", "{{XDG_CACHE_HOME}}/extensions/*/.png",
         1);
  CHECK(run_case(fixture, base, "\026\r", NULL, &result) == 0);
  name = strrchr(result.line, '/');
  CHECK(name && !strcmp(name + 1, ".png.png") &&
        bytes_equal(result.line, &small) && !*result.error);
  unsetenv("SOFTLINE_TEST_IMAGE_TEMPLATE");
  CHECK(run_case(fixture, "relative", "\026\r", NULL, &result) == 0);
  snprintf(directory, sizeof(directory), "%s/.cache/softline", base);
  CHECK(image_path(&result, "", "", &small, directory, path) == 0 &&
        !*result.error);
  setenv("SOFTLINE_TEST_IMAGE_TEMPLATE", "{{xdg_cache_home}}/fallback/*", 1);
  CHECK(run_case(fixture, "relative", "\026\r", NULL, &result) == 0);
  snprintf(directory, sizeof(directory), "%s/.cache/fallback", base);
  CHECK(image_path(&result, "", "", &small, directory, path) == 0 &&
        !*result.error);
  unsetenv("SOFTLINE_TEST_IMAGE_TEMPLATE");
  setenv("SOFTLINE_TEST_DISABLE_IMAGE_PASTE", "1", 1);
  CHECK(inventory(base, &before) == 0);
  CHECK(run_case(fixture, base, "draft\026\r", NULL, &result) == 0);
  CHECK(!strcmp(result.line, "draft") && strstr(result.error, "disabled") &&
        unchanged(base, &before));
  unsetenv("SOFTLINE_TEST_DISABLE_IMAGE_PASTE");
  CHECK(run_case(fixture, base, "\026 \026\r", NULL, &result) == 0);
  end = strchr(result.line, ' ');
  CHECK(end);
  *end = 0;
  strcpy(other, end + 1);
  CHECK(strcmp(result.line, other) && bytes_equal(result.line, &small) &&
        bytes_equal(other, &small) && !*result.error);
  CHECK(inventory(base, &before) == 0);
  CHECK(run_case(fixture, base, "pre\033[200~\026\033[201~\r", NULL, &result) ==
        0);
  CHECK(!strcmp(result.line, "pre\026") && !*result.error &&
        unchanged(base, &before));
  CHECK(offer(&jpeg) == 0);
  CHECK(run_case(fixture, base, "\026\r", NULL, &result) == 0);
  snprintf(directory, sizeof(directory), "%s/softline", base);
  CHECK(image_path(&result, "", "", &jpeg, directory, path) == 0 &&
        !*result.error);
  CHECK(strstr(result.line, ".jpeg"));
  CHECK(offer(&large) == 0);
  CHECK(run_case(fixture, base, "\026\r", NULL, &result) == 0);
  CHECK(image_path(&result, "", "", &large, directory, path) == 0 &&
        !*result.error);
  CHECK(offer(&oversized) == 0);
  CHECK(inventory(base, &before) == 0);
  CHECK(run_case(fixture, base, "draft\026\r", NULL, &result) == 0);
  CHECK(!strcmp(result.line, "draft") &&
        strstr(result.error, "exceeded 32 MiB") && unchanged(base, &before));
  invalid.bytes = (unsigned char *)"not actually a PNG";
  invalid.length = strlen((char *)invalid.bytes);
  invalid.mime = "image/png";
  CHECK(offer(&invalid) == 0);
  CHECK(inventory(base, &before) == 0);
  CHECK(run_case(fixture, base, "draft\026\r", NULL, &result) == 0);
  CHECK(!strcmp(result.line, "draft") &&
        strstr(result.error, "transfer failed") && unchanged(base, &before));
  CHECK(offer(&small) == 0);
  CHECK(inventory(base, &before) == 0);
  CHECK(run_case(fixture, base, "draft\026\r", "limit", &result) == 0);
  CHECK(!strcmp(result.line, "draft") && strstr(result.error, "line limit") &&
        unchanged(base, &before));
  gtk.text(clipboard, "plain clipboard text", -1);
  pump();
  CHECK(run_case(fixture, base, "draft\026\r", NULL, &result) == 0);
  CHECK(!strcmp(result.line, "draft") &&
        strstr(result.error, "no PNG or JPEG"));
  setenv("DISPLAY", "invalid:0", 1);
  CHECK(run_case(fixture, base, "\026\r", "override", &result) == 0);
  CHECK(!strcmp(result.line, "OVERRIDE") && !*result.error);
  setenv("DISPLAY", "", 1);
  CHECK(run_case(fixture, base, "draft\026\r", NULL, &result) == 0);
  CHECK(!strcmp(result.line, "draft") &&
        strstr(result.error, "DISPLAY is unavailable"));
cleanup:
  setenv("DISPLAY", display, 1);
  gtk.text(clipboard, "", -1);
  pump();
  free(small.bytes);
  free(large.bytes);
  free(oversized.bytes);
  free(jpeg.bytes);
  remove_owned(base);
  rmdir(base);
  return failed;
}
#include "clipboard_transport.h"

static int forwarded_cases(const char *fixture, const char *build,
                           const char *inputs) {
  struct image image;
  struct result result;
  char base[4096], input[4096], original[256], authority[4096],
      original_authority[4096], display[256], directory[4096], path[4096];
  const char *old_auth = getenv("XAUTHORITY"), *old_display = getenv("DISPLAY"),
             *colon;
  unsigned char cookie[16], decoy[16];
  const int families[] = {256, 0, 6};
  int failed = 0, index, number, listener = -1, status;
  pid_t relay = -1;
  memset(&image, 0, sizeof(image));
  memset(&result, 0, sizeof(result));
  snprintf(original, sizeof(original), "%s", old_display ? old_display : "");
  snprintf(original_authority, sizeof(original_authority), "%s",
           old_auth ? old_auth : "");
  if (snprintf(base, sizeof(base), "%s/clipboard-network-XXXXXX", build) >=
          (int)sizeof(base) ||
      !mkdtemp(base))
    return 1;
  snprintf(input, sizeof(input), "%s/small.png", inputs);
  CHECK(image_load(&image, input, "image/png") == 0);
  CHECK(offer(&image) == 0);
  colon = strrchr(original, ':');
  CHECK(colon);
  CHECK(read_cookie(old_auth, atoi(colon + 1), cookie) == 0);
  for (index = 0; index < 16; ++index)
    decoy[index] = (unsigned char)(cookie[index] ^ 0xff);
  snprintf(authority, sizeof(authority), "%s/forwarded.Xauthority", base);
  snprintf(directory, sizeof(directory), "%s/softline", base);
  for (index = 0; index < 3; ++index) {
    listener = listen_display(families[index] == 6 ? AF_INET6 : AF_INET, 300,
                              400, &number);
    CHECK(listener >= 0);
    CHECK(write_auth(authority, number, families[index], cookie, decoy) == 0);
    snprintf(display, sizeof(display),
             families[index] == 6 ? "[::1]:%d.0" : "localhost:%d.0", number);
    relay = fork();
    CHECK(relay >= 0);
    if (relay == 0)
      _exit(relay_display(listener, original, cookie));
    close(listener);
    listener = -1;
    setenv("DISPLAY", display, 1);
    setenv("XAUTHORITY", authority, 1);
    setenv("SSH_CONNECTION", "client 123 server 22", 1);
    CHECK(run_case(fixture, base, "remote\026\r", NULL, &result) == 0);
    CHECK(image_path(&result, "remote", "", &image, directory, path) == 0 &&
          !*result.error);
    CHECK(waitpid(relay, &status, 0) == relay);
    relay = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
cleanup:
  if (listener >= 0)
    close(listener);
  if (relay > 0) {
    kill(relay, SIGTERM);
    waitpid(relay, &status, 0);
  }
  setenv("DISPLAY", original, 1);
  if (*original_authority)
    setenv("XAUTHORITY", original_authority, 1);
  else
    unsetenv("XAUTHORITY");
  unsetenv("SSH_CONNECTION");
  gtk.text(clipboard, "", -1);
  pump();
  free(image.bytes);
  remove_owned(base);
  rmdir(base);
  return failed;
}
static int auth_owner(const char *input) {
  struct image image;
  struct pollfd fd;
  int failed = 1;
  if (load() || image_load(&image, input, "image/png"))
    return 1;
  if (offer(&image)) {
    free(image.bytes);
    return 1;
  }
  puts("READY");
  fflush(stdout);
  fd.fd = 0;
  fd.events = POLLIN;
  for (;;) {
    char byte;
    pump();
    if (poll(&fd, 1, 10) < 0)
      break;
    if (fd.revents & (POLLIN | POLLHUP)) {
      ssize_t count = read(0, &byte, 1);
      if (count == 0) {
        failed = 0;
        break;
      }
      if (count < 0)
        break;
    }
  }
  gtk.text(clipboard, "", -1);
  pump();
  free(image.bytes);
  return failed;
}
static int auth_server(const char *self, const char *fixture, const char *build,
                       const char *inputs) {
  struct image image;
  struct result result;
  char base[4096], input[4096], server_auth[4096], client_auth[4096],
      wrong_auth[4096], log[4096], display[64], server_display[32],
      directory[4096], path[4096];
  unsigned char cookie[16], wrong[16];
  unsigned char address[4];
  int failed = 0, number, socket_fd = -1, status, index,
      owner_input[2] = {-1, -1}, owner_output[2] = {-1, -1}, log_fd;
  pid_t server = -1, owner = -1;
  FILE *file;
  double deadline;
  struct sockaddr_in network;
  struct pollfd fd;
  const char *binary = getenv("SOFTLINE_TEST_XVFB");
  memset(&image, 0, sizeof(image));
  memset(&result, 0, sizeof(result));
  if (snprintf(base, sizeof(base), "%s/clipboard-auth-XXXXXX", build) >=
          (int)sizeof(base) ||
      !mkdtemp(base))
    return 1;
  snprintf(input, sizeof(input), "%s/small.png", inputs);
  CHECK(image_load(&image, input, "image/png") == 0);
  socket_fd = listen_display(AF_INET, 200, 250, &number);
  CHECK(socket_fd >= 0);
  close(socket_fd);
  socket_fd = -1;
  for (index = 0; index < 16; ++index) {
    cookie[index] = (unsigned char)(17 + index);
    wrong[index] = (unsigned char)(cookie[index] ^ 0xff);
  }
  snprintf(server_auth, sizeof(server_auth), "%s/server.Xauthority", base);
  snprintf(client_auth, sizeof(client_auth), "%s/client.Xauthority", base);
  snprintf(wrong_auth, sizeof(wrong_auth), "%s/wrong.Xauthority", base);
  CHECK(write_auth(server_auth, number, 65535, cookie, NULL) == 0);
  CHECK(write_auth(client_auth, number, 65535, cookie, NULL) == 0);
  CHECK(write_auth(wrong_auth, number, 65535, wrong, NULL) == 0);
  inet_pton(AF_INET, "127.0.0.1", address);
  file = fopen(client_auth, "ab");
  CHECK(file);
  CHECK(auth_record(file, 0, address, 4, number, cookie) == 0);
  CHECK(fclose(file) == 0);
  file = fopen(wrong_auth, "ab");
  CHECK(file);
  CHECK(auth_record(file, 0, address, 4, number, wrong) == 0);
  CHECK(fclose(file) == 0);
  snprintf(log, sizeof(log), "%s/xvfb.log", base);
  log_fd = open(log, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  CHECK(log_fd >= 0);
  snprintf(server_display, sizeof(server_display), ":%d", number);
  snprintf(display, sizeof(display), "127.0.0.1:%d.0", number);
  server = fork();
  if (server == 0) {
    dup2(log_fd, 1);
    dup2(log_fd, 2);
    close(log_fd);
    execlp(binary ? binary : "Xvfb", binary ? binary : "Xvfb", server_display,
           "-nolock", "-nolisten", "unix", "-listen", "tcp", "-auth",
           server_auth, "-screen", "0", "800x600x24", (char *)NULL);
    _exit(127);
  }
  close(log_fd);
  CHECK(server > 0);
  memset(&network, 0, sizeof(network));
  network.sin_family = AF_INET;
  network.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  network.sin_port = htons((unsigned short)(6000 + number));
  deadline = now() + 8;
  do {
    CHECK(waitpid(server, &status, WNOHANG) == 0);
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(socket_fd >= 0);
    if (!connect(socket_fd, (struct sockaddr *)&network, sizeof(network))) {
      close(socket_fd);
      socket_fd = -1;
      break;
    }
    close(socket_fd);
    socket_fd = -1;
    fd.fd = -1;
    poll(&fd, 0, 20);
  } while (now() < deadline);
  CHECK(now() < deadline);
  CHECK(pipe(owner_input) == 0 && pipe(owner_output) == 0);
  owner = fork();
  if (owner == 0) {
    close(owner_input[1]);
    close(owner_output[0]);
    dup2(owner_input[0], 0);
    dup2(owner_output[1], 1);
    close(owner_input[0]);
    close(owner_output[1]);
    setenv("DISPLAY", display, 1);
    setenv("XAUTHORITY", client_auth, 1);
    execl(self, self, "--auth-owner", input, (char *)NULL);
    _exit(127);
  }
  CHECK(owner > 0);
  close(owner_input[0]);
  owner_input[0] = -1;
  close(owner_output[1]);
  owner_output[1] = -1;
  fd.fd = owner_output[0];
  fd.events = POLLIN;
  CHECK(poll(&fd, 1, 5000) > 0);
  CHECK(receive(owner_output[0], (unsigned char *)path, 6) == 0 &&
        !memcmp(path, "READY\n", 6));
  setenv("DISPLAY", display, 1);
  setenv("XAUTHORITY", client_auth, 1);
  CHECK(run_case(fixture, base, "auth\026\r", NULL, &result) == 0);
  snprintf(directory, sizeof(directory), "%s/softline", base);
  CHECK(image_path(&result, "auth", "", &image, directory, path) == 0 &&
        !*result.error);
  setenv("XAUTHORITY", wrong_auth, 1);
  CHECK(run_case(fixture, base, "draft\026\r", NULL, &result) == 0);
  CHECK(!strcmp(result.line, "draft") &&
        strstr(result.error, "check XAUTHORITY"));
cleanup:
  if (socket_fd >= 0)
    close(socket_fd);
  if (owner_input[1] >= 0) {
    close(owner_input[1]);
    owner_input[1] = -1;
  }
  if (owner > 0) {
    kill(owner, SIGTERM);
    waitpid(owner, &status, 0);
  }
  for (index = 0; index < 2; ++index) {
    if (owner_input[index] >= 0)
      close(owner_input[index]);
    if (owner_output[index] >= 0)
      close(owner_output[index]);
  }
  if (server > 0) {
    kill(server, SIGTERM);
    waitpid(server, &status, 0);
  }
  free(image.bytes);
  remove_owned(base);
  rmdir(base);
  return failed;
}
int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "--auth-owner"))
    return auth_owner(argv[2]);
  if ((argc != 4 && argc != 5) || load())
    return 2;
  if (argc == 5 && !strcmp(argv[4], "transport"))
    return forwarded_cases(argv[1], argv[2], argv[3]) ||
           auth_server(argv[0], argv[1], argv[2], argv[3]);
  if (argc != 4)
    return 2;
  return local_cases(argv[1], argv[2], argv[3]);
}
