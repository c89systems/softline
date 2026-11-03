#include "softline_internal.h"

#include <limits.h>

typedef struct {
  sl_queue_entry_t value;
  sl_prompt_id_t local_id;
} backend_entry_t;

typedef struct {
  sl_queue_request_t value;
  int pending, seen, want_edit, want_steer, abandoned, sync_requested;
  sl_queue_result_status_t status;
  char *reason, *result_id, *result_revision, *result_token;
} backend_pending_t;
typedef struct {
  int key;
  int binding_done;
  size_t len;
  char bytes[8];
} backend_key_t;

struct sl_backend {
  sl_queue_backend_t callbacks;
  void *context;
  backend_entry_t *entries;
  size_t count, total;
  backend_pending_t *pending;
  size_t pending_count, pending_limit;
  sl_queue_request_id_t serial;
  char *revision;
  int synchronized, callback_active, needs_sync, sync_pending;
  backend_key_t *keys;
  size_t key_count, key_capacity, key_head;
  int reading_paste;
};

static sl_impl_t *editor(const sl_t *self) {
  return self ? (sl_impl_t *)self->impl : NULL;
}

static int error(sl_t *self, int status, const char *text) {
  sl_impl_t *impl = editor(self);
  if (impl)
    (void)snprintf(impl->error, sizeof(impl->error), "%s", text);
  return status;
}

static char *copy(const char *text) {
  char *value;
  size_t length;
  if (!text)
    return NULL;
  length = strlen(text) + 1;
  value = malloc(length);
  if (value)
    memcpy(value, text, length);
  return value;
}

static int token(const char *text, int optional) {
  return (!text && optional) || (text && *text && strlen(text) <= 127);
}

static int reserved_operation(sl_queue_operation_t operation) {
  return operation == SL_QUEUE_SYNCHRONIZE || operation == SL_QUEUE_RESOLVE ||
         operation == SL_QUEUE_STOP;
}

static void entry_free(backend_entry_t *entry) {
  free((void *)entry->value.id);
  free((void *)entry->value.revision);
  free((void *)entry->value.text);
  free((void *)entry->value.edit_token);
  memset(entry, 0, sizeof(*entry));
}

static void entries_free(backend_entry_t *entries, size_t count) {
  size_t index;
  for (index = 0; index < count; ++index)
    entry_free(entries + index);
  free(entries);
}

static int entry_copy(backend_entry_t *out, const sl_queue_entry_t *in,
                      size_t limit) {
  if (!token(in->id, 0) || !token(in->revision, 0) || !in->text || !*in->text ||
      strlen(in->text) > limit || !token(in->edit_token, 1) || in->held < 0 ||
      in->held > 1 || in->edit_held < 0 || in->edit_held > 1 ||
      (in->edit_token && !in->edit_held) || in->ready < 0 || in->ready > 1 ||
      in->stopped < 0 || in->stopped > 1 || in->action < 0 || in->action > 1 ||
      (in->mode != SL_PROMPT_QUEUE_MODE_QUEUED &&
       in->mode != SL_PROMPT_QUEUE_MODE_STEER))
    return SL_ERROR_INVALID;
  out->value = *in;
  out->value.id = copy(in->id);
  out->value.revision = copy(in->revision);
  out->value.text = copy(in->text);
  out->value.edit_token = copy(in->edit_token);
  if (!out->value.id || !out->value.revision || !out->value.text ||
      (in->edit_token && !out->value.edit_token)) {
    entry_free(out);
    return SL_ERROR_NOMEM;
  }
  return SL_OK;
}

static backend_entry_t *find(sl_backend_t *backend, const char *id) {
  size_t index;
  if (id)
    for (index = 0; index < backend->count; ++index)
      if (!strcmp(backend->entries[index].value.id, id))
        return backend->entries + index;
  return NULL;
}

static backend_entry_t *find_local(sl_backend_t *backend, sl_prompt_id_t id) {
  size_t index;
  for (index = 0; index < backend->count; ++index)
    if (backend->entries[index].local_id == id)
      return backend->entries + index;
  return NULL;
}

static void request_free(backend_pending_t *request) {
  free((void *)request->value.item_id);
  free((void *)request->value.item_revision);
  free((void *)request->value.queue_revision);
  free((void *)request->value.edit_token);
  free((void *)request->value.text);
  free(request->reason);
  free(request->result_id);
  free(request->result_revision);
  free(request->result_token);
  memset(request, 0, sizeof(*request));
}

static void request_remove(sl_backend_t *backend, size_t index) {
  request_free(backend->pending + index);
  --backend->pending_count;
  memmove(backend->pending + index, backend->pending + index + 1,
          (backend->pending_count - index) * sizeof(*backend->pending));
  memset(backend->pending + backend->pending_count, 0,
         sizeof(*backend->pending));
}

int sl_backend_create(sl_t *self, const sl_config_t *config) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend;
  size_t limit;
  if (!config->queue_backend)
    return SL_OK;
  limit = config->queue_backend->max_pending_requests;
  if (!limit)
    limit = 16;
  if (!config->prompt_queue || config->prompt_queue_max_entries == INT_MAX ||
      !config->queue_backend->request ||
      !(config->queue_backend->capabilities & (1UL << SL_QUEUE_OFFER)) ||
      !(config->queue_backend->capabilities & (1UL << SL_QUEUE_SYNCHRONIZE)) ||
      limit > (size_t)INT_MAX / sizeof(backend_pending_t) - 3)
    return SL_ERROR_INVALID;
  backend = calloc(1, sizeof(*backend));
  if (!backend)
    return SL_ERROR_NOMEM;
  backend->pending = calloc(limit + 3, sizeof(*backend->pending));
  if (!backend->pending) {
    free(backend);
    return SL_ERROR_NOMEM;
  }
  backend->callbacks = *config->queue_backend;
  backend->context = config->queue_backend_context;
  backend->pending_limit = limit;
  backend->needs_sync = 1;
  impl->backend = backend;
  impl->prompt_queue.remote = 1;
  return SL_OK;
}

