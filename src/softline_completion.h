#ifndef SOFTLINE_COMPLETION_H
#define SOFTLINE_COMPLETION_H

#include "softline/softline.h"
#include <stddef.h>

typedef struct sl_completion_item {
  char *text;
  size_t length;
  sl_completion_scope_t scope;
} sl_completion_item_t;

typedef struct sl_completion_list {
  sl_completion_item_t *items;
  size_t count;
  size_t max_length;
} sl_completion_list_t;

typedef struct sl_completion_match {
  size_t start, common, count;
  size_t preview[3];
} sl_completion_match_t;

void sl_completion_clear(sl_completion_list_t *list);
int sl_completion_copy(sl_completion_list_t *out, const sl_completion_t *items,
                       size_t count);
int sl_completion_find(const sl_completion_list_t *list, const char *text,
                       size_t cursor, sl_completion_match_t *match);
int sl_completion_space(unsigned char ch);

#endif
