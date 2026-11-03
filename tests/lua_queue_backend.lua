local softline = require("softline")
local requests, detached = {}, 0
local sl
local context = {}
local callbacks = {
  capabilities = (1 << (softline.QUEUE_RESOLVE + 1)) - 1,
  max_pending_requests = 4,
  request = function(request, ctx)
    assert(ctx == context)
    assert(not pcall(function() sl:queue_backend_update {kind = softline.QUEUE_DISCONNECTED} end))
    requests[#requests + 1] = request
    return true
  end,
  detach = function(ctx) assert(ctx == context); detached = detached + 1 end,
}
sl = softline.new {prompt_queue = true, queue_backend = callbacks, queue_backend_context = context}
callbacks.request = function() error("constructor failed to copy callbacks") end
assert(sl:queue_backend_update {revision = "q0", entries = {}, total_count = 0})
local id = assert(sl:queue_backend_request {text = "hello"})
assert(requests[1].request_id == id and requests[1].text == "hello")
assert(sl:queue_backend_pending(1).pending)
assert(sl:set_buffer("new draft"))
local entry = {id = "backend/one", revision = "v1", text = "hello", ready = true}
assert(sl:queue_backend_update {kind = softline.QUEUE_UPSERT,
  previous_revision = "q0", revision = "q1", entries = {entry}, total_count = 1,
  request_id = id})
assert(sl:queue_backend_entry(1).id == "backend/one")
assert(not sl:queue_info(1).eligible)
assert(not sl:queue_take(1))
assert(not sl:queue_clear())
assert(sl:queue_backend_update {kind = softline.QUEUE_REMOVE,
  previous_revision = "q1", revision = "q2", item_id = entry.id, total_count = 0})
assert(sl:queue_backend_result {request_id = id, item_id = entry.id, item_revision = "v1"})
assert(sl:queue_backend_result {request_id = id, item_id = entry.id, item_revision = "v1"})
assert(sl:queue_count() == 0 and sl:buffer() == "new draft")
id = assert(sl:queue_backend_request {text = "recover me"})
assert(sl:queue_backend_result {request_id = id, status = softline.QUEUE_REJECTED, reason = "rejected"})
assert(not sl:queue_backend_recover(id))
assert(sl:set_buffer(""))
assert(sl:queue_backend_recover(id) and sl:buffer() == "recover me")
id = assert(sl:queue_backend_request {text = "outcome unknown"})
assert(sl:queue_backend_update {kind = softline.QUEUE_DISCONNECTED})
assert(not sl:queue_backend_info().synchronized)
assert(sl:set_buffer(""))
assert(not sl:queue_backend_recover(id))
assert(sl:queue_backend_pending(1).status == softline.QUEUE_UNKNOWN)
assert(sl:queue_backend_update {revision = "q2", entries = {}, total_count = 0})
assert(sl:queue_backend_info().synchronized)
assert(not pcall(function() sl:queue_backend_request {text = "bad\0text"} end))
assert(not sl:queue_backend_update {kind = softline.QUEUE_STATE,
  previous_revision = "missing", revision = "q3", total_count = 0})
assert(sl:queue_backend_info().revision == "q2")
sl:close()
assert(detached == 1)
local local_editor = softline.new()
assert(not local_editor:queue_backend_info().enabled)
local_editor:close()
assert(not pcall(softline.new, {queue_backend = callbacks}))

-- Constructor coroutines may be collected. Every later callback runs on the
-- coroutine making that call, including legacy methods routed to the backend.
local caller, fail_request, fail_history, history_calls = nil, false, false, 0
local creator = coroutine.create(function()
  return softline.new {prompt_queue = true, queue_backend = {
    capabilities = (1 << (softline.QUEUE_RESOLVE + 1)) - 1,
    request = function()
      assert(coroutine.running() == caller)
      if fail_request then error('bridge handoff failed') end
      return true
    end,
  }}
end)
local resumed, migrated = coroutine.resume(creator)
assert(resumed and migrated)
creator = nil; collectgarbage('collect')
caller = coroutine.running()
assert(migrated:queue_backend_update {revision = 'q0', entries = {}, total_count = 0})
assert(migrated:set_buffer('newer draft'))
assert(migrated:history_set_backend('queue-history', {append = function(_, text)
  assert(coroutine.running() == caller)
  history_calls = history_calls + 1
  assert(not migrated:queue_backend_update {kind = softline.QUEUE_DISCONNECTED})
  if fail_history then error('history append failed') end
  assert(text == 'accepted once')
  return true
end}))
assert(migrated:history_set_auto_add(true))
id = assert(migrated:queue_backend_request {text = 'accepted once'})
caller = coroutine.create(function()
  assert(migrated:queue_backend_result {request_id = id, item_id = 'one', item_revision = 'v1'})
  assert(migrated:queue_backend_result {request_id = id, item_id = 'one', item_revision = 'v1'})
  assert(migrated:queue_resume())
end)
assert(coroutine.resume(caller))
assert(history_calls == 1 and migrated:buffer() == 'newer draft')
caller = coroutine.running()
assert(migrated:queue_backend_info().synchronized)
fail_request = true
assert(not migrated:queue_backend_request {text = 'not handed off'})
assert(migrated:buffer() == 'newer draft')
fail_request = false
id = assert(migrated:queue_backend_request {text = 'history failure'})
fail_history = true
local ok, code, message = migrated:queue_backend_result {request_id = id, item_id = 'two', item_revision = 'v1'}
assert(not ok and message:find('history append failed', 1, true))
assert(migrated:buffer() == 'newer draft')
assert(not pcall(function() migrated:queue_backend_request {operation = 0x100000000, text = 'bad enum'} end))
assert(not pcall(function() migrated:queue_backend_update {revision = 'q1', total_count = -1} end))
migrated:close()

-- Owning callbacks/context on the receiver permits ordinary reference cycles.
local weak, cycle_detached = setmetatable({}, {__mode = 'v'}), 0
do
  local editor
  local ctx = {}
  editor = softline.new {prompt_queue = true, queue_backend_context = ctx,
    queue_backend = {
      capabilities = (1 << softline.QUEUE_OFFER) | (1 << softline.QUEUE_SYNCHRONIZE),
      request = function() assert(editor); return true end,
      detach = function(context) assert(context.editor); cycle_detached = cycle_detached + 1 end,
    }}
  ctx.editor = editor; weak[1] = editor
end
collectgarbage('collect'); collectgarbage('collect')
assert(weak[1] == nil and cycle_detached == 1)
print("Lua backend requests, copied callbacks, late results, recovery and disconnect passed")
