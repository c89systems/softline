local softline = require("softline")
local sl = assert(softline.new({prompt_queue = true, prompt_theme = softline.PROMPT_THEME_PLAIN}))
local function configure()
  return sl:set_completion_providers({
    {trigger="/", complete=function() return {"/approve on", "/approve off", "/apple", "/apply", "/appraise"} end},
    {trigger="v", scope=softline.COMPLETION_WORD, complete=function() return {"verification"} end},
    {trigger="n", scope=softline.COMPLETION_WORD, complete=function() return {"next steps"} end},
  })
end
assert(configure())
assert(not sl:set_completion_providers({{trigger=" ", complete=function() return {} end}}))
assert(not pcall(function() sl:set_completion_providers({{trigger="/\0", complete=function() return {} end}}) end))
assert(not pcall(function() sl:set_completion_providers({{trigger="/", scope=99, complete=function() return {} end}}) end))
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
assert(sl:set_completion_providers({{trigger="/", complete=function() return {"/status"} end}}))
assert(sl:set_buffer("/st"))
assert(sl:bind_key(softline.KEY_CTRL_T, function()
  local text, cursor = sl:buffer(), sl:cursor()
  assert(sl:set_completion_providers({{trigger="/", complete=function() return {"/stop"} end}}))
  assert(sl:buffer() == text and sl:cursor() == cursor)
  return softline.KEY_ACTION_HANDLED
end))
assert(sl:next_prompt("update> ") == nil)
assert(sl:buffer() == "/stop")
assert(sl:set_completion_providers(nil))
assert(sl:set_buffer("/app"))
assert(sl:next_prompt("cleared> ") == nil)
assert(sl:buffer() == "/app#" and calls == 2)
local resolutions, fresh = 0, false
assert(sl:set_completion_providers({{trigger="@", strip_trigger=true,
  complete=function(trigger)
    assert(trigger == "@" and sl:buffer():sub(1,1) == "@")
    resolutions = resolutions + 1
    return fresh and {"@fresh"} or {"@alpha", "@beta"}
  end}}))
assert(sl:set_buffer(""))
assert(sl:bind_key(softline.KEY_CTRL_T, function()
  fresh = true
  return softline.KEY_ACTION_HANDLED
end))
assert(sl:next_prompt("provider-1> ") == "beta")
assert(resolutions == 1)
assert(sl:next_prompt("provider-2> ") == "fresh")
assert(resolutions == 2)
assert(sl:set_completion_providers({{trigger="@", strip_trigger=true,
  complete=function() return {"@bad\0text"} end}}))
assert(sl:next_prompt("provider-3> ") == "@a#")
assert(calls == 3)
assert(sl:set_completion_providers({{trigger="@", strip_trigger=true,
  complete=function() return {"@co"} end}}))
local co = coroutine.create(function()
  assert(sl:next_prompt("provider-4> ") == "co")
end)
assert(coroutine.resume(co))
assert(coroutine.status(co) == "dead")
assert(sl:set_completion_providers({{trigger="@", strip_trigger=true,
  complete=function() error("provider unavailable") end}}))
assert(sl:next_prompt("provider-5> ") == "@e#")
assert(calls == 4)
sl:close()
print("lua completion passed")
