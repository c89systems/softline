local softline = require("softline")
local sl = assert(softline.new({prompt_queue = true, prompt_theme = softline.PROMPT_THEME_PLAIN}))
local function configure()
  return sl:set_completions({
    "/approve on", "/approve off", "/apple", "/apply", "/appraise",
    {text = "verification", scope = softline.COMPLETION_WORD},
    {text = "next steps", scope = softline.COMPLETION_WORD},
  })
end
assert(configure())
assert(not sl:set_completions({"bad\27[31m"}))
assert(not pcall(function() sl:set_completions({"bad\0text"}) end))
assert(not pcall(function() sl:set_completions({{text="fine", scope=99}}) end))
assert(sl:set_status_message("Reasoning remains host owned."))
assert(sl:set_statusline(true, 0))
assert(sl:set_status_elements({"completion demo"}))
assert(sl:set_queue_delivery("manual"))
assert(sl:queue_append("queued stays visible"))
local held = assert(sl:inject_queued_prompt("held stays visible"))
local calls = 0
assert(sl:bind_key(softline.KEY_TAB, function()
  calls = calls + 1
  assert(sl:insert("#"))
  return softline.KEY_ACTION_HANDLED
end))
assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte("s"), function()
  return softline.KEY_ACTION_SUSPEND
end))
local line, status = sl:next_prompt("complete> ")
assert(line == nil and status == softline.READLINE_SUSPENDED)
assert(sl:buffer() == "/approve on#" and calls == 1)
assert(sl:queue_count() == 2 and sl:queue_info(2).id == held and sl:queue_info(2).held)
assert(sl:set_buffer("say neX later"))
assert(sl:set_cursor(6))
assert(sl:next_prompt("inside> ") == nil)
assert(sl:buffer() == "say next steps later")
assert(sl:set_completions({"/status"}))
assert(sl:set_buffer("/st"))
assert(sl:bind_key(softline.KEY_CTRL_T, function()
  local text, cursor = sl:buffer(), sl:cursor()
  assert(sl:set_completions({"/stop"}))
  assert(sl:buffer() == text and sl:cursor() == cursor)
  return softline.KEY_ACTION_HANDLED
end))
assert(sl:next_prompt("update> ") == nil)
assert(sl:buffer() == "/stop")
assert(sl:set_completions(nil))
assert(sl:set_buffer("/app"))
assert(sl:next_prompt("cleared> ") == nil)
assert(sl:buffer() == "/app#" and calls == 2)
sl:close()
print("lua completion passed")