void sl_backend_destroy(sl_t *self) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  size_t index;
  if (!backend)
    return;
  if (backend->callbacks.detach) {
    backend->callback_active = 1;
    backend->callbacks.detach(backend->context);
  }
  entries_free(backend->entries, backend->count);
  for (index = 0; index < backend->pending_count; ++index)
    request_free(backend->pending + index);
  free(backend->pending);
  free(backend->keys);
  free(backend->revision);
  free(backend);
  impl->backend = NULL;
}

int sl_backend_unsupported(sl_t *self) {
  return error(
      self, SL_ERROR_UNSUPPORTED,
      "backend queue operation requires an acknowledged backend request");
}

int sl_backend_callback_active(const sl_impl_t *impl) {
  return impl && impl->backend && impl->backend->callback_active;
}

int sl_queue_backend_request_impl(sl_t *self, const sl_queue_request_t *input,
                                  sl_queue_request_id_t *out) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  backend_pending_t pending;
  backend_entry_t *entry;
  int status;
  if (out)
    *out = 0;
  if (!backend || !input || backend->callback_active || impl->history_busy ||
      input->operation < SL_QUEUE_OFFER ||
      input->operation > SL_QUEUE_RESOLVE || !token(input->item_id, 1) ||
      !token(input->item_revision, 1) || !token(input->queue_revision, 1) ||
      !token(input->edit_token, 1) ||
      (input->mode != SL_PROMPT_QUEUE_MODE_QUEUED &&
       input->mode != SL_PROMPT_QUEUE_MODE_STEER) ||
      input->enabled < 0 || input->enabled > 1 ||
      (input->text && strlen(input->text) > impl->line_max_len))
    return error(self, SL_ERROR_INVALID,
                 "invalid or reentrant backend request");
  if (!(backend->callbacks.capabilities & (1UL << input->operation)))
    return sl_backend_unsupported(self);
  if (!backend->synchronized && input->operation != SL_QUEUE_SYNCHRONIZE &&
      input->operation != SL_QUEUE_RESOLVE)
    return error(self, SL_ERROR_NOT_READY,
                 "queue backend needs synchronization");
  if (input->operation == SL_QUEUE_RESOLVE) {
    size_t index;
    for (index = 0; index < backend->pending_count; ++index)
      if (backend->pending[index].value.request_id ==
              input->target_request_id &&
          !backend->pending[index].pending &&
          backend->pending[index].status == SL_QUEUE_UNKNOWN)
        break;
    if (index == backend->pending_count)
      return error(self, SL_ERROR_INVALID,
                   "RESOLVE needs an original unknown request identity");
  }
  {
    size_t index, ordinary = 0;
    int control = reserved_operation(input->operation);
    for (index = 0; index < backend->pending_count; ++index) {
      sl_queue_operation_t op = backend->pending[index].value.operation;
      if (!reserved_operation(op))
        ++ordinary;
      else if (control && op == input->operation)
        return error(self, SL_ERROR_NOT_READY,
                     "backend control request already outstanding");
    }
    if (!control && ordinary == backend->pending_limit)
      return error(self, SL_ERROR_FULL, "backend request retention is full");
  }
  if (backend->serial == ULONG_MAX)
    return error(self, SL_ERROR_FULL, "backend request identities exhausted");
  entry = find(backend, input->item_id);
  if (input->operation >= SL_QUEUE_BEGIN_EDIT &&
      input->operation <= SL_QUEUE_SET_INTENT && !entry)
    return error(self, SL_ERROR_CONFLICT,
                 "backend queue item is not in the view");
  if (entry && entry->value.held &&
      (input->operation == SL_QUEUE_BEGIN_EDIT ||
       input->operation == SL_QUEUE_APPROVE ||
       input->operation == SL_QUEUE_WITHDRAW)) {
    size_t index;
    for (index = 0; backend->entries + index != entry; ++index)
      if (backend->entries[index].value.held)
        return error(self, SL_ERROR_CONFLICT,
                     "approval actions must follow backend FIFO order");
  }
  if ((input->operation == SL_QUEUE_END_EDIT ||
       input->operation == SL_QUEUE_ABORT_EDIT) &&
      (!entry->value.edit_held || !entry->value.edit_token))
    return error(self, SL_ERROR_CONFLICT,
                 "backend edit hold is not owned by this attachment");
  {
    size_t index;
    for (index = 0; input->item_id && index < backend->pending_count; ++index)
      if (backend->pending[index].value.item_id &&
          !strcmp(backend->pending[index].value.item_id, input->item_id) &&
          backend->pending[index].value.operation != SL_QUEUE_OFFER &&
          (backend->pending[index].pending ||
           backend->pending[index].status == SL_QUEUE_UNKNOWN))
        return error(self, SL_ERROR_NOT_READY,
                     "an item operation is awaiting its outcome");
  }
  if ((input->operation == SL_QUEUE_OFFER ||
       input->operation == SL_QUEUE_END_EDIT) &&
      (!input->text || !*input->text))
    return error(self, SL_ERROR_INVALID,
                 "backend offer/replacement needs text");
  memset(&pending, 0, sizeof(pending));
  pending.value = *input;
  pending.value.request_id = ++backend->serial;
  pending.value.item_id = copy(input->item_id);
  pending.value.item_revision = copy(input->item_revision ? input->item_revision
                                     : entry ? entry->value.revision
                                             : NULL);
  pending.value.queue_revision =
      copy(input->queue_revision ? input->queue_revision : backend->revision);
  pending.value.edit_token = copy(input->edit_token ? input->edit_token
                                  : entry           ? entry->value.edit_token
                                                    : NULL);
  pending.value.text = copy(input->text);
  if ((input->item_id && !pending.value.item_id) ||
      ((input->item_revision || entry) && !pending.value.item_revision) ||
      ((input->queue_revision || backend->revision) &&
       !pending.value.queue_revision) ||
      ((input->edit_token || (entry && entry->value.edit_token)) &&
       !pending.value.edit_token) ||
      (input->text && !pending.value.text)) {
    request_free(&pending);
    return error(self, SL_ERROR_NOMEM, "cannot retain backend request");
  }
  pending.pending = 1;
  backend->callback_active = 1;
  status = backend->callbacks.request(backend->context, &pending.value);
  backend->callback_active = 0;
  if (status != SL_OK) {
    request_free(&pending);
    return error(self, status, "queue bridge did not accept the request");
  }
  if (out)
    *out = pending.value.request_id;
  backend->pending[backend->pending_count++] = pending;
  if (input->operation == SL_QUEUE_SYNCHRONIZE)
    backend->sync_pending = 1;
  return SL_OK;
}

