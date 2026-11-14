local softline=require("softline")
local output=assert(io.open("/proc/self/fd/"..assert(arg[1]),"w"))
local bytes,hash=0,2166136261
local ended,aborted,paused,id,phase=false,false,false,nil,0
local retried=false
local finished=false
local context={}
local owner=coroutine.running()
local creator=coroutine.create(function()
  return assert(softline.new {image_paste_max_bytes=0,
    image_paste_context=context,
    image_paste_callback=function(ctx,event)
      assert(ctx==context and coroutine.running()==owner)
      id=event.id
      if event.operation==softline.IMAGE_PASTE_DATA then
        assert(event.offset==bytes and #event.data==event.length)
        if not retried then
          retried=true;paused=true
          return softline.ERROR_NOT_READY
        end
        for i=1,#event.data do hash=((hash ~ event.data:byte(i))*16777619)&0xffffffff end
        bytes=bytes+#event.data
      elseif event.operation==softline.IMAGE_PASTE_END then ended=true
      elseif event.operation==softline.IMAGE_PASTE_ABORT then aborted=true
      elseif event.operation==softline.IMAGE_PASTE_DONE then
        assert(ended and not aborted and not finished);finished=true
      end
      return true
    end})
end)
local ok,editor=coroutine.resume(creator)
assert(ok and editor)
creator=nil;collectgarbage("collect");collectgarbage("collect")
assert(editor:set_idle_callback(function()
  if paused then paused=false;assert(editor:image_paste_resume(id)) end
  if aborted then assert(editor:submit());return end
  if ended and phase==0 then
    assert(not editor:buffer():find("/backend/",1,true))
    assert(editor:image_paste_complete(id,"/backend/image"));phase=1
  elseif phase==1 and editor:buffer():find("/backend/image",1,true) then
    assert(editor:submit())
  end
end))
local line=assert(editor:readline("> "))
assert(finished or aborted)
local diagnostic=aborted and (editor:last_error() or "failed") or (bytes..":"..hash)
assert(output:write(line,"\n",diagnostic,"\n"));output:close();editor:close()
