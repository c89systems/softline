#include "completion.h"
#include "softline/softline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int set_prompt_theme_from_environment(sl_t *sl,
                                             sl_prompt_theme_t fallback) {
  const char *name;
  sl_prompt_theme_t theme;
  name = getenv("SOFTLINE_PROMPT_THEME");
  theme = fallback;
  if (name && name[0] != '\0') {
    if (strcmp(name, "default") == 0)
      theme = SL_PROMPT_THEME_DEFAULT;
    else if (strcmp(name, "plain") == 0)
      theme = SL_PROMPT_THEME_PLAIN;
    else if (strcmp(name, "accent") == 0)
      theme = SL_PROMPT_THEME_ACCENT;
    else if (strcmp(name, "dracula") == 0)
      theme = SL_PROMPT_THEME_DRACULA;
    else if (strcmp(name, "gruvbox") == 0)
      theme = SL_PROMPT_THEME_GRUVBOX;
    else if (strcmp(name, "monochrome") == 0)
      theme = SL_PROMPT_THEME_MONOCHROME;
    else if (strcmp(name, "monogreen") == 0)
      theme = SL_PROMPT_THEME_MONOGREEN;
    else if (strcmp(name, "outrun") == 0)
      theme = SL_PROMPT_THEME_OUTRUN;
    else if (strcmp(name, "riced") == 0)
      theme = SL_PROMPT_THEME_RICED;
    else if (strcmp(name, "synthwave") == 0)
      theme = SL_PROMPT_THEME_SYNTHWAVE;
    else {
      fprintf(stderr,
              "invalid SOFTLINE_PROMPT_THEME: %s (use default, plain, accent, "
              "dracula, gruvbox, monochrome, monogreen, outrun, riced, or "
              "synthwave)\n",
              name);
      return -1;
    }
  }
  return sl_set_prompt_theme(sl, theme) == SL_OK ? 0 : -1;
}

int main(void) {
  sl_t *sl;
  char *line;

  sl = sl_create();
  if (!sl) {
    fprintf(stderr, "failed to create softline\n");
    return 1;
  }
  if (set_prompt_theme_from_environment(sl, SL_PROMPT_THEME_DEFAULT) != 0) {
    fprintf(stderr, "failed to set prompt theme\n");
    sl->destroy(sl);
    return 1;
  }
  if (demo_completion_setup(sl, 0) != SL_OK) {
    fprintf(stderr, "failed to set completions\n");
    sl->destroy(sl);
    return 1;
  }
  if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
    puts("/ resolves commands; @ lists one directory at a time (except .git). "
         "Tab cycles; Right/Ctrl-F commits and previews the next list. "
         "Enter or a separating space removes @.");
    puts("Completion demo: type h for a dim suggestion; Tab accepts, more Tabs "
         "cycle. Backspace restarts matching.");
    puts("  Try say ver + Tab on any line; Ctrl-J adds a line; /q + Tab "
         "completes /quit.");
  }

  for (;;) {
    line = sl->readline(sl, "softline> ");
    if (!line)
      break;
    if (strcmp(line, "/quit") == 0) {
      sl->free_string(sl, line);
      break;
    }
    printf("submitted: %s\n", line);
    sl->free_string(sl, line);
  }

  sl->destroy(sl);
  return 0;
}