int sl_queue_backend_result_impl(sl_t *self, const sl_queue_result_t *result) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  backend_pending_t *pending;
  size_t index;
  char *reason, *id, *revision, *edit_token;
  if (!backend || backend->callback_active || impl->history_busy || !result ||
      !result->request_id || result->request_id > backend->serial ||
      result->status < SL_QUEUE_ACCEPTED || result->status > SL_QUEUE_UNKNOWN ||
      !token(result->item_id, 1) || !token(result->item_revision, 1) ||
      !token(result->edit_token, 1) ||
      (result->reason && strlen(result->reason) > impl->line_max_len))
    return error(self, SL_ERROR_INVALID,
                 "invalid or reentrant backend response");
  for (index = 0; index < backend->pending_count; ++index)
    if (backend->pending[index].value.request_id == result->request_id)
      break;
  if (index == backend->pending_count)
    return SL_OK;
  pending = backend->pending + index;
  if (!pending->pending && pending->status != SL_QUEUE_UNKNOWN)
    return SL_OK;
  if (result->status == SL_QUEUE_ACCEPTED &&
      pending->value.operation <= SL_QUEUE_SET_INTENT &&
      (!result->item_id || !result->item_revision ||
       (pending->value.item_id &&
        strcmp(pending->value.item_id, result->item_id))))
    return error(
        self, SL_ERROR_INVALID,
        "backend acknowledgement needs the requested item identity/revision");
  if (result->status == SL_QUEUE_ACCEPTED &&
      pending->value.operation == SL_QUEUE_BEGIN_EDIT &&
      (!result->item_id || !result->item_revision || !result->edit_token))
    return error(self, SL_ERROR_INVALID,
                 "edit response needs identity, revision and token");
  reason = copy(result->reason);
  id = copy(result->item_id);
  revision = copy(result->item_revision);
  edit_token = copy(result->edit_token);
  if ((result->reason && !reason) || (result->item_id && !id) ||
      (result->item_revision && !revision) ||
      (result->edit_token && !edit_token)) {
    free(reason);
    free(id);
    free(revision);
    free(edit_token);
    return error(self, SL_ERROR_NOMEM, "cannot retain backend response");
  }
  free(pending->reason);
  free(pending->result_id);
  free(pending->result_revision);
  free(pending->result_token);
  pending->reason = reason;
  pending->result_id = id;
  pending->result_revision = revision;
  pending->result_token = edit_token;
  pending->pending = 0;
  pending->status = result->status;
  if (pending->value.operation == SL_QUEUE_SYNCHRONIZE &&
      result->status != SL_QUEUE_ACCEPTED) {
    backend->sync_pending = 0;
    backend->needs_sync = 0; /* Retry is explicit, never interval polling. */
  }
  if (result->reason)
    (void)error(self, SL_OK, result->reason);
  if (result->status == SL_QUEUE_ACCEPTED) {
    int history_status = SL_OK;
    if (impl->history_auto_add && pending->value.operation == SL_QUEUE_OFFER)
      history_status = sl_history_add(self, pending->value.text);
    /* Typed acquisitions have no native editor to open. Only native edit
     * intent (including cancelled intent awaiting release) retains its ACK. */
    if (!pending->want_edit && !pending->want_steer && !pending->abandoned)
      request_remove(backend, index);
    return history_status;
  }
  if ((result->status == SL_QUEUE_REJECTED ||
       result->status == SL_QUEUE_UNKNOWN) &&
      (pending->value.operation == SL_QUEUE_SYNCHRONIZE ||
       pending->value.operation == SL_QUEUE_RESOLVE))
    request_remove(backend, index);
  else if (result->status == SL_QUEUE_REJECTED && !pending->value.text)
    request_remove(backend, index);
  return SL_OK;
}

