local softline=require('softline')
local expected=('a\0b'):rep(1024)
local editor,owner,received,ended,done,aborts,paused
local function host(_,event)
  assert(coroutine.running()==owner,'wrong callback coroutine')
  collectgarbage('collect')
  if event.operation==softline.IMAGE_PASTE_DATA then
    assert(event.offset==received and event.data==expected and event.length==#expected)
    if paused==false then paused=true;return softline.ERROR_NOT_READY end
    received=received+#event.data
  elseif event.operation==softline.IMAGE_PASTE_END then ended=ended+1
  elseif event.operation==softline.IMAGE_PASTE_DONE then done=done+1
  elseif event.operation==softline.IMAGE_PASTE_ABORT then aborts=aborts+1 end
  return true
end
local creator=coroutine.create(function()return assert(softline.new{image_paste_callback=host})end)
local ok
ok,editor=coroutine.resume(creator);assert(ok and editor);creator=nil
collectgarbage('collect')
local worker=coroutine.create(function()
  owner=coroutine.running();received=0;ended=0;done=0;aborts=0;paused=false
  local context={step=0,offset=0,closed=0,calls=0}
  local source={context=context,
    prepare=function(ctx)
      assert(ctx==context and coroutine.running()==owner)
      assert(not pcall(editor.buffer,editor),'source callback reentered the editor')
      return{timeout_ms=0,fds={}}
    end,
    close=function(ctx)
      assert(ctx==context and coroutine.running()==owner)
      assert(not pcall(editor.close,editor),'source close destroyed editor')
      ctx.closed=ctx.closed+1
    end,
    next=function(ctx)
      assert(ctx==context and coroutine.running()==owner)
      assert(not pcall(editor.insert,editor,'bad'))
      ctx.calls=ctx.calls+1
      if ctx.step==0 then ctx.step=1;return{operation=softline.IMAGE_PASTE_BEGIN,media_type='image/png',filename='source.png'}end
      if ctx.step<=4 then
        ctx.step=ctx.step+1
        local offset=ctx.offset;ctx.offset=ctx.offset+#expected
        return setmetatable({operation=softline.IMAGE_PASTE_DATA,offset=offset},{__index=function(_,key)
          collectgarbage('collect')
          if key=='data' then return ('a\0b'):rep(1024) end
        end})
      end
      return{operation=softline.IMAGE_PASTE_END,offset=ctx.offset}
    end}
  assert(editor:set_buffer('draft '))
  local id=assert(editor:image_paste_start_source(source))
  local other={next=function()end,prepare=function()end,close=function()error('unadopted source closed')end}
  assert(select(2,editor:image_paste_start_source(other))==softline.ERROR_INVALID)
  assert(editor:image_paste_prepare().timeout_ms==0)
  assert(editor:insert('tail'))
  for _=1,20 do
    assert(editor:image_paste_pump())
    if paused==true then
      local calls=context.calls
      collectgarbage('collect')
      assert(editor:image_paste_pump() and context.calls==calls and received==0)
      assert(editor:image_paste_resume(id));paused=nil
    end
    if ended>0 then break end
  end
  assert(ended==1 and context.closed==1 and received==4*#expected)
  assert(editor:image_paste_complete(id,'/backend/image.png'))
  assert(editor:image_paste_pump() and done==1 and aborts==0)
  assert(editor:buffer()=='draft /backend/image.pngtail')
  editor:close();assert(context.closed==1)
end)
local result,message=coroutine.resume(worker);assert(result,message)

local function failure(next_callback,prepare_callback,expected_error)
  local closed,aborted,data=0,0,0
  local sl=assert(softline.new{image_paste_callback=function(_,event)
    if event.operation==softline.IMAGE_PASTE_ABORT then aborted=aborted+1 end
    if event.operation==softline.IMAGE_PASTE_DATA then data=data+1 end
    return true
  end})
  local id=assert(sl:image_paste_start_source{
    next=next_callback,prepare=prepare_callback or function()return{timeout_ms=0}end,
    close=function()closed=closed+1 end})
  if prepare_callback then
    assert(not sl:image_paste_prepare())
    if expected_error then assert(sl:last_error():find(expected_error,1,true)) end
    assert(sl:image_paste_cancel(id))
  else
    for _=1,3 do assert(sl:image_paste_pump());if aborted>0 then break end end
  end
  if expected_error and not prepare_callback then assert(sl:last_error():find(expected_error,1,true)) end
  assert(closed==1 and aborted==1 and data==0)
  sl:close();assert(closed==1)
end
failure(function()error('source exception sentinel')end,nil,'source exception sentinel')
failure(function()return nil,'source failure sentinel'end,nil,'source failure sentinel')
failure(function()return{operation=softline.IMAGE_PASTE_DATA,data='x'}end)
failure(function()end,function()return{timeout_ms=-2}end)
failure(function()end,function()return{fds={{fd=0,events=0}}}end)
failure(function()end,function()error('prepare sentinel')end,'prepare sentinel')
do
  local step=0
  failure(function()
    step=step+1
    if step==1 then return{operation=softline.IMAGE_PASTE_BEGIN,media_type='image/png'}end
    return{operation=softline.IMAGE_PASTE_DATA,data=('x'):rep(16385)}
  end)
end
do
  local closed=0
  local plain=assert(softline.new())
  local source={next=function()end,prepare=function()return{}end,close=function()closed=closed+1 end}
  assert(select(2,plain:image_paste_start_source(source))==softline.ERROR_INVALID)
  plain:close();assert(closed==0,'failed adoption took source ownership')
  local sl=assert(softline.new{image_paste_callback=function()return true end})
  local id=assert(sl:image_paste_start_source{next=function()end,prepare=function()return{}end,
    close=function()error('close sentinel')end})
  assert(sl:image_paste_cancel(id))
  assert(sl:image_paste_start_source(source))
  sl:close();assert(closed==1,'close exception retained source ownership')
end
local weak=setmetatable({},{__mode='v'})
local closed=0
do
  local sl=assert(softline.new{image_paste_callback=function()return true end})
  local source={context={editor=sl},next=function()return nil end,
    prepare=function()return{timeout_ms=-1}end,close=function()closed=closed+1 end}
  weak[1]=sl;assert(sl:image_paste_start_source(source))
end
collectgarbage('collect');collectgarbage('collect')
assert(weak[1]==nil and closed==1,'source cycle rooted the editor')
print('lua image source passed')
