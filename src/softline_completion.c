#include "softline_completion.h"
#include <stdlib.h>
#include <string.h>

int sl_completion_space(unsigned char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
         ch == '\f';
}
void sl_completion_cycle_clear(sl_completion_cycle_t *cycle) {
  free(cycle->original);
  free(cycle->items);
  memset(cycle, 0, sizeof(*cycle));
}
void sl_completion_clear(sl_completion_list_t *list) {
  size_t i;
  for (i = 0; i < list->count; ++i)
    free(list->items[i].text);
  free(list->items);
  memset(list, 0, sizeof(*list));
}
void sl_completion_reset(sl_completion_state_t *state) {
  sl_completion_clear(&state->list);
  sl_completion_cycle_clear(&state->cycle);
  state->active = state->failed = 0;
  state->start = state->end = 0;
}
int sl_completion_add(sl_completion_state_t *state, const char *text) {
  sl_completion_list_t *list = &state->list;
  sl_completion_item_t *item;
  size_t length = strlen(text), i;
  for (i = 0; i < list->count; ++i)
    if (!strcmp(list->items[i].text, text))
      return SL_OK;
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 8;
    sl_completion_item_t *next;
    if (capacity < list->capacity || capacity > (size_t)-1 / sizeof(*next))
      return SL_ERROR_NOMEM;
    next = realloc(list->items, capacity * sizeof(*next));
    if (!next)
      return SL_ERROR_NOMEM;
    list->items = next;
    list->capacity = capacity;
  }
  item = &list->items[list->count];
  item->text = malloc(length + 1);
  if (!item->text)
    return SL_ERROR_NOMEM;
  memcpy(item->text, text, length + 1);
  item->length = length;
  ++list->count;
  return SL_OK;
}
int sl_completion_find(const sl_completion_state_t *state, const char *text,
                       size_t cursor, sl_completion_match_t *match) {
  size_t i, prefix;
  memset(match, 0, sizeof(*match));
  if (!state->active || cursor <= state->start || cursor > state->end)
    return 0;
  prefix = cursor - state->start;
  match->start = state->start;
  for (i = 0; i < state->list.count; ++i) {
    const sl_completion_item_t *item = &state->list.items[i];
    if (item->length >= prefix &&
        !memcmp(text + state->start, item->text, prefix)) {
      if (!match->count)
        match->first = i;
      ++match->count;
    }
  }
  return match->count != 0;
}
