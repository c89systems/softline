local softline = require("softline")
local sl = assert(softline.new({prompt_queue = true, prompt_theme = softline.PROMPT_THEME_PLAIN}))
assert(sl:set_queue_profile("queued_turns"))
assert(sl:set_buffer("independent draft"))
assert(sl:queue_stop() and sl:queue_stopped())
assert(sl:buffer() == "independent draft")
assert(sl:queue_resume() and not sl:queue_stopped())
assert(sl:set_buffer(""))
assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte("s"), function()
  return softline.KEY_ACTION_SUSPEND
end))
assert(sl:bind_key(softline.KEY_ESCAPE, function()
  return softline.KEY_ACTION_CANCEL
end))
local edited, history, calls = 0, 0, 0
assert(sl:history_set_backend("policy", {append = function()
  history = history + 1
  return true
end}))
assert(sl:history_set_auto_add(true))
local function policy(text, mode, initial)
  calls = calls + 1
  local draft = sl:queue_draft_info()
  if draft then edited = draft.id end
  assert(type(initial.enqueue) == "boolean")
  assert(mode == softline.QUEUE_MODE_QUEUED or mode == softline.QUEUE_MODE_STEER)
  assert(not pcall(function() sl:close() end), "callback destroyed editor")
  return {enqueue = text:sub(1, 1) == "/" or text:sub(1, 1) == "!",
          independent = text == "/status",
          executable = text:sub(1, 5) ~= "/hold"}
end
assert(sl:set_queue_policy(policy))
local function suspended(prompt)
  local text = sl:next_prompt(prompt)
  assert(text == nil and sl:last_readline_status() == softline.READLINE_SUSPENDED)