/* Prepare a complete replacement before changing the current editor/cache. */
static int mirror_commit(sl_t *self, backend_entry_t *entries, size_t count,
                         char *revision, const sl_queue_change_t *change) {
  sl_impl_t *impl = editor(self);
  sl_prompt_queue_t *queue = &impl->prompt_queue;
  sl_backend_t *backend = impl->backend;
  sl_prompt_queue_entry_t *items, edited;
  sl_prompt_id_t serial = queue->serial;
  size_t index, other, used = 0;
  int editing_index = -1;
  memset(&edited, 0, sizeof(edited));
  items = calloc(count + 1, sizeof(*items));
  if (!items)
    return SL_ERROR_NOMEM;
  for (index = 0; index < count; ++index) {
    backend_entry_t *before = find(backend, entries[index].value.id);
    sl_prompt_queue_entry_t *out;
    for (other = 0; other < index; ++other)
      if (!strcmp(entries[other].value.id, entries[index].value.id))
        goto invalid;
    if (before)
      entries[index].local_id = before->local_id;
    else {
      if (serial == ULONG_MAX)
        goto invalid;
      entries[index].local_id = ++serial;
    }
    if (entries[index].local_id == queue->editing.info.id &&
        entries[index].value.edit_held && entries[index].value.edit_token &&
        before && before->value.edit_token &&
        !strcmp(entries[index].value.edit_token, before->value.edit_token)) {
      out = &edited;
      editing_index = (int)used;
    } else
      out = items + used++;
    out->text = copy(entries[index].value.text);
    if (!out->text)
      goto nomem;
    out->info.id = entries[index].local_id;
    out->info.mode = entries[index].value.mode;
    out->info.held = entries[index].value.held;
    out->info.policy.executable = entries[index].value.ready;
    out->info.policy.enqueue = entries[index].value.action;
    out->info.stopped = entries[index].value.stopped;
  }
  for (index = 0; index < (size_t)queue->len; ++index)
    free(queue->items[index].text);
  free(queue->items);
  free(queue->editing.text);
  queue->items = items;
  queue->len = (int)used;
  queue->cap = (int)count + 1;
  queue->serial = serial;
  if (queue->editing.info.id && editing_index < 0)
    (void)error(self, SL_ERROR_CONFLICT,
                "backend edit ended; unsent text remains in the draft");
  queue->editing = edited;
  queue->editing_index = editing_index < 0 ? 0 : editing_index;
  queue->stopped = change->stopped;
  queue->auto_approval = change->auto_approval;
  entries_free(backend->entries, backend->count);
  backend->entries = entries;
  backend->count = count;
  backend->total = change->total_count;
  free(backend->revision);
  backend->revision = revision;
  backend->synchronized = 1;
  if (change->kind == SL_QUEUE_SNAPSHOT) {
    backend->needs_sync = 0;
    backend->sync_pending = 0;
  }
  for (index = 0; index < backend->pending_count; ++index)
    if (backend->pending[index].value.request_id == change->request_id)
      backend->pending[index].seen = 1;
    else if (change->kind == SL_QUEUE_SNAPSHOT &&
             !backend->pending[index].pending &&
             backend->pending[index].status == SL_QUEUE_ACCEPTED &&
             (backend->pending[index].value.operation == SL_QUEUE_BEGIN_EDIT ||
              backend->pending[index].sync_requested))
      backend->pending[index].seen = 1;
  return SL_OK;
invalid:
  for (index = 0; index < used; ++index)
    free(items[index].text);
  free(items);
  free(edited.text);
  return SL_ERROR_INVALID;
nomem:
  for (index = 0; index < used; ++index)
    free(items[index].text);
  free(items);
  free(edited.text);
  return SL_ERROR_NOMEM;
}

int sl_queue_backend_update_impl(sl_t *self, const sl_queue_change_t *change) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  backend_entry_t *entries, *existing;
  size_t count, index, source = 0, at = 0;
  char *revision;
  int status;
  if (!backend || backend->callback_active || impl->history_busy || !change ||
      change->kind < SL_QUEUE_SNAPSHOT || change->kind > SL_QUEUE_DISCONNECTED)
    return error(self, SL_ERROR_INVALID, "invalid or reentrant backend update");
  if (change->kind == SL_QUEUE_DISCONNECTED) {
    backend->synchronized = 0;
    backend->needs_sync = 0;
    backend->sync_pending = 0;
    for (index = 0; index < backend->pending_count; ++index) {
      backend->pending[index].sync_requested = 0;
      backend->pending[index].seen = 0;
      if (backend->pending[index].value.operation == SL_QUEUE_SYNCHRONIZE ||
          backend->pending[index].value.operation == SL_QUEUE_RESOLVE) {
        request_remove(backend, index--);
        continue;
      }
      if (backend->pending[index].pending ||
          backend->pending[index].status == SL_QUEUE_ACCEPTED) {
        backend->pending[index].pending = 0;
        backend->pending[index].status = SL_QUEUE_UNKNOWN;
      }
    }
    return error(self, SL_OK,
                 "queue disconnected; request outcomes are unknown");
  }
  if (!token(change->revision, 0) || change->stopped < 0 ||
      change->stopped > 1 || change->auto_approval < 0 ||
      change->auto_approval > 1 ||
      ((change->kind == SL_QUEUE_REMOVE || change->kind == SL_QUEUE_STATE) &&
       change->count) ||
      (change->kind == SL_QUEUE_REMOVE && !token(change->item_id, 0)))
    return error(self, SL_ERROR_INVALID, "invalid backend queue state");
  if (backend->revision && !strcmp(change->revision, backend->revision) &&
      (change->kind != SL_QUEUE_SNAPSHOT ||
       (backend->synchronized && !backend->sync_pending)))
    return SL_OK;
  if (change->kind == SL_QUEUE_SNAPSHOT && backend->revision &&
      backend->synchronized && !backend->sync_pending)
    return error(
        self, SL_ERROR_CONFLICT,
        "unsolicited replacement snapshot would regress the subscription");
  if (change->kind != SL_QUEUE_SNAPSHOT &&
      (!backend->synchronized || !change->previous_revision ||
       strcmp(change->previous_revision, backend->revision))) {
    backend->synchronized = 0;
    backend->needs_sync = 1;
    return error(self, SL_ERROR_CONFLICT,
                 "backend revision gap; snapshot required");
  }
  existing = find(backend, change->item_id);
  count = change->kind == SL_QUEUE_SNAPSHOT ? change->count : backend->count;
  if (change->kind == SL_QUEUE_UPSERT) {
    if (change->count != 1 || !change->entries)
      return error(self, SL_ERROR_INVALID, "UPSERT needs one backend entry");
    existing = find(backend, change->entries[0].id);
    if (!existing)
      ++count;
    at = existing ? (size_t)(existing - backend->entries) : change->index;
    if (at >= count || (existing && change->index != at))
      return error(self, SL_ERROR_INVALID,
                   "backend UPSERT cannot reorder entries");
  } else if (change->kind == SL_QUEUE_REMOVE) {
    if (!existing) {
      backend->synchronized = 0;
      backend->needs_sync = 1;
      return error(self, SL_ERROR_CONFLICT,
                   "removed backend item is outside the view");
    }
    at = (size_t)(existing - backend->entries);
    --count;
  }
  if (count > (size_t)impl->prompt_queue.max_entries ||
      change->total_count < count ||
      (change->kind == SL_QUEUE_SNAPSHOT && count && !change->entries))
    return error(self, SL_ERROR_FULL,
                 "backend update exceeds the bounded mirror");
  entries = calloc(count + 1, sizeof(*entries));
  revision = copy(change->revision);
  if (!entries || !revision) {
    free(entries);
    free(revision);
    return SL_ERROR_NOMEM;
  }
  for (index = 0; index < count; ++index) {
    const sl_queue_entry_t *value;
    if (change->kind == SL_QUEUE_SNAPSHOT)
      value = change->entries + index;
    else if (change->kind == SL_QUEUE_UPSERT && index == at) {
      value = change->entries;
      if (existing)
        ++source;
    } else {
      if (change->kind == SL_QUEUE_REMOVE && source == at)
        ++source;
      value = &backend->entries[source++].value;
    }
    status = entry_copy(entries + index, value, impl->line_max_len);
    if (status != SL_OK) {
      entries_free(entries, count);
      free(revision);
      return error(self, status, "cannot copy bounded backend update");
    }
  }
  status = mirror_commit(self, entries, count, revision, change);
  if (status != SL_OK) {
    entries_free(entries, count);
    free(revision);
    return error(self, status, "cannot commit backend update");
  }
  return SL_OK;
}

