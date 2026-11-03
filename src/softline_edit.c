#include "softline_edit.h"
#include <stdlib.h>
#include <string.h>

struct sl_undo_entry {
  struct sl_undo_entry *next;
  unsigned long group;
  size_t at, removed, inserted, cursor;
  /* Removed bytes follow this structure in the same allocation. */
};

void sl_edit_clear_undo(sl_edit_t *edit) {
  while (edit->undo) {
    struct sl_undo_entry *entry = edit->undo;
    edit->undo = entry->next;
    free(entry);
  }
  edit->group = 0;
  edit->open = 0;
  edit->last_command = SL_EDIT_OTHER;
  edit->yank_valid = 0;
}

void sl_edit_destroy(sl_edit_t *edit) {
  int i;
  sl_edit_clear_undo(edit);
  free(edit->stash);
  for (i = 0; i < edit->kill_count; i++)
    free(edit->kills[i].text);
}

void sl_edit_begin(sl_edit_t *edit, int command) {
  if ((command != SL_EDIT_INSERT && command != SL_EDIT_PASTE &&
       command != SL_EDIT_COMPLETE) ||
      edit->last_command != command)
    edit->group++;
  edit->open = 1;
  edit->command = command;
  edit->changed = 0;
}

void sl_edit_end(sl_edit_t *edit) {
  if (edit->open) {
    edit->last_command = edit->command;
    edit->open = 0;
  }
}

int sl_edit_record(sl_edit_t *edit, size_t at, const char *old, size_t old_len,
                   size_t inserted, size_t cursor) {
  struct sl_undo_entry *entry;
  if (!edit->enabled || (!old_len && !inserted))
    return 0;
  if (!edit->open) {
    edit->group++;
    edit->last_command = SL_EDIT_OTHER;
  }
  entry = edit->undo;
  if (!old_len && entry && !entry->removed && entry->group == edit->group &&
      at == entry->at + entry->inserted) {
    entry->inserted += inserted;
    edit->yank_valid = 0;
    return 0;
  }
  if (old_len > (size_t)-1 - sizeof(*entry))
    return -1;
  entry = malloc(sizeof(*entry) + old_len);
  if (!entry)
    return -1;
  entry->next = edit->undo;
  entry->group = edit->group;
  entry->at = at;
  entry->removed = old_len;
  entry->inserted = inserted;
  entry->cursor = cursor;
  if (old_len)
    memcpy(entry + 1, old, old_len);
  edit->undo = entry;
  edit->yank_valid = 0;
  return 0;
}

int sl_edit_undo(sl_edit_t *edit, char *buf, size_t cap, size_t *len,
                 size_t *cursor, sl_edit_change_t *change) {
  unsigned long group;
  size_t restored;
  struct sl_undo_entry *check;
  size_t original = *len, first = *len, last = 0;
  int changed = 0;
  if (change)
    memset(change, 0, sizeof(*change));
  if (!edit->undo)
    return 0;
  group = edit->undo->group;
  restored = *len;
  if (!cap || restored >= cap)
    goto invalid;
  for (check = edit->undo; check && check->group == group;
       check = check->next) {
    if (check->at > restored || check->inserted > restored - check->at)
      goto invalid;
    restored -= check->inserted;
    if (check->removed > cap - 1 - restored)
      goto invalid;
    restored += check->removed;
    if (check->cursor > restored)
      goto invalid;
    if (!changed || last < check->at + check->inserted)
      last = check->at + check->removed;
    else
      last = last - check->inserted + check->removed;
    if (!changed || check->at < first)
      first = check->at;
    changed = 1;
  }
  do {
    struct sl_undo_entry *entry = edit->undo;
    size_t tail = *len - entry->at - entry->inserted;
    memmove(buf + entry->at + entry->removed, buf + entry->at + entry->inserted,
            tail);
    if (entry->removed)
      memcpy(buf + entry->at, entry + 1, entry->removed);
    *len = *len - entry->inserted + entry->removed;
    buf[*len] = '\0';
    *cursor = entry->cursor;
    edit->undo = entry->next;
    free(entry);
  } while (edit->undo && edit->undo->group == group);
  edit->yank_valid = 0;
  if (change) {
    change->at = first;
    change->removed = original - (*len - last) - first;
    change->inserted = last - first;
  }
  return 1;
invalid:
  sl_edit_clear_undo(edit);
  return -1;
}

char *sl_edit_prepare_kill(sl_edit_t *edit, const char *text, size_t len,
                           int backward, size_t *combined_len) {
  char *copy;
  size_t old_len = edit->command == SL_EDIT_KILL &&
                           edit->last_command == SL_EDIT_KILL &&
                           edit->kill_count
                       ? edit->kills[0].len
                       : 0;
  if (len >= (size_t)-1 - old_len)
    return NULL;
  *combined_len = old_len + len;
  copy = malloc(*combined_len + 1);
  if (!copy)
    return NULL;
  if (backward) {
    memcpy(copy, text, len);
    if (old_len)
      memcpy(copy + len, edit->kills[0].text, old_len);
  } else {
    if (old_len)
      memcpy(copy, edit->kills[0].text, old_len);
    memcpy(copy + old_len, text, len);
  }
  copy[*combined_len] = '\0';
  return copy;
}

void sl_edit_commit_kill(sl_edit_t *edit, char *text, size_t len) {
  if (edit->command == SL_EDIT_KILL && edit->last_command == SL_EDIT_KILL &&
      edit->kill_count) {
    free(edit->kills[0].text);
  } else {
    if (edit->kill_count == SL_EDIT_KILLS)
      free(edit->kills[--edit->kill_count].text);
    memmove(edit->kills + 1, edit->kills,
            (size_t)edit->kill_count * sizeof(edit->kills[0]));
    edit->kill_count++;
  }
  edit->kills[0].text = text;
  edit->kills[0].len = len;
}