end
local text, source, id = sl:next_prompt("policy-1> ")
assert(text == "/commandX" and id == edited and id > 0)
assert(source == softline.PROMPT_SOURCE_QUEUED and calls == 2 and history == 1)
assert(sl:set_status_busy(true))
suspended("policy-2> ")
local held = assert(sl:queue_info(1))
assert(held.policy.enqueue and not held.policy.executable and not held.held)
assert(not held.eligible and not held.stopped)
assert(not sl:queue_set_executable(0, true))
assert(not sl:queue_set_executable(-1, true))
assert(not sl:queue_set_executable(0x100000000 + held.id, true))
assert(not sl:queue_set_executable(math.maxinteger, true))
assert(not pcall(function() sl:queue_set_executable(held.id, "invalid") end))
assert(not sl:queue_info(1).policy.executable)
local rejected = sl:queue_take(1)
assert(rejected == nil)
text, source = sl:next_prompt("policy-3> ")
assert(text == "/status" and source == softline.PROMPT_SOURCE_QUEUED)
assert(sl:queue_count() == 1 and sl:queue_info(1).id == held.id)
suspended("policy-4> ")
assert(sl:queue_info(1).mode == softline.QUEUE_MODE_STEER)
assert(not sl:queue_info(1).policy.executable)
assert(sl:queue_set_executable(held.id, true))
text, source, id = sl:next_prompt("unused> ")
assert(text == "/hold review" and id == held.id)
assert(source == softline.PROMPT_SOURCE_PROMOTED)
local old = assert(sl:queue_append("old turn", false))
text = sl:next_prompt("policy-5> ")
assert(text == nil and sl:last_readline_status() == softline.READLINE_CANCELLED)
assert(sl:queue_stopped() and sl:queue_info(1).stopped)
local passive = assert(sl:queue_append("passive", false))
assert(sl:set_status_busy(false))
assert(sl:set_queue_delivery("auto"))
text = sl:next_prompt("policy-6> ")
assert(text == "/status" and sl:queue_stopped())
assert(sl:queue_info(1).id == old and sl:queue_info(2).id == passive)
text, source, id = sl:next_prompt("policy-7> ")
assert(text == "/goal resume" and source == softline.PROMPT_SOURCE_QUEUED and id > 0)
assert(sl:queue_stopped() and sl:queue_count() == 2)
assert(sl:queue_resume() and not sl:queue_stopped())
text, source, id = sl:next_prompt("unused> ")
assert(text == "old turn" and id == old)
text, source, id = sl:next_prompt("unused> ")
assert(text == "passive" and id == passive)
assert(not sl:queue_set_executable(id, true))
assert(sl:set_queue_policy(function() return {executable = "invalid"} end))
suspended("policy-8> ")
assert(sl:buffer() == "/bad" and sl:queue_count() == 0)
assert(sl:set_queue_policy(policy))
text = sl:next_prompt("policy-9> ")
assert(text == "/bad")
local editor
editor = coroutine.create(function()
  assert(sl:set_queue_policy(function()
    assert(coroutine.running() == editor, "policy escaped editor coroutine")
    return {enqueue = true}
  end))
  local result, origin = sl:next_prompt("policy-10> ")
  assert(result == "/coroutine" and origin == softline.PROMPT_SOURCE_QUEUED)
end)
local ok, err = coroutine.resume(editor)
assert(ok, err)
assert(sl:set_queue_policy(function() return nil end))
text, source = sl:next_prompt("policy-11> ")
assert(text == "ordinary" and source == softline.PROMPT_SOURCE_DIRECT)
assert(sl:set_queue_policy(function() error("classification failed") end))
suspended("policy-12> ")
assert(sl:buffer() == "/exception" and sl:queue_count() == 0)
assert(sl:set_queue_policy(nil))
text, source = sl:next_prompt("policy-13> ")
assert(text == "/exception" and source == softline.PROMPT_SOURCE_DIRECT)
assert(sl:set_queue_policy(function()
  return setmetatable({enqueue = true}, {__index = function()
    error("policy fields must not invoke metamethods outside the callback")
  end})
end))
text, source = sl:next_prompt("policy-14> ")
assert(text == "/metadata" and source == softline.PROMPT_SOURCE_QUEUED)
local prompt_index = 14
for _, profile in ipairs({"default", "queued_turns"}) do
  assert(sl:set_queue_profile(profile))
  for _, search in ipairs({false, true}) do
    for _, executable in ipairs({false, true}) do
      local expected = executable and "/command callback" or "/hold callback"
      local submitted, classified = false, 0
      local before_history = history
      if search then assert(sl:history_add(expected)) end
      assert(sl:set_queue_policy(function(value, mode)
        classified = classified + 1
        assert(value == expected and mode == softline.QUEUE_MODE_QUEUED)
        return {enqueue = true, executable = executable}
      end))
      assert(sl:set_idle_callback(function()
        if not submitted and sl:buffer() == expected then
          submitted = true
          assert(sl:submit())
        elseif submitted and sl:queue_count() == 1 then
          assert(sl:cancel())
        end
      end))
      prompt_index = prompt_index + 1
      text, source, id = sl:next_prompt("policy-" .. prompt_index .. "> ")
      assert(submitted and classified == 1 and history == before_history + 1)
      assert(sl:set_idle_callback(nil))
      if executable then
        assert(text == expected and source == softline.PROMPT_SOURCE_QUEUED and id > 0)
      else
        assert(text == nil and sl:last_readline_status() == softline.READLINE_CANCELLED)
        local pending = assert(sl:queue_info(1))
        assert(pending.mode == softline.QUEUE_MODE_QUEUED and not pending.eligible)
        assert(not pending.policy.executable and not pending.held)
        assert(sl:queue_set_executable(pending.id, true))
        assert(not sl:queue_info(1).eligible, "cancellation stop was bypassed")
        assert(sl:queue_resume())
        text, source, id = sl:next_prompt("unused> ")
        assert(text == expected and source == softline.PROMPT_SOURCE_QUEUED and id == pending.id)
      end
      assert(classified == 1 and history == before_history + 1)
    end
  end
end
assert(sl:set_queue_profile("default"))
for _, api in ipairs({false, true}) do
  local classified = 0
  assert(sl:set_queue_policy(function()
    classified = classified + 1
    return {enqueue = true, executable = false}
  end))
  prompt_index = prompt_index + 1
  suspended("policy-" .. prompt_index .. "> ")
  local pending = assert(sl:queue_info(1))
  assert(not pending.eligible)
  local before_history = history
  assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte("r"), function()
    assert(sl:queue_draft_info().id == pending.id)
    assert(sl:queue_set_executable(pending.id, true))
    if api then assert(sl:queue_draft()) end
    return softline.KEY_ACTION_HANDLED
  end))
  assert(sl:set_idle_callback(function()
    if sl:queue_count() > 0 then assert(sl:cancel()) end
  end))
  prompt_index = prompt_index + 1
  text, source, id = sl:next_prompt("policy-" .. prompt_index .. "> ")
  assert(text == "/hold requeue" and source == softline.PROMPT_SOURCE_QUEUED and id == pending.id)
  assert(classified == 1 and history == before_history)
  assert(sl:set_idle_callback(nil))
