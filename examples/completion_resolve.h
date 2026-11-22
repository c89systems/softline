#ifndef SOFTLINE_DEMO_COMPLETION_RESOLVE_H
#define SOFTLINE_DEMO_COMPLETION_RESOLVE_H
#include <dirent.h>
#include <softline/softline.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
typedef struct demo_completion_rule {
  char trigger;
  int word, strip, expand;
} demo_completion_rule_t;
static const demo_completion_rule_t demo_completion_rules[] = {{'/', 0, 0, 1},
                                                               {'h', 0, 0, 0},
                                                               {'v', 1, 0, 0},
                                                               {'n', 1, 0, 0},
                                                               {'@', 1, 1, 1}};
static const demo_completion_rule_t *demo_completion_rule(char trigger) {
  size_t i;
  for (i = 0;
       i < sizeof(demo_completion_rules) / sizeof(demo_completion_rules[0]);
       ++i)
    if (demo_completion_rules[i].trigger == trigger)
      return demo_completion_rules + i;
  return NULL;
}

/* Only this directory is read. Commit a selection before Tab resolves children.
 */
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
 * Directory children are resolved on demand after committing the selection. */
static int demo_resolve(sl_t *sl, const char *prefix, sl_completion_emit_t emit,
                        void *context, void *userdata) {
  static const char *const simple[] = {"/quit", "hello world", "hello there",
                                       "verification", "next steps"};
  static const char *const chat[] = {
      "hello world",    "hello there", "/status",      "/approve",
      "/steer",         "/resume",     "/shell",       "/quit",
      "/terminal-lock", "/stream",     "verification", "next steps"};
  const char *const *items = userdata ? chat : simple;
  size_t i, count = userdata ? sizeof(chat) / sizeof(chat[0])
                             : sizeof(simple) / sizeof(simple[0]);
  (void)sl;
  if (!demo_completion_rule(prefix[0]))
    return SL_OK;
  if (prefix[0] == '@')
    return demo_complete_files(prefix, emit, context);
  if (prefix[1]) {
    int status;
    if (!userdata)
      return SL_OK;
    if (!strcmp(prefix, "/approve")) {
      status = emit("/approve on", context);
      return status == SL_OK ? emit("/approve off", context) : status;
    }
    if (!strcmp(prefix, "/terminal-lock")) {
      status = emit("/terminal-lock on", context);
      return status == SL_OK ? emit("/terminal-lock off", context) : status;
    }
    if (!strcmp(prefix, "/stream")) {
      status = emit("/stream direct", context);
      return status == SL_OK ? emit("/stream llm", context) : status;
    }
    return SL_OK;
  }
  for (i = 0; i < count; ++i)
    if (items[i][0] == prefix[0]) {
      int status = emit(items[i], context);
      if (status != SL_OK)
        return status;
    }
  if (prefix[0] == '/' && sl && userdata == sl) {
    sl_queue_backend_info_t backend;
    int status = emit("/demo", context);
    if (status != SL_OK)
      return status;
    if (sl_queue_backend_info(sl, &backend) == SL_OK && backend.enabled)
      return emit("/type", context);
  }
  return SL_OK;
}

#endif
