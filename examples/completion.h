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
  sl_completion_provider_t providers[sizeof(demo_completion_rules) /
                                     sizeof(demo_completion_rules[0])];
  size_t i;
  for (i = 0; i < sizeof(providers) / sizeof(providers[0]); ++i) {
    providers[i].trigger = demo_completion_rules[i].trigger;
    providers[i].scope = demo_completion_rules[i].word ? SL_COMPLETION_WORD
                                                       : SL_COMPLETION_START;
    providers[i].strip_trigger = demo_completion_rules[i].strip;
    providers[i].callback = demo_complete;
    providers[i].expand = demo_completion_rules[i].expand ? demo_resolve : NULL;
    providers[i].userdata = chat ? sl : NULL;
  }
  return sl_set_completion_providers(sl, providers,
                                     sizeof(providers) / sizeof(providers[0]));
}
#endif
