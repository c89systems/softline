#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "softline/softline.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "history:%d: %s\n", __LINE__, #x);                       \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static const char hash[] =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
static char root[2048], directory[4096], path[8192], snapshot[4096];
static int append_calls;
static int fail_append, fail_load, ignore_emit_error;
static sl_t *callback_receiver;

static char *read_file(const char *name) {
  FILE *fp;
  long len;
  char *text;
  fp = fopen(name, "rb");
  CHECK(fp != NULL && fseek(fp, 0, SEEK_END) == 0);
  len = ftell(fp);
  CHECK(len >= 0 && fseek(fp, 0, SEEK_SET) == 0);
  text = (char *)malloc((size_t)len + 1);
  CHECK(text != NULL && fread(text, 1, (size_t)len, fp) == (size_t)len);
  text[len] = '\0';
  CHECK(fclose(fp) == 0);
  return text;
}

static void expect_file(const char *name, const char *expected) {
  char *text = read_file(name);
  CHECK(strcmp(text, expected) == 0);
  free(text);
}

static int append_hook(const char *key, const char *prompt, void *userdata) {
  CHECK(strcmp(key, "custom") == 0 && userdata == &append_calls);
  CHECK(prompt && *prompt);
  if (callback_receiver) {
    CHECK(sl_history_add(callback_receiver, "reentrant") == SL_ERROR_INVALID);
    CHECK(sl_history_close(callback_receiver) == SL_ERROR_INVALID);
    CHECK(sl_history_set_max_len(callback_receiver, 3) == SL_ERROR_INVALID);
    sl_destroy(
        callback_receiver); /* Must refuse destruction inside the hook. */
  }
  append_calls++;
  return fail_append ? SL_ERROR_IO : SL_OK;
}

static int load_hook(const char *key, sl_history_emit_t emit, void *context,
                     void *userdata) {
  CHECK(strcmp(key, "custom") == 0 && userdata == &append_calls);
  if (ignore_emit_error) {
    CHECK(emit(context, NULL) == SL_ERROR_INVALID);
    return SL_OK;
  }
  CHECK(emit(context, "older\nmultiline") == SL_OK);
  CHECK(emit(context, "latest\r\t\\n\303\245") == SL_OK);
  return fail_load ? SL_ERROR_IO : SL_OK;
}

static void custom_hooks(void) {
  sl_t *sl = sl_create();
  char key[] = "custom";
  CHECK(sl != NULL);
  CHECK(sl_history_add(sl, "before") == SL_OK);
  CHECK(sl_history_set_backend(sl, key, load_hook, append_hook,
                               &append_calls) == SL_OK);
  key[0] = 'X';
  CHECK(append_calls == 0);
  CHECK(sl_history_save(sl, snapshot) == SL_OK);
  expect_file(snapshot,
              "before\nolder\\nmultiline\nlatest\\r\\t\\\\n\303\245\n");
  callback_receiver = sl;
  CHECK(sl_history_add(sl, "accepted") == SL_OK && append_calls == 1);
  CHECK(sl_history_add(sl, "accepted") == SL_OK && append_calls == 1);
  CHECK(sl_history_add(sl, "") == SL_OK && append_calls == 1);
  fail_append = 1;
  CHECK(sl_history_add(sl, "failed") == SL_ERROR_IO);
  fail_append = 0;
  CHECK(sl_history_save(sl, snapshot) == SL_OK);
  expect_file(
      snapshot,
      "before\nolder\\nmultiline\nlatest\\r\\t\\\\n\303\245\naccepted\n");
  fail_load = 1;
  CHECK(sl_history_set_backend(sl, "custom", load_hook, append_hook,
                               &append_calls) == SL_ERROR_IO);
  fail_load = 0;
  ignore_emit_error = 1;
  CHECK(sl_history_set_backend(sl, "custom", load_hook, append_hook,
                               &append_calls) == SL_ERROR_INVALID);
  ignore_emit_error = 0;
  CHECK(sl_history_save(sl, snapshot) == SL_OK);
  expect_file(
      snapshot,
      "before\nolder\\nmultiline\nlatest\\r\\t\\\\n\303\245\naccepted\n");
  CHECK(sl_history_load(sl, snapshot) == SL_OK && append_calls == 2);
  CHECK(sl_history_add(sl, "after failed attach") == SL_OK &&
        append_calls == 3);
  CHECK(sl_history_compact(sl) == SL_ERROR_INVALID);
  CHECK(sl_history_close(sl) == SL_OK);
  CHECK(sl_history_close(sl) == SL_OK);
  CHECK(sl_history_add(sl, "detached") == SL_OK && append_calls == 3);
  CHECK(sl_history_set_backend(sl, "custom", NULL, append_hook,
                               &append_calls) == SL_OK);
  CHECK(sl_history_set_max_len(sl, 0) == SL_OK);
  CHECK(sl_history_add(sl, "disabled") == SL_OK && append_calls == 3);
  callback_receiver = NULL;
  sl_destroy(sl);
  puts("Custom hooks: ordering, import bypass, rollback, deduplication, "
       "reentry and detach passed.");
}