int sl_queue_backend_info_impl(const sl_t *self, sl_queue_backend_info_t *out) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  if (!impl || !out)
    return SL_ERROR_INVALID;
  memset(out, 0, sizeof(*out));
  if (backend) {
    out->enabled = 1;
    out->synchronized = backend->synchronized;
    out->revision = backend->revision;
    out->visible_count = backend->count;
    out->total_count = backend->total;
    out->pending_count = backend->pending_count;
  }
  return SL_OK;
}

int sl_queue_backend_pending_impl(const sl_t *self, size_t index,
                                  sl_queue_pending_info_t *out) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  backend_pending_t *pending;
  if (!backend || !out || index >= backend->pending_count)
    return SL_ERROR_INVALID;
  pending = backend->pending + index;
  out->request = pending->value;
  out->pending = pending->pending;
  out->status = pending->status;
  out->reason = pending->reason;
  return SL_OK;
}

int sl_queue_backend_entry_impl(const sl_t *self, size_t index,
                                sl_queue_entry_t *out) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  if (!backend || !out || index >= backend->count)
    return SL_ERROR_INVALID;
  *out = backend->entries[index].value;
  return SL_OK;
}

int sl_queue_backend_recover_impl(sl_t *self, sl_queue_request_id_t id) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  size_t index;
  if (!backend || backend->callback_active || impl->history_busy || impl->len ||
      impl->prompt_queue.editing.info.id)
    return SL_ERROR_INVALID;
  for (index = 0; index < backend->pending_count; ++index) {
    backend_pending_t *pending = backend->pending + index;
    if (pending->value.request_id != id)
      continue;
    if (pending->pending || pending->status != SL_QUEUE_REJECTED ||
        !pending->value.text)
      return error(self, SL_ERROR_NOT_READY,
                   "request outcome must be resolved before recovery");
    if (sl_set_buffer(self, pending->value.text) != SL_OK)
      return SL_ERROR_NOMEM;
    request_remove(backend, index);
    return SL_OK;
  }
  return SL_ERROR_INVALID;
}

int sl_backend_recover_newest(sl_t *self) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl->backend;
  size_t index = backend->pending_count;
  while (index) {
    backend_pending_t *pending = backend->pending + --index;
    if (!pending->pending && pending->status == SL_QUEUE_REJECTED &&
        pending->value.text)
      return sl_queue_backend_recover_impl(self, pending->value.request_id);
  }
  return SL_ERROR_NOT_READY;
}

int sl_backend_action(sl_t *self, sl_prompt_id_t id, sl_queue_operation_t op) {
  sl_impl_t *impl = editor(self);
  backend_entry_t *entry = find_local(impl->backend, id);
  sl_queue_request_t request;
  int status;
  if (!entry)
    return SL_ERROR_CONFLICT;
  memset(&request, 0, sizeof(request));
  request.operation = op;
  request.item_id = entry->value.id;
  request.mode = entry->value.mode;
  if (op == SL_QUEUE_SET_INTENT)
    request.mode = SL_PROMPT_QUEUE_MODE_STEER;
  status = sl_queue_backend_request_impl(self, &request, NULL);
  if (status == SL_OK && op == SL_QUEUE_BEGIN_EDIT)
    impl->backend->pending[impl->backend->pending_count - 1].want_edit = 1;
  return status;
}

