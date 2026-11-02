#ifndef SOFTLINE_DEMO_COMPLETION_H
#define SOFTLINE_DEMO_COMPLETION_H
#include <dirent.h>
#include <softline/softline.h>
#include <stdlib.h>
#include <string.h>

static int demo_complete(sl_t *sl, char trigger, sl_completion_emit_t emit,
                         void *context, void *userdata) {
  static const char *const simple[] = {"/quit", "hello world", "hello there",
                                       "verification", "next steps"};
  static const char *const chat[] = {"hello world",  "hello there",  "/status",
                                     "/approve on",  "/approve off", "/steer",
                                     "/resume",      "/shell",       "/quit",
                                     "verification", "next steps"};
  const char *const *items = userdata ? chat : simple;
  size_t i, count = userdata ? sizeof(chat) / sizeof(chat[0])
                             : sizeof(simple) / sizeof(simple[0]);
  (void)sl;
  if (trigger == '@') {
    DIR *directory = opendir(".");
    struct dirent *entry;
    int status = SL_OK;
    if (!directory)
      return SL_ERROR_IO;
    while ((entry = readdir(directory)) != NULL) {
      char *candidate;
      size_t length;
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;
      length = strlen(entry->d_name);
      candidate = malloc(length + 2);
      if (!candidate) {
        status = SL_ERROR_NOMEM;
        break;
      }
      candidate[0] = '@';
      memcpy(candidate + 1, entry->d_name, length + 1);
      status = emit(candidate, context);
      free(candidate);
      if (status != SL_OK)
        break;
    }
    closedir(directory);
    return status;
  }
  for (i = 0; i < count; ++i)
    if (items[i][0] == trigger) {
      int status = emit(items[i], context);
      if (status != SL_OK)
        return status;
    }
  return SL_OK;
}

static int demo_completion_setup(sl_t *sl, int chat) {
  sl_completion_provider_t providers[] = {
      {'/', SL_COMPLETION_START, 0, demo_complete, NULL},
      {'h', SL_COMPLETION_START, 0, demo_complete, NULL},
      {'v', SL_COMPLETION_WORD, 0, demo_complete, NULL},
      {'n', SL_COMPLETION_WORD, 0, demo_complete, NULL},
      {'@', SL_COMPLETION_WORD, 1, demo_complete, NULL}};
  size_t i;
  for (i = 0; i < sizeof(providers) / sizeof(providers[0]); ++i)
    providers[i].userdata = chat ? sl : NULL;
  return sl_set_completion_providers(sl, providers,
                                     sizeof(providers) / sizeof(providers[0]));
}
#endif
