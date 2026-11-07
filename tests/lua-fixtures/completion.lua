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
local roots, expanded = 0, {}
local function more(prefix)
  expanded[prefix] = (expanded[prefix] or 0) + 1
  local ok, code = sl:set_completion_providers(nil)
  assert(not ok and code == softline.ERROR_NOT_READY)
  if prefix == "@lua/" then return {"@lua/a.txt", "@lua/sub/"} end
  if prefix == "@lua/sub/" then return {"@lua/sub/deep.txt"} end
  return {}
end
assert(not pcall(function()
  sl:set_completion_providers({{trigger="@", complete=function() return {} end, expand=true}})
end))
assert(sl:set_completion_providers({{trigger="@", strip_trigger=true,
  complete=function() roots=roots+1; return {"@lua/", "@lib/"} end, expand=more}}))
assert(sl:next_prompt("expand-1> ") == "lua/a.txt")
assert(roots == 1 and expanded["@lua/"] == 1)
assert(sl:next_prompt("expand-2> ") == "lua/sub/deep.txt")
assert(roots == 2 and expanded["@lua/"] == 2 and expanded["@lua/sub/"] == 1)
assert(sl:next_prompt("expand-3> ") == "lib/")
assert(roots == 3 and expanded["@lua/"] == 3)
assert(sl:set_completion_providers({{trigger="@", strip_trigger=true,
  complete=function() return {"@lua/"} end, expand=more}}))
local expanding = coroutine.create(function()
  assert(sl:next_prompt("expand-4> ") == "lua/a.txt")
end)
assert(coroutine.resume(expanding))
assert(coroutine.status(expanding) == "dead")
for i, expected in ipairs({"lua/", "lua/a.txt", "lua/sub/deep.txt"}) do
  local count = 0
  assert(sl:set_completion_providers({{trigger="@", strip_trigger=true,
    complete=function() return {"@lua/", "@lib/"} end,
    expand=function(prefix) count=count+1; return more(prefix) end}}))
  assert(sl:next_prompt("commit-" .. i .. "> ") == expected)
  assert(count == i - 1)
end
local leaf_calls = 0
assert(sl:set_completion_providers({{trigger="/",
  complete=function() return {"/status"} end,
  expand=function(prefix)
    assert(prefix == "/status")
    leaf_calls = leaf_calls + 1
    return {}
  end}}))
for i = 1, 2 do
  assert(sl:next_prompt("leaf-" .. i .. "> ") == "/status !later")
  assert(leaf_calls == i)
end
local attempts = 0
assert(sl:set_completion_providers({{trigger="/",
  complete=function() return {"/ap", "/apple"} end,
  expand=function(prefix) attempts=attempts+1; return {prefix .. " extra"} end}}))
assert(sl:next_prompt("expand-overlap-1> ") == "/apple")
assert(attempts == 0)
assert(sl:next_prompt("expand-overlap-2> ") == "/apple")
assert(attempts == 1)
attempts = 0
assert(sl:set_completion_providers({{trigger="/",
  complete=function() return {"/root/"} end,
  expand=function(prefix)
    attempts=attempts+1
    if prefix == "/root/" then return {"/root/a", "/root/apple"} end
    return {prefix .. " extra"}
  end}}))
assert(sl:next_prompt("expand-overlap-3> ") == "/root/apple")
assert(attempts == 1)
for i, resolve in ipairs({
  function() return {"@lua/good", "@wrong"} end,
  function() return {"@lua/bad\0text"} end,
  function() error("continuation unavailable") end,
  function() return false end,
}) do
  local attempts = 0
  assert(sl:set_completion_providers({{trigger="@", strip_trigger=true,
    complete=function() return {"@lua/", "@lib/"} end,
    expand=function(prefix) attempts=attempts+1; return resolve(prefix) end}}))
  assert(sl:next_prompt("expand-error-" .. i .. "> ") == "lib/")
  assert(attempts == 1)
end
sl:close()
for mode = 0, 1 do
  local editor = assert(softline.new({prompt_theme=softline.PROMPT_THEME_PLAIN}))
  local resolutions = 0
  assert(editor:set_completion_providers({{trigger="@", strip_trigger=true,
    complete=function() resolutions=resolutions+1; return {"@alpha", "@amber"} end}}))
  assert(editor:bind_key(softline.KEY_ALT_BASE + string.byte("s"), function()
    if mode == 1 then
      assert(editor:suspend())
      return softline.KEY_ACTION_HANDLED
    end
    return softline.KEY_ACTION_SUSPEND
  end))
  if mode == 1 then assert(editor:output_stream_begin()) end
  local line, status = editor:readline("suspend-completion-" .. mode .. "> ")
  assert(line == nil and status == softline.READLINE_SUSPENDED)
  assert(editor:buffer() == "@alpha" and editor:cursor() == 6 and resolutions == 1)
  if mode == 1 then
    assert(editor:output_stream_end())
    assert(editor:output_stream_begin())
  end
  assert(editor:readline("resume-completion-" .. mode .. "> ") == "amber")
  assert(resolutions == 1)
  editor:close()
end
print("lua completion passed")