int sl_backend_edit(sl_t *self, int index) {
  sl_impl_t *impl = editor(self);
  size_t pending_index;
  if (impl->len || impl->prompt_queue.editing.info.id)
    return SL_ERROR_NOT_READY;
  impl->backend->reading_paste = 0;
  if (sl_backend_recover_newest(self) == SL_OK)
    return SL_OK;
  if (!(impl->backend->callbacks.capabilities & (1UL << SL_QUEUE_BEGIN_EDIT)))
    return sl_backend_unsupported(self);
  for (pending_index = impl->backend->pending_count; pending_index;) {
    backend_pending_t *pending = impl->backend->pending + --pending_index;
    if ((pending->value.operation == SL_QUEUE_OFFER ||
         pending->value.operation == SL_QUEUE_SET_INTENT) &&
        pending->pending) {
      pending->want_edit = 1;
      return SL_OK;
    }
  }
  if (index < 0 || index >= impl->prompt_queue.len)
    return SL_ERROR_INVALID;
  return sl_backend_action(self, impl->prompt_queue.items[index].info.id,
                           SL_QUEUE_BEGIN_EDIT);
}

int sl_backend_promote_newest(sl_t *self) {
  sl_impl_t *impl = editor(self);
  size_t index = impl->backend->pending_count;
  if (!(impl->backend->callbacks.capabilities & (1UL << SL_QUEUE_SET_INTENT)))
    return sl_backend_unsupported(self);
  while (index) {
    backend_pending_t *pending = impl->backend->pending + --index;
    if (pending->value.operation == SL_QUEUE_OFFER && pending->pending) {
      pending->want_steer = 1;
      return SL_OK;
    }
  }
  return impl->prompt_queue.len
             ? sl_backend_action(
                   self,
                   impl->prompt_queue.items[impl->prompt_queue.len - 1].info.id,
                   SL_QUEUE_SET_INTENT)
             : SL_OK;
}

int sl_backend_wait_edit(const sl_impl_t *impl) {
  size_t i;
  if (!impl->backend || impl->len || impl->prompt_queue.editing.info.id ||
      impl->history_search_active)
    return 0;
  for (i = 0; i < impl->backend->pending_count; ++i) {
    backend_pending_t *p = impl->backend->pending + i;
    if ((p->pending || p->status == SL_QUEUE_ACCEPTED) && !p->abandoned &&
        p->want_edit)
      return 1;
  }
  return 0;
}
int sl_backend_reading_paste(const sl_impl_t *impl) {
  return impl->backend && impl->backend->reading_paste;
}
static int pending_item(const sl_impl_t *impl) {
  size_t i;
  if (!impl->backend)
    return 0;
  for (i = 0; i < impl->backend->pending_count; ++i) {
    backend_pending_t *p = impl->backend->pending + i;
    if (p->pending && p->value.operation >= SL_QUEUE_BEGIN_EDIT &&
        p->value.operation <= SL_QUEUE_SET_INTENT)
      return 1;
  }
  return 0;
}
int sl_backend_wait_keys(const sl_impl_t *impl) {
  return sl_backend_wait_edit(impl) ||
         (impl->backend && impl->backend->key_count && pending_item(impl));
}
int sl_backend_wait_key(const sl_impl_t *impl, int key) {
  if (!pending_item(impl))
    return 0;
  return key == SL_KEY_ALT_A || key == SL_KEY_ALT_Y ||
         key == SL_KEY_ALT_BASE + 'Y' || key == SL_KEY_ALT_BASE + 'n' ||
         key == SL_KEY_ALT_BASE + 'N' ||
         key == (int)impl->prompt_queue.keys.edit_newest ||
         (!impl->len &&
          key == (int)impl->prompt_queue.keys.submit_or_promote_newest);
}
int sl_backend_push_key(sl_t *self, int key, const char *bytes, size_t len,
                        int binding_done) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *b = impl->backend;
  backend_key_t *keys;
  size_t index;
  /* Leave room for bracketed-paste framing and ordinary edit controls around
   * a full-size draft while keeping deferred keyboard input bounded. */
  size_t limit = impl->line_max_len + SL_PENDING_INPUT_MAX;
  if (len > sizeof(b->keys[0].bytes) || b->key_count >= limit)
    return error(
        self, SL_ERROR_FULL,
        "input waiting for the edit hold exceeds the configured limit");
  if (b->key_count == b->key_capacity) {
    size_t capacity = b->key_capacity ? b->key_capacity * 2 : 32;
    if (capacity > limit)
      capacity = limit;
    if (capacity > (size_t)-1 / sizeof(*keys))
      return SL_ERROR_NOMEM;
    keys = malloc(capacity * sizeof(*keys));
    if (!keys)
      return SL_ERROR_NOMEM;
    for (index = 0; index < b->key_count; ++index)
      keys[index] = b->keys[(b->key_head + index) % b->key_capacity];
    free(b->keys);
    b->keys = keys;
    b->key_capacity = capacity;
    b->key_head = 0;
  }
  index = (b->key_head + b->key_count) % b->key_capacity;
  b->keys[index].key = key;
  b->keys[index].binding_done = binding_done;
  b->keys[index].len = len;
  if (len)
    memcpy(b->keys[index].bytes, bytes, len);
  ++b->key_count;
  if (key == SL_KEY_PASTE_BEGIN)
    b->reading_paste = 1;
  if (key == SL_KEY_PASTE_END)
    b->reading_paste = 0;
  return SL_OK;
}
int sl_backend_pop_key(sl_impl_t *impl, int *key, char *bytes, size_t *len,
                       int *binding_done) {
  sl_backend_t *b = impl->backend;
  if (!b || !b->key_count)
    return 0;
  *key = b->keys[b->key_head].key;
  *binding_done = b->keys[b->key_head].binding_done;
  *len = b->keys[b->key_head].len;
  if (*len)
    memcpy(bytes, b->keys[b->key_head].bytes, *len);
  --b->key_count;
  b->key_head = b->key_count ? (b->key_head + 1) % b->key_capacity : 0;
  return 1;
}

