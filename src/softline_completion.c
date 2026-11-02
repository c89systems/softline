#include "softline_completion.h"
#include <stdlib.h>
#include <string.h>

int sl_completion_space(unsigned char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
         ch == '\f';
}

void sl_completion_clear(sl_completion_list_t *list) {
  size_t i;
  for (i = 0; i < list->count; ++i)
    free(list->items[i].text);
  free(list->items);
  memset(list, 0, sizeof(*list));
}

int sl_completion_copy(sl_completion_list_t *out, const sl_completion_t *items,
                       size_t count) {
  size_t i, j;
  memset(out, 0, sizeof(*out));
  if (count > (size_t)-1 / sizeof(*out->items))
    return SL_ERROR_INVALID;
  if (!count)
    return SL_OK;
  out->items = calloc(count, sizeof(*out->items));
  if (!out->items)
    return SL_ERROR_NOMEM;
  for (i = 0; i < count; ++i) {
    sl_completion_item_t *item;
    for (j = 0; j < out->count; ++j)
      if (!strcmp(out->items[j].text, items[i].text))
        break;
    if (j < out->count) {
      if (items[i].scope == SL_COMPLETION_WORD)
        out->items[j].scope = SL_COMPLETION_WORD;
      continue;
    }
    item = &out->items[out->count++];
    item->length = strlen(items[i].text);
    if (out->max_length < item->length)
      out->max_length = item->length;
    item->scope = items[i].scope;
    item->text = malloc(item->length + 1);
    if (!item->text) {
      sl_completion_clear(out);
      return SL_ERROR_NOMEM;
    }
    memcpy(item->text, items[i].text, item->length + 1);
  }
  return SL_OK;
}

/* Prefer the longest matching phrase prefix when several whitespace anchors
 * qualify. Candidates from different replacement spans never share an LCP. */
int sl_completion_find(const sl_completion_list_t *list, const char *text,
                       size_t cursor, sl_completion_match_t *match) {
  size_t start, i;
  memset(match, 0, sizeof(*match));
  if (!cursor || !list->count)
    return 0;
  for (start = cursor > list->max_length ? cursor - list->max_length : 0;
       start < cursor; ++start) {
    size_t prefix = cursor - start;
    if (start && !sl_completion_space((unsigned char)text[start - 1]))
      continue;
    for (i = 0; i < list->count; ++i) {
      const sl_completion_item_t *item = &list->items[i];
      size_t common;
      if ((start && item->scope != SL_COMPLETION_WORD) ||
          prefix > item->length || memcmp(text + start, item->text, prefix))
        continue;
      if (!match->count) {
        match->start = start;
        match->common = item->length;
      }
      if (match->count < 3)
        match->preview[match->count] = i;
      ++match->count;
      common = 0;
      while (common < match->common && common < item->length &&
             item->text[common] == list->items[match->preview[0]].text[common])
        ++common;
      match->common = common;
    }
    if (match->count)
      return 1;
  }
  return 0;
}
