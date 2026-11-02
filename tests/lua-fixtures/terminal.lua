local softline = require("softline")
local sl = assert(softline.new({prompt_queue = true}))
assert(sl:set_queue_delivery("manual"))
local id = assert(sl:inject_queued_prompt("held approval"))
local function escape(sl_, key)
  return softline.KEY_ACTION_SUSPEND
end
assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte("s"), escape))
print("TERMINAL-EDIT")
local text, status = sl:next_prompt("terminal> ")
assert(text == nil and status == softline.READLINE_SUSPENDED)
local held = assert(sl:queue_draft_info())
assert(held.id == id and held.held)
local draft, cursor = sl:buffer(), sl:cursor()
local closed = 0
local ok, escaped = softline.terminal_handoff(sl, function(event)
  if event.operation == softline.TERMINAL_OPEN then
    assert(not pcall(function() sl:set_buffer("bad") end))
    assert(not pcall(function() sl:close() end))
    io.write("TERMINAL-READY\r\n")
    io.flush()
  elseif event.operation == softline.TERMINAL_STEP then
    return {ready = false}
  elseif event.operation == softline.TERMINAL_CLOSE then
    closed = closed + 1
  end
  return {}
end)
assert(ok and escaped and closed == 1)
assert(sl:buffer() == draft and sl:cursor() == cursor)
assert(sl:queue_draft_info().id == id and sl:queue_draft_info().held)
for _, failure in ipairs({"throw", "invalid", "close"}) do
  closed = 0
  local succeeded = pcall(function()
    local success = softline.terminal_handoff(sl, function(event)
      if event.operation == softline.TERMINAL_CLOSE then
        closed = closed + 1
        if failure == "close" then error("close failure") end
      elseif failure == "throw" then
        error("opening failure")
      elseif failure == "invalid" then
        return {status = softline.OK + 1}
      elseif event.operation == softline.TERMINAL_STEP then
        return {ready = true}
      elseif event.operation == softline.TERMINAL_READ then
        return {data = ""}
      end
      return {}
    end)
    if failure == "invalid" then assert(not success) end
  end)
  assert((failure == "invalid") == succeeded and closed == 1)
  assert(sl:buffer() == draft and sl:cursor() == cursor)
end
print("TERMINAL-RESUMED")
text, status = sl:next_prompt("terminal> ")
assert(text == nil and status == softline.READLINE_SUSPENDED)
assert(sl:buffer() == draft and sl:queue_draft_info().id == id)
sl:close()
print("lua terminal relay passed")
