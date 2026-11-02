#ifndef SOFTLINE_EDIT_H
#define SOFTLINE_EDIT_H

#include <stddef.h>

#define SL_EDIT_KILLS 10
enum sl_edit_command {
  SL_EDIT_OTHER,
  SL_EDIT_INSERT,
  SL_EDIT_PASTE,
  SL_EDIT_COMPLETE,
  SL_EDIT_KILL,
  SL_EDIT_YANK,
  SL_EDIT_YANK_POP
};
struct sl_undo_entry;
typedef struct sl_kill_entry {
  char *text;
  size_t len;
} sl_kill_entry_t;
/* Draft edits only. No producer bytes enter the journal or kill ring. */
typedef struct sl_edit {
  struct sl_undo_entry *undo;
  unsigned long group;
  int enabled, open, command, last_command; /* Active or suspended draft. */
  int changed; /* Current command edited, navigated or replaced its context. */
  char *stash; /* One saved draft, independent of undo and the kill ring. */
  sl_kill_entry_t kills[SL_EDIT_KILLS];
  int kill_count;
  int yank_valid, yank_index;
  size_t yank_at, yank_len;
} sl_edit_t;

void sl_edit_clear_undo(sl_edit_t *edit);
void sl_edit_destroy(sl_edit_t *edit);
void sl_edit_begin(sl_edit_t *edit, int command);
void sl_edit_end(sl_edit_t *edit);
int sl_edit_record(sl_edit_t *edit, size_t at, const char *old, size_t old_len,
                   size_t inserted, size_t cursor);
/* Validate the entire undo group before writing. Return -1 and retire a stale
 * journal without changing the draft if its ranges no longer fit the buffer.
 */
int sl_edit_undo(sl_edit_t *edit, char *buf, size_t cap, size_t *len,
                 size_t *cursor);
/* Prepare before editing; commit takes ownership only after deletion succeeds.
 */
char *sl_edit_prepare_kill(sl_edit_t *edit, const char *text, size_t len,
                           int backward, size_t *combined_len);
void sl_edit_commit_kill(sl_edit_t *edit, char *text, size_t len);

#endif
