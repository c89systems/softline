#ifndef SOFTLINE_COMPLETION_H
#define SOFTLINE_COMPLETION_H

#include "softline/softline.h"
#include <stddef.h>

typedef struct sl_completion_item {
  char *text;
  size_t length;
} sl_completion_item_t;

typedef struct sl_completion_list {
  sl_completion_item_t *items;
  size_t count;
  size_t capacity;
} sl_completion_list_t;

typedef struct sl_completion_match {
  size_t start, count, first;
} sl_completion_match_t;

/* One Tab cycle owns the original local draft and its resolved candidate set.
 * Neither the producer transcript nor queue authority enters this state. */
typedef struct sl_completion_cycle {
  char *original;
  size_t *items;
  size_t count, selected, start, cursor, end, inserted;
  int updating;
} sl_completion_cycle_t;

typedef struct sl_completion_state {
  sl_completion_provider_t *providers;
  size_t provider_count;
  sl_completion_list_t list;
  sl_completion_cycle_t cycle;
  sl_completion_provider_t provider;
  size_t start, end, limit;
  int active, resolving, finishing, failed;
} sl_completion_state_t;

void sl_completion_clear(sl_completion_list_t *list);
int sl_completion_add(sl_completion_state_t *state, const char *text);
void sl_completion_reset(sl_completion_state_t *state);
int sl_completion_find(const sl_completion_state_t *state, const char *text,
                       size_t cursor, sl_completion_match_t *match);
int sl_completion_space(unsigned char ch);
void sl_completion_cycle_clear(sl_completion_cycle_t *cycle);

#endif
