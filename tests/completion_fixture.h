#ifndef SOFTLINE_COMPLETION_FIXTURE_H
#define SOFTLINE_COMPLETION_FIXTURE_H
#include <softline/softline.h>
#include <string.h>
struct completion_item {
  const char *text;
  sl_completion_scope_t scope;
};
struct completion_fixture {
  const struct completion_item *items;
  size_t count;
  int calls;
};
static int fixture_complete(sl_t *sl, char trigger, sl_completion_emit_t emit,
                            void *context, void *userdata) {
  struct completion_fixture *fixture = userdata;
  size_t i;
  (void)sl;
  ++fixture->calls;
  for (i = 0; i < fixture->count; ++i)
    if (fixture->items[i].text[0] == trigger) {
      int status = emit(fixture->items[i].text, context);
      if (status != SL_OK)
        return status;
    }
  return SL_OK;
}
static int fixture_setup(sl_t *sl, struct completion_fixture *fixture) {
  sl_completion_provider_t providers[94];
  size_t i, j, count = 0;
  int status;
  memset(providers, 0, sizeof(providers));
  for (i = 0; i < fixture->count; ++i) {
    for (j = 0; j < count; ++j)
      if (providers[j].trigger == fixture->items[i].text[0])
        break;
    if (j == count) {
      if (count == sizeof(providers) / sizeof(providers[0]))
        return SL_ERROR_INVALID;
      providers[count].trigger = fixture->items[i].text[0];
      providers[count].scope = fixture->items[i].scope;
      providers[count].callback = fixture_complete;
      providers[count++].userdata = fixture;
    } else if (fixture->items[i].scope == SL_COMPLETION_WORD)
      providers[j].scope = SL_COMPLETION_WORD;
  }
  status = sl_set_completion_providers(sl, providers, count);
  memset(providers, 0, sizeof(providers)); /* Registrations must be copied. */
  return status;
}
#endif