static void native_store(void) {
  sl_t *sl, *reader;
  FILE *fp;
  struct stat st;
  char *before;
  struct rlimit original, limited;
  sl = sl_create();
  CHECK(sl != NULL);
  CHECK(sl_history_open(sl, "abc", directory) == SL_OK);
  CHECK(sl_history_add(sl, "first\nsecond\r\t\\n \342\230\203") == SL_OK);
  expect_file(path, "first\\nsecond\\r\\t\\\\n \342\230\203\n");
  CHECK(stat(directory, &st) == 0 && (st.st_mode & 0777) == 0700);
  CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
  reader = sl_create();
  CHECK(reader && sl_history_open(reader, "abc", directory) == SL_OK);
  CHECK(sl_history_save(reader, snapshot) == SL_OK);
  expect_file(snapshot, "first\\nsecond\\r\\t\\\\n \342\230\203\n");
  CHECK(sl_history_add(reader, "concurrent handle") == SL_OK);
  /* Repair a torn, large final record, with no full-file buffering. */
  fp = fopen(path, "ab");
  CHECK(fp != NULL);
  {
    int i;
    for (i = 0; i < 9000; i++)
      CHECK(fputc('x', fp) != EOF);
  }
  CHECK(fclose(fp) == 0);
  CHECK(sl_history_open(reader, "abc", directory) == SL_OK);
  CHECK(sl_history_add(sl, "after crash") == SL_OK);
  expect_file(path, "first\\nsecond\\r\\t\\\\n \342\230\203\nconcurrent "
                    "handle\nafter crash\n");
  CHECK(sl_history_set_max_len(sl, 2) == SL_OK);
  CHECK(sl_history_compact(sl) == SL_OK);
  expect_file(path, "concurrent handle\nafter crash\n");
  CHECK(sl_history_add(reader, "after compact") == SL_OK);
  expect_file(path, "concurrent handle\nafter crash\nafter compact\n");
  before = read_file(path);
  CHECK(sl_history_open(sl, "", directory) == SL_ERROR_INVALID);
  CHECK(sl_history_open(sl, "abc", "relative") == SL_ERROR_INVALID);
  CHECK(sl_history_add(sl, "still attached") == SL_OK);
  free(before);
  /* Force a short write after part of the new record; rollback must preserve
   * committed bytes and memory, with no fclose re-flush of a failed record. */
  CHECK(stat(path, &st) == 0);
  before = read_file(path);
  CHECK(getrlimit(RLIMIT_FSIZE, &original) == 0);
  limited = original;
  limited.rlim_cur = (rlim_t)st.st_size + 3;
  CHECK(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
  CHECK(setrlimit(RLIMIT_FSIZE, &limited) == 0);
  CHECK(sl_history_add(sl, "write must fail") == SL_ERROR_IO);
  CHECK(setrlimit(RLIMIT_FSIZE, &original) == 0);
  CHECK(stat(path, &st) == 0 && st.st_size == (off_t)(limited.rlim_cur - 3));
  expect_file(path, before);
  free(before);
  CHECK(sl_history_save(sl, snapshot) == SL_OK);
  before = read_file(snapshot);
  CHECK(strstr(before, "write must fail") == NULL);
  free(before);
  CHECK(sl_history_add(sl, "write must fail") == SL_OK);
  before = read_file(path);
  CHECK(strstr(before, "write must fail\n") != NULL);
  free(before);
  sl_destroy(reader);
  CHECK(sl_history_set_max_len(sl, 0) == SL_OK &&
        sl_history_compact(sl) == SL_OK);
  expect_file(path, "");
  sl_destroy(sl);
  puts("Native store: immediate multiline append, permissions, recovery, "
       "compaction and failed-write retry passed.");
}

static void concurrent_writers(void) {
  pid_t children[3];
  sl_t *sl;
  int i, j, status, count;
  FILE *fp;
  char line[80];
  CHECK(unlink(path) == 0);
  sl = sl_create();
  CHECK(sl && sl_history_set_max_len(sl, 1000) == SL_OK);
  CHECK(sl_history_open(sl, "abc", directory) == SL_OK);
  for (i = 0; i < 3; i++) {
    children[i] = fork();
    CHECK(children[i] >= 0);
    if (!children[i]) {
      sl_t *writer = sl_create();
      CHECK(writer && sl_history_open(writer, "abc", directory) == SL_OK);
      for (j = 0; j < 80; j++) {
        sprintf(line, "writer-%d-%d", i, j);
        CHECK(sl_history_add(writer, line) == SL_OK);
      }
      /* Intentionally no destroy/save: persistence must already be complete. */
      _exit(0);
    }
  }
  for (i = 0; i < 8; i++)
    CHECK(sl_history_compact(sl) == SL_OK);
  for (i = 0; i < 3; i++) {
    CHECK(waitpid(children[i], &status, 0) == children[i]);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  fp = fopen(path, "r");
  CHECK(fp != NULL);
  count = 0;
  {
    int seen[3][80];
    memset(seen, 0, sizeof(seen));
    while (fgets(line, sizeof(line), fp)) {
      CHECK(sscanf(line, "writer-%d-%d", &i, &j) == 2 && i >= 0 && i < 3 &&
            j >= 0 && j < 80);
      CHECK(strchr(line, '\n') != NULL && !seen[i][j]);
      seen[i][j] = 1;
      count++;
    }
  }
  CHECK(count == 240 && fclose(fp) == 0);
  sl_destroy(sl);
  puts(
      "Concurrent appenders and compaction preserve all 240 distinct records.");
}

static void *thread_writer(void *userdata) {
  sl_t *writer;
  int i, id;
  char line[80];
  id = *(int *)userdata;
  writer = sl_create();
  CHECK(writer && sl_history_open(writer, "abc", directory) == SL_OK);
  for (i = 0; i < 100; i++) {
    sprintf(line, "thread-%d-%d", id, i);
    CHECK(sl_history_add(writer, line) == SL_OK);
  }
  sl_destroy(writer);
  return NULL;
}

static void same_process_writers(void) {
  pthread_t threads[4];
  int ids[4], seen[4][100], i, j, count;
  sl_t *compactor;
  FILE *fp;
  char line[80];
  CHECK(unlink(path) == 0);
  memset(seen, 0, sizeof(seen));
  compactor = sl_create();
  CHECK(compactor && sl_history_set_max_len(compactor, 1000) == SL_OK);
  CHECK(sl_history_open(compactor, "abc", directory) == SL_OK);
  for (i = 0; i < 4; i++) {
    ids[i] = i;
    CHECK(pthread_create(&threads[i], NULL, thread_writer, &ids[i]) == 0);
  }
  for (i = 0; i < 12; i++)
    CHECK(sl_history_compact(compactor) == SL_OK);
  for (i = 0; i < 4; i++)
    CHECK(pthread_join(threads[i], NULL) == 0);
  fp = fopen(path, "r");
  CHECK(fp != NULL);
  count = 0;
  while (fgets(line, sizeof(line), fp)) {
    CHECK(sscanf(line, "thread-%d-%d", &i, &j) == 2 && i >= 0 && i < 4 &&
          j >= 0 && j < 100);
    CHECK(strchr(line, '\n') != NULL && !seen[i][j]);
    seen[i][j] = 1;
    count++;
  }
  CHECK(count == 400 && fclose(fp) == 0);
  sl_destroy(compactor);
  puts("Independent owner-thread handles plus compaction preserve all 400 "
       "records.");
}

static void xdg_and_safety(void) {
  sl_t *sl = sl_create();
  char file[8192], other[16384], longkey[66];
  FILE *fp;
  mode_t mask;
  sl_config_t cfg;
  CHECK(sl != NULL);
  CHECK(setenv("XDG_STATE_HOME", root, 1) == 0);
  CHECK(sl_history_open(sl, "abc", NULL) == SL_OK);
  CHECK(sl_history_add(sl, "xdg") == SL_OK);
  sprintf(file, "%s/softline/history/%s.history", root, hash);
  expect_file(file, "xdg\n");
  CHECK(setenv("HOME", root, 1) == 0 &&
        setenv("XDG_STATE_HOME", "relative", 1) == 0);
  CHECK(sl_history_open(sl, "abc", NULL) == SL_OK);
  CHECK(sl_history_add(sl, "home") == SL_OK);
  sprintf(file, "%s/.local/state/softline/history/%s.history", root, hash);
  expect_file(file, "home\n");
  CHECK(setenv("XDG_STATE_HOME", "", 1) == 0);
  CHECK(sl_history_open(sl, "abc", NULL) == SL_OK);
  CHECK(sl_history_open(sl, "abc", directory) == SL_OK);
  CHECK(sl_history_open(sl, "separate key", directory) == SL_OK);
  CHECK(sl_history_add(sl, "isolated") == SL_OK);
  /* SHA-256 padding across both the 56-byte and full-block boundaries. */
  memset(longkey, 'a', 65);
  longkey[65] = '\0';
  CHECK(sl_history_open(sl, longkey, directory) == SL_OK);
  CHECK(sl_history_add(sl, "hash") == SL_OK);
  sprintf(file,
          "%s/"
          "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0."
          "history",
          directory);
  expect_file(file, "hash\n");
  CHECK(sl_history_open(
            sl, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
            directory) == SL_OK);
  CHECK(sl_history_add(sl, "padding") == SL_OK);
  sprintf(file,
          "%s/"
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1."
          "history",
          directory);
  expect_file(file, "padding\n");
  /* Exact private modes must not depend on a restrictive caller umask. */
  sprintf(file, "%s/restrictive/nested", root);
  mask = umask(0777);
  CHECK(sl_history_open(sl, "abc", file) == SL_OK);
  CHECK(sl_history_add(sl, "private") == SL_OK);
  CHECK(sl_history_compact(sl) == SL_OK);
  umask(mask);
  sprintf(other, "%s/%s.history", file, hash);
  expect_file(other, "private\n");
  /* Reject symlink/FIFO files without blocking or changing their target. */
  sl_history_close(sl);
  CHECK(unlink(path) == 0);
  sprintf(other, "%s/target", root);
  fp = fopen(other, "w");
  CHECK(fp && fputs("untouched\n", fp) >= 0 && fclose(fp) == 0);
  CHECK(symlink(other, path) == 0 &&
        sl_history_open(sl, "abc", directory) == SL_ERROR_IO);
  expect_file(other, "untouched\n");
  CHECK(unlink(path) == 0 && mkfifo(path, 0600) == 0);
  CHECK(sl_history_open(sl, "abc", directory) == SL_ERROR_IO);
  CHECK(unlink(path) == 0);
  fp = fopen(path, "w");
  CHECK(fp && fputs("too long\nok\n", fp) >= 0 && fclose(fp) == 0);
  sl_config_init(&cfg);
  cfg.line_max_len = 2;
  {
    sl_t *small = sl_create_with_config(&cfg);
    CHECK(small &&
          sl_history_open(small, "abc", directory) == SL_ERROR_INVALID);
    sl_destroy(small);
  }
  sl_destroy(sl);
  puts("XDG fallback, exact key hash, isolation, malicious file types and "
       "bounded loading passed.");
}

static void plain_auto_add(void) {
  sl_t *sl;
  sl_config_t config;
  int input[2], output[2];
  char *line;
  CHECK(pipe(input) == 0 && pipe(output) == 0);
  CHECK(write(input[1], "manual\nauto\nauto\n", 17) == 17);
  close(input[1]);
  sl_config_init(&config);
  config.input_fd = input[0];
  config.output_fd = output[1];
  sl = sl_create_with_config(&config);
  CHECK(sl);
  line = sl_readline(sl, NULL);
  CHECK(line && strcmp(line, "manual") == 0);
  sl_free_string(sl, line);
  CHECK(sl_history_save(sl, snapshot) == SL_OK);
  expect_file(snapshot, "");
  CHECK(sl_history_set_auto_add(sl, 1) == SL_OK);
  line = sl_readline(sl, NULL);
  CHECK(line && strcmp(line, "auto") == 0);
  sl_free_string(sl, line);
  line = sl_readline(sl, NULL);
  CHECK(line && strcmp(line, "auto") == 0);
  sl_free_string(sl, line);
  CHECK(sl_history_save(sl, snapshot) == SL_OK);
  expect_file(snapshot, "auto\n");
  sl_destroy(sl);
  close(input[0]);
  close(output[0]);
  close(output[1]);
  puts("Plain reader opt-in auto-add preserves manual defaults and suppresses "
       "consecutive duplicates.");
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  CHECK(sl_history_open(NULL, "abc", NULL) == SL_ERROR_INVALID);
  CHECK(sl_history_close(NULL) == SL_ERROR_INVALID);
  CHECK(sl_history_compact(NULL) == SL_ERROR_INVALID);
  CHECK(sl_history_set_backend(NULL, "abc", NULL, NULL, NULL) ==
        SL_ERROR_INVALID);
  CHECK(sl_history_set_auto_add(NULL, 1) == SL_ERROR_INVALID);
  CHECK(strlen(argv[1]) < sizeof(root) - 32);
  sprintf(root, "%s/history.XXXXXX", argv[1]);
  CHECK(mkdtemp(root) != NULL);
  sprintf(directory, "%s/nested/store", root);
  sprintf(path, "%s/%s.history", directory, hash);
  sprintf(snapshot, "%s/snapshot", root);
  custom_hooks();
  native_store();
  concurrent_writers();
  same_process_writers();
  xdg_and_safety();
  plain_auto_add();
  puts("History storage tests passed.");
  return 0;
}
