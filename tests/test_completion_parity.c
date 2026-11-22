#define _DEFAULT_SOURCE
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "parity line %d: %s\n", __LINE__, #c);                   \
      return 1;                                                                \
    }                                                                          \
  } while (0)
struct item {
  char *prefix, *text;
};
struct fixture {
  struct item items[64];
  size_t count;
  sl_completion_provider_t providers[8];
  size_t providers_count;
  int master;
  const char *keys;
};
static int resolve(sl_t *sl, const char *prefix, sl_completion_emit_t emit,
                   void *context, void *data) {
  struct fixture *f = data;
  size_t i;
  (void)sl;
  for (i = 0; i < f->count; ++i) {
    int status;
    if (strcmp(f->items[i].prefix, prefix))
      continue;
    status = emit(f->items[i].text, context);
    if (status != SL_OK)
      return status;
  }
  return SL_OK;
}
static int initial(sl_t *sl, char trigger, sl_completion_emit_t emit,
                   void *context, void *data) {
  char prefix[2];
  prefix[0] = trigger;
  prefix[1] = 0;
  return resolve(sl, prefix, emit, context, data);
}
static int drive(sl_t *sl, const sl_watch_event_t *event, void *data) {
  struct fixture *f = data;
  char byte;
  (void)sl;
  return read(event->fd, &byte, 1) == 1 &&
                 write(f->master, f->keys, strlen(f->keys)) ==
                     (ssize_t)strlen(f->keys)
             ? SL_OK
             : SL_ERROR_IO;
}
static int run(struct fixture *f, const char *script, const char *expected) {
  static const struct {
    const char *tag, *bytes;
  } tags[] = {{"<Enter>", "\r"},       {"<Tab>", "\t"},
              {"<Space>", " "},        {"<Right>", "\033[C"},
              {"<Left>", "\033[D"},    {"<CtrlF>", "\006"},
              {"<Home>", "\001"},      {"<End>", "\005"},
              {"<Backspace>", "\177"}, {"<Delete>", "\033[3~"}};
  char keys[2048], *out = keys, *line;
  int master, slave, wake[2];
  size_t i;
  struct winsize size = {24, 160, 0, 0};
  sl_config_t config;
  sl_t *sl;
  sl_watch_id_t watch;
  while (*script) {
    const char *bytes = NULL;
    for (i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i)
      if (!strncmp(script, tags[i].tag, strlen(tags[i].tag))) {
        bytes = tags[i].bytes;
        script += strlen(tags[i].tag);
        break;
      }
    if (bytes) {
      CHECK((size_t)(out - keys) + strlen(bytes) < sizeof(keys));
      memcpy(out, bytes, strlen(bytes));
      out += strlen(bytes);
    } else {
      CHECK(out + 1 < keys + sizeof(keys));
      *out++ = *script++;
    }
  }
  *out = 0;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(wake));
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  sl = sl_create_with_config(&config);
  CHECK(sl && sl_set_completion_providers(sl, f->providers,
                                          f->providers_count) == SL_OK);
  f->master = master;
  f->keys = keys;
  CHECK(sl_watch_add(sl, wake[0], SL_WATCH_READ, drive, f, &watch) == SL_OK);
  CHECK(write(wake[1], "x", 1) == 1);
  line = sl_readline(sl, "");
  if (!line || strcmp(line, expected)) {
    fprintf(stderr, "native got [%s], expected [%s]\n", line ? line : "NULL",
            expected);
    return 1;
  }
  sl_free_string(sl, line);
  sl_destroy(sl);
  close(master);
  close(slave);
  close(wake[0]);
  close(wake[1]);
  return 0;
}
int main(int argc, char **argv) {
  struct fixture f;
  FILE *file;
  char line[4096];
  size_t i, cases = 0;
  CHECK(argc == 2);
  memset(&f, 0, sizeof(f));
  file = fopen(argv[1], "r");
  CHECK(file);
  while (fgets(line, sizeof(line), file)) {
    char *kind, *a, *b, *c, *d;
    if (line[0] == '#')
      continue;
    line[strcspn(line, "\r\n")] = 0;
    kind = strtok(line, "\t");
    a = strtok(NULL, "\t");
    b = strtok(NULL, "\t");
    c = strtok(NULL, "\t");
    d = strtok(NULL, "\t");
    CHECK(kind && a && b);
    if (!strcmp(kind, "provider")) {
      sl_completion_provider_t *p;
      CHECK(c && d && f.providers_count < 8);
      p = f.providers + f.providers_count++;
      p->trigger = a[0];
      p->scope = !strcmp(b, "word") ? SL_COMPLETION_WORD : SL_COMPLETION_START;
      p->strip_trigger = atoi(c);
      p->callback = initial;
      p->expand = atoi(d) ? resolve : NULL;
      p->userdata = &f;
    } else if (!strcmp(kind, "candidate")) {
      CHECK(f.count < 64);
      f.items[f.count].prefix = strdup(a);
      f.items[f.count].text = strdup(b);
      CHECK(f.items[f.count].prefix && f.items[f.count].text);
      ++f.count;
    } else {
      CHECK(!strcmp(kind, "case") && c);
      if (run(&f, b, c)) {
        fprintf(stderr, "case: %s\n", a);
        return 1;
      }
      ++cases;
    }
  }
  CHECK(!ferror(file) && cases >= 30);
  fclose(file);
  for (i = 0; i < f.count; ++i) {
    free(f.items[i].prefix);
    free(f.items[i].text);
  }
  printf("%lu native/browser completion parity cases passed\n",
         (unsigned long)cases);
  return 0;
}
