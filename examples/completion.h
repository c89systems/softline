#ifndef SOFTLINE_DEMO_COMPLETION_H
#define SOFTLINE_DEMO_COMPLETION_H
#include "completion_resolve.h"
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
