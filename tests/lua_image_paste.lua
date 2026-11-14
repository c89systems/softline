local softline = require("softline")
local callback = function() return true end
for _, key in ipairs {"image_paste_max_bytes", "image_paste_source_timeout_ms",
                      "image_paste_host_timeout_ms", "image_paste_total_timeout_ms"} do
  for _, value in ipairs {-1, 2147483648, 1.5} do
    assert(not pcall(softline.new, {[key] = value}))
  end
end
assert(not pcall(softline.new, {image_paste_context = {}}))
assert(not pcall(softline.new, {image_paste_callback = "bad"}))
local plain = assert(softline.new())
assert(select(2, plain:image_paste_start()) == softline.ERROR_INVALID)
plain:close()

local editor, caller, aborts, expected
local context = {}
local creator = coroutine.create(function()
  return assert(softline.new {image_paste_context=context,
    image_paste_callback=function(ctx,event)
      assert(ctx == context and coroutine.running() == caller)
      assert(event.operation == softline.IMAGE_PASTE_ABORT)
      assert(event.id == expected and event.offset == 0 and event.reason)
      assert(not pcall(editor.insert,editor,"bad"))
      assert(not pcall(editor.close,editor))
      aborts = aborts + 1
      return true
    end})
end)
local ok
ok, editor = coroutine.resume(creator)
assert(ok and editor)
creator=nil
collectgarbage("collect"); collectgarbage("collect")
local owner=coroutine.create(function()
  caller=coroutine.running(); aborts=0
  assert(editor:set_buffer("keep"))
  expected=assert(editor:image_paste_start())
  assert(select(2,editor:image_paste_complete(expected,"/early"))==softline.ERROR_INVALID)
  assert(editor:image_paste_pump()) -- DISPLAY is intentionally unavailable.
  assert(aborts==1 and editor:buffer()=="keep")
  assert(select(2,editor:image_paste_cancel(expected))==softline.ERROR_INVALID)
  local wait=assert(editor:image_paste_prepare())
  assert(#wait.fds==0 and wait.timeout_ms==-1)
  expected=assert(editor:image_paste_start())
  assert(editor:image_paste_cancel(expected) and aborts==2)
  editor:close()
end)
local resumed,message=coroutine.resume(owner)
assert(resumed,message)

local weak=setmetatable({}, {__mode="v"})
local function cycle()
  local retained
  retained=assert(softline.new {image_paste_callback=function()
    assert(retained)
    return true
  end})
  weak[1]=retained
end
cycle()
collectgarbage("collect");collectgarbage("collect")
assert(weak[1]==nil,"image callback cycle rooted the editor")
print("lua image paste passed")