int sl_backend_submit(sl_t *self, sl_prompt_queue_mode_t mode) {
  sl_impl_t *impl = editor(self);
  sl_queue_request_t request;
  backend_entry_t *entry;
  int status;
  if (!impl->len && !impl->prompt_queue.editing.info.id)
    return SL_OK;
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_OFFER;
  request.mode = mode;
  request.text = impl->buf;
  if (impl->prompt_queue.editing.info.id) {
    entry = find_local(impl->backend, impl->prompt_queue.editing.info.id);
    if (!entry)
      return SL_ERROR_CONFLICT;
    request.operation = impl->len ? SL_QUEUE_END_EDIT : SL_QUEUE_WITHDRAW;
    request.item_id = entry->value.id;
    request.mode =
        mode == SL_PROMPT_QUEUE_MODE_STEER ? mode : entry->value.mode;
    request.edit_token = entry->value.edit_token;
  }
  status = sl_queue_backend_request_impl(self, &request, NULL);
  if (status != SL_OK)
    return status;
  if (impl->prompt_queue.editing.info.id)
    sl_queue_cached_edit_restore(self);
  else {
    /* The proposal is already retained and handed off. Clearing must not
     * allocate a throwaway undo entry and fail after that commitment. */
    impl->buf[0] = '\0';
    impl->len = impl->cursor = 0;
    impl->edit.changed = 1;
    impl->preferred_column_valid = 0;
  }
  sl_edit_clear_undo(&impl->edit);
  return SL_OK;
}

int sl_backend_cancel_edit(sl_t *self) {
  sl_impl_t *impl = editor(self);
  backend_entry_t *entry =
      find_local(impl->backend, impl->prompt_queue.editing.info.id);
  sl_queue_request_t request;
  int status;
  if (!entry)
    return SL_ERROR_CONFLICT;
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_ABORT_EDIT;
  request.item_id = entry->value.id;
  request.text = impl->buf;
  request.mode = entry->value.mode;
  status = sl_queue_backend_request_impl(self, &request, NULL);
  /* On failure/detach the backend edit hold remains authoritative. Preserve
   * the unsubmitted edit locally rather than releasing it on a timeout. */
  if (status == SL_OK)
    sl_queue_cached_edit_restore(self);
  return status;
}

void sl_backend_cancel_waiting(sl_impl_t *impl) {
  sl_backend_t *backend = impl->backend;
  size_t i;
  if (!backend)
    return;
  backend->key_count = 0;
  backend->key_head = 0;
  backend->reading_paste = 0;
  for (i = 0; i < backend->pending_count; ++i) {
    backend_pending_t *p = backend->pending + i;
    if (p->value.operation == SL_QUEUE_BEGIN_EDIT && p->want_edit)
      p->abandoned = 1;
    p->want_edit = p->want_steer = 0;
    if (!p->abandoned && !p->pending && p->status == SL_QUEUE_ACCEPTED)
      request_remove(backend, i--);
  }
}

/* Results can precede ordered state. Query once after acceptance rather than
 * trusting a pre-acceptance reconnect snapshot or waiting for a lost delta. */
static int wait_for_snapshot(sl_t *self, backend_pending_t *pending) {
  sl_backend_t *backend = editor(self)->backend;
  if (pending->sync_requested && !backend->sync_pending &&
      !backend->needs_sync) {
    (void)error(self, SL_ERROR_NOT_READY,
                "accepted item is unavailable; queue synchronization failed");
    return 0;
  }
  if (!backend->sync_pending && !pending->sync_requested) {
    pending->sync_requested = 1;
    backend->needs_sync = 1;
  }
  return 1;
}