end
assert(sl:set_queue_policy(nil))
assert(sl:set_queue_delivery("manual"))
local older = assert(sl:queue_append("older", false))
assert(sl:queue_set_executable(older, false))
local newer = assert(sl:queue_append("newer", false))
assert(not sl:queue_info(2).eligible)
suspended("policy-27> ")
assert(sl:queue_count() == 2 and sl:queue_info(1).id == older and sl:queue_info(2).id == newer)
assert(not sl:queue_info(2).eligible and not sl:queue_take(2))
assert(sl:queue_set_executable(older, true))
text, id = sl:queue_take(1)
assert(text == "older" and id == older)
text, id = sl:queue_take(1)
assert(text == "newerX" and id == newer)
assert(sl:set_queue_delivery("auto"))
assert(sl:set_queue_policy(function() return nil end))
older = assert(sl:queue_append("steer", false))
assert(sl:queue_set_mode(1, softline.QUEUE_MODE_STEER))
newer = assert(sl:queue_append("ordinary", false))
assert(not sl:queue_info(2).eligible)
local removed = false
assert(sl:set_idle_callback(function()
  if not removed then
    assert(sl:set_queue_policy(nil))
    removed = true
  else
    assert(sl:cancel())
  end
end))
text, source, id = sl:next_prompt("policy-28> ")
assert(removed and text == "ordinary" and source == softline.PROMPT_SOURCE_QUEUED and id == newer)
assert(sl:queue_count() == 1 and sl:queue_info(1).id == older)
assert(sl:set_idle_callback(nil))
assert(sl:queue_clear())
assert(sl:set_queue_delivery("manual"))
prompt_index = 28
for _, held in ipairs({false, true}) do
  for _, steering in ipairs({false, true}) do
    for _, api in ipairs({false, true}) do
      local removed = assert(sl:queue_append(api and "first\nsecond" or "remove", held))
      assert(sl:queue_set_mode(1, steering and softline.QUEUE_MODE_STEER or softline.QUEUE_MODE_QUEUED))
      assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte("r"), function()
        assert(sl:set_buffer(""))
        assert(sl:queue_draft())
        return softline.KEY_ACTION_HANDLED
      end))
      local before_history = history
      prompt_index = prompt_index + 1
      suspended("policy-" .. prompt_index .. "> ")
      assert(sl:queue_count() == 0 and sl:queue_draft_info() == nil)
      assert(sl:buffer() == "" and sl:last_prompt_id() == 0)
      assert(not sl:queue_reject(removed) and not sl:queue_approve(removed))
      assert(history == before_history)
    end
  end
end
local cancelled = assert(sl:inject_queued_prompt("cancelled edit"))
text = sl:next_prompt("policy-37> ")
assert(text == nil and sl:last_readline_status() == softline.READLINE_CANCELLED)
assert(sl:queue_info(1).id == cancelled and sl:queue_info(1).held)
assert(sl:queue_peek(1) == "cancelled edit")
assert(sl:queue_clear())
prompt_index = 37
for _, held in ipairs({false, true}) do
  for _, steering in ipairs({false, true}) do
    assert(sl:set_buffer(""))
    local removed = assert(sl:queue_append("discard\nwithout erasing", held))
    assert(sl:queue_set_mode(1, steering and softline.QUEUE_MODE_STEER or softline.QUEUE_MODE_QUEUED))
    local before_history = history
    prompt_index = prompt_index + 1
    suspended("policy-" .. prompt_index .. "> ")
    assert(sl:queue_count() == 0 and sl:queue_draft_info() == nil)
    assert(sl:buffer() == "discard\nwithout erasing" and sl:last_prompt_id() == 0)
    assert(not sl:queue_reject(removed) and not sl:queue_approve(removed))
    assert(history == before_history)
  end
end
local before_stash_history = history
suspended("policy-42> ")
assert(sl:buffer() == "prefix discard\nwithout erasing" and sl:queue_draft_info() == nil)
suspended("policy-43> ")
assert(sl:buffer() == "prefix discard\nwithout erasing" and sl:queue_count() == 0)
suspended("policy-44> ")
assert(sl:buffer() == "discard\nwithout erasing" and sl:queue_count() == 0)
for _, letter in ipairs({"k", "j", "n"}) do
  assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte(letter), function()
    return softline.KEY_ACTION_HANDLED
  end))
end
suspended("policy-45> ")
assert(sl:buffer() == "discard\nwithout erasing" and history == before_stash_history)
sl:close()
print("lua queue policy passed")
