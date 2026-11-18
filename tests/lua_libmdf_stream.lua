package.path = assert(arg[1], "prepared libmdf module directory missing") .. "/?.lua;" .. package.path
package.cpath = arg[1] .. "/?.so;" .. package.cpath
local softline = require("softline")
local mdf = require("libmdf")

local sl, renderer, writes = nil, nil, 0
-- Construct both handles on a coroutine that is collected before readline.
-- Both native bindings must dispatch later callbacks on the active caller.
do
  local co = coroutine.create(function()
    sl = assert(softline.new())
    return mdf.document_stream({format = "ansi", ansi_mode = "on",
      boring = true, width = 76, margin_left = 2}, function(chunk)
        assert(type(chunk) == "string" and #chunk > 0)
        assert(type(sl:buffer()) == "string")
        assert(sl:output_stream_write(chunk))
        writes = writes + 1
        return true
      end)
  end)
  local ok
  ok, renderer = coroutine.resume(co)
  assert(ok, renderer)
end
collectgarbage("collect")
assert(sl:output_stream_begin())
local function feed(text)
  for at = 1, #text do assert(renderer:write(text:sub(at, at))) end
end
local producer = assert(io.popen("sh -c 'printf A; sleep 0.5; printf B'", "r"))
local watch, first_writes
local fence = string.rep(string.char(96), 3)
watch = assert(sl:watch_add(producer,
  softline.WATCH_READ | softline.WATCH_HANGUP | softline.WATCH_ERROR, function()
    local event = producer:read(1)
    if event == "A" then
      feed("# MDF-LUA-FIRST\n\n**Live** café output.\n\n")
      assert(writes > 0, "renderer buffered the complete document")
      first_writes = writes
    elseif event == "B" then
      assert(first_writes and sl:buffer() == "draft", "typing was blocked by rendering")
      assert(renderer:finish_document())
      assert(renderer:set_geometry(72, 2, 0))
      for _, kind in ipairs({"mdf-bar-chart", "mdf-horizontal-bar-chart",
          "mdf-vertical-bar-chart", "mdf-tile-chart"}) do
        assert(renderer:begin_document())
        feed(fence .. kind .. ",sort=desc,colored_bars=on\nStage,Value\nBuild,40\nTest,25\nReview,10\n" .. fence .. "\n")
        assert(renderer:finish_document())
      end
      assert(renderer:begin_document())
      feed("# MDF-LUA-LAST\n")
      assert(renderer:finish_document())
      assert(writes > first_writes)
    elseif event == nil then
      assert(sl:watch_remove(watch))
      assert(producer:close())
      assert(sl:output_stream_write("MDF-LUA-EOF\n"))
    else error("unexpected producer event") end
  end))
local line = assert(sl:readline("mdf> "))
assert(line == "draft")
renderer:close()
assert(sl:output_stream_end())
sl:close()
print("Lua libmdf streaming passed: " .. writes .. " sink fragments")