void sl_backend_pump(sl_t *self) {
  sl_impl_t *impl = editor(self);
  sl_backend_t *backend = impl ? impl->backend : NULL;
  sl_queue_request_t request;
  size_t index;
  if (!backend || backend->callback_active)
    return;
  if (backend->needs_sync && !backend->sync_pending) {
    int outstanding = 0;
    for (index = 0; index < backend->pending_count; ++index)
      if (backend->pending[index].value.operation == SL_QUEUE_SYNCHRONIZE)
        outstanding = 1;
    /* The previous snapshot can arrive before its request's acknowledgement.
     * Wait for that reserved control slot rather than treating it as failure.
     */
    if (!outstanding) {
      memset(&request, 0, sizeof(request));
      request.operation = SL_QUEUE_SYNCHRONIZE;
      if (sl_queue_backend_request_impl(self, &request, NULL) != SL_OK)
        backend->needs_sync = 0;
    }
  }
  if (!backend->synchronized)
    return;
  for (index = 0; index < backend->pending_count; ++index) {
    backend_pending_t *pending = backend->pending + index;
    backend_entry_t *entry;
    int queue_index;
    if (!pending->pending && pending->status == SL_QUEUE_ACCEPTED &&
        (pending->value.operation == SL_QUEUE_OFFER ||
         pending->value.operation == SL_QUEUE_SET_INTENT) &&
        (pending->want_edit || pending->want_steer)) {
      sl_prompt_id_t local_id;
      int edit = pending->want_edit, steer;
      entry = find(backend, pending->result_id);
      if (!entry && !pending->seen) {
        if (!wait_for_snapshot(self, pending))
          request_remove(backend, index--);
        continue;
      }
      local_id = entry ? entry->local_id : 0;
      steer = pending->want_steer && entry &&
              entry->value.mode != SL_PROMPT_QUEUE_MODE_STEER;
      request_remove(backend, index--);
      if (local_id && (steer || edit) &&
          sl_backend_action(self, local_id,
                            steer ? SL_QUEUE_SET_INTENT
                                  : SL_QUEUE_BEGIN_EDIT) == SL_OK &&
          steer)
        backend->pending[backend->pending_count - 1].want_edit = edit;
      continue;
    }
    if (pending->pending || pending->status != SL_QUEUE_ACCEPTED ||
        pending->value.operation != SL_QUEUE_BEGIN_EDIT)
      continue;
    entry = find(backend, pending->result_id);
    if (!entry || strcmp(entry->value.revision, pending->result_revision) ||
        !entry->value.edit_token ||
        strcmp(entry->value.edit_token, pending->result_token)) {
      if (pending->seen) {
        (void)error(self, SL_ERROR_CONFLICT,
                    "backend item changed before editing could open");
        request_remove(backend, index--);
      } else if (!wait_for_snapshot(self, pending))
        request_remove(backend, index--);
      continue;
    }
    if (pending->abandoned) {
      backend_pending_t completed = *pending;
      int status;
      memset(&request, 0, sizeof(request));
      request.operation = SL_QUEUE_ABORT_EDIT;
      request.item_id = entry->value.id;
      /* Retain the acknowledged hold until the bridge accepts its release.
       * No editor draft may open after cancellation, even for a late ACK. */
      /* Reuse this completed request's retention slot, including when the
       * configured ordinary budget is one. Restore it if handoff fails. */
      --backend->pending_count;
      memmove(pending, pending + 1,
              (backend->pending_count - index) * sizeof(*pending));
      status = sl_queue_backend_request_impl(self, &request, NULL);
      if (status == SL_OK) {
        request_free(&completed);
        --index;
      } else {
        memmove(pending + 1, pending,
                (backend->pending_count - index) * sizeof(*pending));
        *pending = completed;
        ++backend->pending_count;
      }
      continue;
    }
    if (impl->len || impl->prompt_queue.editing.info.id ||
        impl->history_search_active)
      continue;
    for (queue_index = 0; queue_index < impl->prompt_queue.len; ++queue_index)
      if (impl->prompt_queue.items[queue_index].info.id == entry->local_id)
        break;
    if (queue_index < impl->prompt_queue.len &&
        sl_queue_cached_edit_begin(self, queue_index) == SL_OK)
      request_remove(backend, index--);
  }
}

int sl_backend_notice(const sl_impl_t *impl, char *text, size_t size) {
  const sl_backend_t *backend = impl->backend;
  size_t index, pending = 0, rejected = 0, unknown = 0;
  if (!backend)
    return 0;
  for (index = 0; index < backend->pending_count; ++index) {
    if (backend->pending[index].pending ||
        backend->pending[index].status == SL_QUEUE_ACCEPTED)
      ++pending;
    else if (backend->pending[index].status == SL_QUEUE_REJECTED)
      ++rejected;
    else if (backend->pending[index].status == SL_QUEUE_UNKNOWN)
      ++unknown;
  }
  if (!backend->synchronized || pending || rejected || unknown ||
      backend->total > backend->count) {
    (void)snprintf(
        text, size,
        "P %lu pending / %lu rejected (Alt-E recover) / %lu unknown%s",
        (unsigned long)pending, (unsigned long)rejected, (unsigned long)unknown,
        backend->synchronized ? "" : " / disconnected or synchronizing");
    if (backend->total > backend->count) {
      size_t used = strlen(text);
      (void)snprintf(text + used, size > used ? size - used : 0,
                     " / showing %lu of %lu", (unsigned long)backend->count,
                     (unsigned long)backend->total);
    }
    return 1;
  }
  return 0;
}

int sl_queue_backend_request(sl_t *s, const sl_queue_request_t *r,
                             sl_queue_request_id_t *id) {
  if (id)
    *id = 0;
  return s && s->queue_backend_request ? s->queue_backend_request(s, r, id)
                                       : SL_ERROR_INVALID;
}
int sl_queue_backend_result(sl_t *s, const sl_queue_result_t *r) {
  return s && s->queue_backend_result ? s->queue_backend_result(s, r)
                                      : SL_ERROR_INVALID;
}
int sl_queue_backend_update(sl_t *s, const sl_queue_change_t *c) {
  return s && s->queue_backend_update ? s->queue_backend_update(s, c)
                                      : SL_ERROR_INVALID;
}
int sl_queue_backend_info(const sl_t *s, sl_queue_backend_info_t *o) {
  return s && s->queue_backend_info ? s->queue_backend_info(s, o)
                                    : SL_ERROR_INVALID;
}
int sl_queue_backend_pending(const sl_t *s, size_t i,
                             sl_queue_pending_info_t *o) {
  return s && s->queue_backend_pending ? s->queue_backend_pending(s, i, o)
                                       : SL_ERROR_INVALID;
}
int sl_queue_backend_entry(const sl_t *s, size_t i, sl_queue_entry_t *o) {
  return s && s->queue_backend_entry ? s->queue_backend_entry(s, i, o)
                                     : SL_ERROR_INVALID;
}
int sl_queue_backend_recover(sl_t *s, sl_queue_request_id_t id) {
  return s && s->queue_backend_recover ? s->queue_backend_recover(s, id)
                                       : SL_ERROR_INVALID;
}
