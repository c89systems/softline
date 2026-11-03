#include "../examples/completion.h"
#include <stdio.h>

static int emit_file(const char *text, void *context) {
  (void)context;
  return fwrite(text, 1, strlen(text) + 1, stdout) == strlen(text) + 1
             ? SL_OK
             : SL_ERROR_IO;
}

static int stop_early(const char *text, void *context) {
  int *remaining = context;
  (void)text;
  return --*remaining ? SL_OK : SL_ERROR_IO;
}

int main(int argc, char **argv) {
  sl_t *sl = sl_create();
  int status;
  int remaining = 3;
  if (!sl)
    return 1;
  status = demo_completion_setup(sl, 0);
  if (status == SL_OK && argc == 2 && !strcmp(argv[1], "@") &&
      (demo_complete(sl, '@', stop_early, &remaining, NULL) != SL_ERROR_IO ||
       remaining != 0)) {
    fprintf(stderr, "file discovery did not stop on emitter failure\n");
    status = SL_ERROR;
  }
  if (status == SL_OK)
    status = demo_resolve(sl, argc == 2 ? argv[1] : "@", emit_file, NULL, NULL);
  sl_destroy(sl);
  return status == SL_OK ? 0 : 1;
}
