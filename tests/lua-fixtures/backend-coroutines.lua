local softline = require("softline")

local function run(method, construct_on_main, collect_constructor, read_on_main, mode)
  local editor, creator, caller, observed, synchronize
  local sync_calls, offers, detached = 0, 0, 0
  local context = {}
  local function make()
    return assert(softline.new {prompt_queue = true,
      prompt_theme = softline.PROMPT_THEME_PLAIN, queue_backend_context = context,
      queue_backend = {
        capabilities = (1 << (softline.QUEUE_RESOLVE + 1)) - 1,
        request = function(request, ctx)
          assert(ctx == context and coroutine.running() == caller)
          if request.operation == softline.QUEUE_SYNCHRONIZE then
            sync_calls = sync_calls + 1
            synchronize = request.request_id
          elseif request.operation == softline.QUEUE_OFFER then
            assert(request.text == "queued from reader")
            offers = offers + 1
          else error("unexpected native queue request: " .. request.operation) end
          return true
        end,
        detach = function(ctx)
          assert(ctx == context and coroutine.running() == caller)
          detached = detached + 1
        end,
      }})
  end
  if construct_on_main then editor = make()
  else
    creator = coroutine.create(make)
    local ok
    ok, editor = coroutine.resume(creator)
    assert(ok and editor)
    if collect_constructor then
      local weak = setmetatable({creator}, {__mode="v"})
      creator = nil
      collectgarbage("collect"); collectgarbage("collect")
      assert(weak[1] == nil, "constructor coroutine remained rooted")
    end
  end
  local function read()
    caller = coroutine.running()
    local watch, file, synchronized
    if mode == "watch" then
      file = assert(io.open(arg[0], "rb"))
      watch = assert(editor:watch_add(file, softline.WATCH_READ, function()
        observed = coroutine.running()
        assert(editor:watch_remove(watch))
        assert(editor:cancel())
      end))
    else
      if mode == "key" then
        assert(editor:bind_key(softline.KEY_CTRL_T, function()
          observed = coroutine.running()
          assert(editor:insert("queued from reader"))
          return softline.KEY_ACTION_HANDLED
        end))
      end
      assert(editor:set_idle_callback(function()
        if mode == "idle" then
          observed = coroutine.running()
          assert(editor:cancel())
        elseif not synchronized then
          assert(synchronize, "native synchronization callback missing")
          assert(editor:queue_backend_update {revision="q0", entries={}, total_count=0})
          assert(editor:queue_backend_result {request_id=synchronize})
          synchronized = true
          assert(editor:print_above("backend-ready-" .. method .. "\n"))
        elseif offers == 1 then assert(editor:cancel()) end
      end))
    end
    local line, status = editor[method](editor, "backend-co> ")
    assert(sync_calls == 1, "native synchronization callbacks: " .. sync_calls)
    assert(line == nil and status == softline.READLINE_CANCELLED)
    assert(observed == caller, "native editor callback used another coroutine")
    assert(offers == (mode == "key" and 1 or 0))
    if file then file:close() end
  end
  if read_on_main then read()
  else
    local reader = coroutine.create(read)
    local ok, message = coroutine.resume(reader)
    assert(ok, message)
    assert(coroutine.status(reader) == "dead")
  end
  caller = coroutine.running()
  editor:close()
  assert(detached == 1)
end

for _, method in ipairs({"readline", "next_prompt"}) do
  -- Keep the first constructor alive so a regression fails without dereferencing
  -- a collected Lua state; subsequent cases prove collection is safe too.
  run(method, false, false, false, "idle")
  run(method, false, true, false, "idle")
  run(method, true, false, false, "idle")
  run(method, false, true, true, "idle")
  run(method, false, true, false, "key")
  run(method, false, true, false, "watch")
end
print("lua backend coroutine dispatch passed")
