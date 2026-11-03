#ifndef SOFTLINE_DEMO_COMPLETION_H
#define SOFTLINE_DEMO_COMPLETION_H
#include <dirent.h>
#include <softline/softline.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Only this directory is read. Descendants are resolved by later Tabs. */
static int demo_complete_files(const char *prefix, sl_completion_emit_t emit,
                               void *context) {
  size_t prefix_length = strlen(prefix);
  DIR *directory;
  struct dirent *entry;
  int status = SL_OK;
  if (prefix_length > 1 && prefix[prefix_length - 1] != '/')
    return SL_OK;
  directory = opendir(prefix_length > 1 ? prefix + 1 : ".");
  if (!directory)
    return SL_ERROR_IO;
  while ((entry = readdir(directory)) != NULL) {
    struct stat info;
    char *candidate;
    size_t length = strlen(entry->d_name);
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        !strcmp(entry->d_name, ".git"))
      continue;
    if (prefix_length > (size_t)-1 - length - 2) {
      status = SL_ERROR_NOMEM;
      break;
    }
    candidate = malloc(prefix_length + length + 2);
    if (!candidate) {
      status = SL_ERROR_NOMEM;
      break;
    }
    memcpy(candidate, prefix, prefix_length);
    memcpy(candidate + prefix_length, entry->d_name, length + 1);
    if (lstat(candidate + 1, &info) != 0) {
      free(candidate);
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      length += prefix_length;
      candidate[length++] = '/';
      candidate[length] = '\0';
    }
    status = emit(candidate, context);
    free(candidate);
    if (status != SL_OK)
      break;
  }
  closedir(directory);
  return status;
}

/* Both initial and continuation callbacks use the same host resolver. Emit
 * full strings including the trigger; matching spaces continue phrase ghosts.
 * Directory children are resolved on demand before subsequent Tab rotation. */
static int demo_resolve(sl_t *sl, const char *prefix, sl_completion_emit_t emit,
                        void *context, void *userdata) {
  static const char *const simple[] = {"/quit", "hello world", "hello there",
                                       "verification", "next steps"};
  static const char *const chat[] = {
      "hello world", "hello there", "/status", "/approve",     "/steer",
      "/resume",     "/shell",      "/quit",   "verification", "next steps"};
  const char *const *items = userdata ? chat : simple;
  size_t i, count = userdata ? sizeof(chat) / sizeof(chat[0])
                             : sizeof(simple) / sizeof(simple[0]);
  (void)sl;
  if (prefix[0] == '@')
    return demo_complete_files(prefix, emit, context);
  if (prefix[1]) {
    int status;
    if (!userdata || strcmp(prefix, "/approve"))
      return SL_OK;
    status = emit("/approve on", context);
    return status == SL_OK ? emit("/approve off", context) : status;
  }
  for (i = 0; i < count; ++i)
    if (items[i][0] == prefix[0]) {
      int status = emit(items[i], context);
      if (status != SL_OK)
        return status;
    }
  return SL_OK;
}

static int demo_complete(sl_t *sl, char trigger, sl_completion_emit_t emit,
                         void *context, void *userdata) {
  char prefix[2];
  prefix[0] = trigger;
  prefix[1] = '\0';
  return demo_resolve(sl, prefix, emit, context, userdata);
}

static int demo_completion_setup(sl_t *sl, int chat) {
  sl_completion_provider_t providers[] = {
      {'/', SL_COMPLETION_START, 0, demo_complete, NULL, demo_resolve},
      {'h', SL_COMPLETION_START, 0, demo_complete, NULL, NULL},
      {'v', SL_COMPLETION_WORD, 0, demo_complete, NULL, NULL},
      {'n', SL_COMPLETION_WORD, 0, demo_complete, NULL, NULL},
      {'@', SL_COMPLETION_WORD, 1, demo_complete, NULL, demo_resolve}};
  size_t i;
  for (i = 0; i < sizeof(providers) / sizeof(providers[0]); ++i)
    providers[i].userdata = chat ? sl : NULL;
  return sl_set_completion_providers(sl, providers,
                                     sizeof(providers) / sizeof(providers[0]));
}
#endif
