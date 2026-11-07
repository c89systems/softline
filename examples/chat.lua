local softline = require("softline")

local theme_name = os.getenv("SOFTLINE_PROMPT_THEME") or "default"
local themes = {
  default = softline.PROMPT_THEME_DEFAULT,
  plain = softline.PROMPT_THEME_PLAIN,
  accent = softline.PROMPT_THEME_ACCENT,
  dracula = softline.PROMPT_THEME_DRACULA,
  gruvbox = softline.PROMPT_THEME_GRUVBOX,
  monochrome = softline.PROMPT_THEME_MONOCHROME,
  monogreen = softline.PROMPT_THEME_MONOGREEN,
  outrun = softline.PROMPT_THEME_OUTRUN,
  riced = softline.PROMPT_THEME_RICED,
  synthwave = softline.PROMPT_THEME_SYNTHWAVE,
}

local function is_interactive_terminal()
  local ok, _, code = os.execute("test -t 0 && test -t 1")
  return ok == true or ok == 0 or code == 0
end

local interactive = is_interactive_terminal()
local sl
local busy = false
local exit_requested = false
local worker
local worker_pid
local watch_id
local dispatch_next_queued
local stream_open = false
local terminal_unavailable = false

local function is_shell_command(line)
  return line == "!sh" or line == "/shell"
end

local function command_named(line, name)
  return line == name or line:sub(1, #name + 1) == name .. " "
end

local function is_local_command(line)
  return line == "/status" or line == "/resume" or command_named(line, "/approve")
end

local function command_ready(line, mode, at_seam)
  return is_local_command(line) or not busy or
      (at_seam and mode == softline.QUEUE_MODE_STEER and not is_shell_command(line))
end

local function sync_queue_readiness(at_seam)
  local draft = sl:queue_draft_info()
  if draft then
    assert(sl:queue_set_executable(draft.id, not busy or draft.policy.independent))
  end
  for index = 1, sl:queue_count() do
    local info = assert(sl:queue_info(index))
    local text = assert(sl:queue_peek(index))
    assert(sl:queue_set_executable(info.id, command_ready(text, info.mode, at_seam)))
  end
end

local function command_message(message)
  assert(sl:set_status_message_prefix(message and "! " or ""))
  assert(sl:set_status_message(message or " "))
  assert(sl:output_stream_reconcile())
end

local function handle_command(line)
  if line == "/status" then
    command_message(string.format("Status: %s / backlog %s / %d queued / auto-approval %s",
        busy and "busy" or "idle", sl:queue_stopped() and "paused" or "running",
        sl:queue_count(), sl:get_auto_approval() and "on" or "off"))
  elseif line == "/resume" then
    assert(sl:queue_resume())
    command_message("Backlog resumed.")
  elseif command_named(line, "/approve") then
    local enabled = sl:get_auto_approval()
    if line == "/approve" then enabled = not enabled
    elseif line == "/approve on" then enabled = true
    elseif line == "/approve off" then enabled = false
    else command_message("Usage: /approve [on|off]"); return true end
    assert(sl:set_auto_approval(enabled))
    command_message(enabled and "Auto-approval ON (all waiting requests approved)."
        or "Auto-approval OFF (new requests need approval).")
  elseif command_named(line, "/steer") then
    assert(sl:output_stream_write_quoted_prompt(line))
    assert(sl:output_stream_write("Steering applied.\n"))
  else
    return false
  end
  return true
end

-- The local PTY adapter uses the same relay as remote adapters. Softline owns
-- terminal release/restoration and preserves the suspended editor.
local function run_shell(preserve_draft)
  stream_open = false
  local ok, status, escaped = softline.terminal_local(sl)
  stream_open = true
  local message
  if not ok then
    message = "shell handoff failed (" .. tostring(status) .. ")\n"
  elseif status > 0 and not escaped then
    message = "shell exited unsuccessfully (exit " .. tostring(status) .. ")\n"
  end
  if message and not sl:output_stream_write(message) then
    terminal_unavailable = true
    error("terminal output unavailable")
  end
  if not preserve_draft then
    assert(sl:set_status_message_prefix(""))
    assert(sl:set_status_message(" "))
  end
end

local function operation_step_seconds()
  local value = tonumber(os.getenv("SOFTLINE_CHAT_OPERATION_STEP_MS"))
  if value and value >= 0 and value <= 60000 then
    return value / 1000
  end
  return 1
end

local function print_message(parts)
  for _, part in ipairs(parts) do
    local ok, status = sl:output_stream_write(part)
    if not ok then
      error(sl:last_error() or ("output_stream_write failed: " .. tostring(status)))
    end
  end
end

local function set_chat_busy(value)
  assert(sl:set_status_spinner(value))
  assert(sl:set_status_busy(value))
  assert(sl:set_status_message_prefix(value and "! " or ""))
  assert(sl:set_status_message(value and "Thinking..." or " "))
  busy = value
  sync_queue_readiness(false)
end

local function consume_active_operation_input(line)
  print_message({ "[active operation] consumed: ", line, "\n" })
end

local function consume_steers()
  sync_queue_readiness(true)
  local index = 1
  while index <= sl:queue_count() do
    local info = assert(sl:queue_info(index))
    if info.mode == softline.QUEUE_MODE_STEER and info.eligible then
      local line = assert(sl:queue_take(index))
      if line == "/quit" then
        exit_requested = true
        assert(sl:cancel())
        return
      end
      if not handle_command(line) then
        consume_active_operation_input(line)
      end
    else
      index = index + 1
    end
  end
  sync_queue_readiness(false)
end

local function finish_operation()
  if watch_id then
    assert(sl:watch_remove(watch_id))
    watch_id = nil
  end
  if worker then
    assert(worker:close())
    worker = nil
  end
  worker_pid = nil
  consume_steers()
  set_chat_busy(false)
  if not exit_requested then
    dispatch_next_queued()
  end
end

-- Escape and Ctrl-C arrive here as READLINE_CANCELLED. The worker belongs to
-- the example application, so cancellation stops it instead of only clearing
-- the editor.
local function cancel_operation()
  if watch_id then
    assert(sl:watch_remove(watch_id))
    watch_id = nil
  end
  if worker_pid then
    os.execute("kill -TERM " .. worker_pid .. " >/dev/null 2>&1")
  end
  if worker then
    worker:close()
    worker = nil
  end
  worker_pid = nil
  set_chat_busy(false)
end

local function start_operation()
  if busy then
    return
  end
  local delay = string.format("%.3f", operation_step_seconds())
  local gate = os.getenv("SOFTLINE_CHAT_GATE_FD")
  local gate_command = ""
  if gate then
    local fd = tonumber(gate)
    assert(fd and fd % 1 == 0 and fd >= 3 and fd <= 9,
        "SOFTLINE_CHAT_GATE_FD must be an inherited FD 3..9")
    gate_command = "IFS= read -r gate <&" .. string.format("%d", fd) .. " || exit 1; "
  end
  worker = assert(io.popen("exec sh -c 'echo $$; " .. gate_command .. "printf P; sleep " .. delay ..
      "; printf L; sleep " .. delay .. "; printf W; sleep " .. delay ..
      "; printf C; sleep " .. delay .. "; printf R'", "r"))
  worker_pid = assert(tonumber(worker:read("*l")), "operation PID missing")
  set_chat_busy(true)
  watch_id = assert(sl:watch_add(worker,
      softline.WATCH_READ | softline.WATCH_HANGUP | softline.WATCH_ERROR,
      function()
        local event = worker:read(1)
        if event == "P" then
          print_message({ "[operation] processing input.\n" })
        elseif event == "L" then
          print_message({ "[operation] planning next steps.\n" })
        elseif event == "W" then
          print_message({ "[operation] running work.\n" })
        elseif event == "C" then
          print_message({ "[operation] checking result.\n" })
        elseif event == "R" then
          print_message({ "[operation] produced a result.\n" })
        elseif event == nil then
          finish_operation()
        end
        if event ~= nil then
          consume_steers()
        end
      end))
end

dispatch_next_queued = function()
  local index = 1
  while index <= sl:queue_count() do
    local info = assert(sl:queue_info(index))
    if info.eligible and not info.policy.enqueue then
      local line = assert(sl:queue_take(index))
      if line == "/quit" then
        exit_requested = true
        assert(sl:cancel())
        return
      end
      print_message({ "[queued] ", line, "\n" })
      start_operation()
      return
    else
      index = index + 1
    end
  end
end

local function run()
  sl = softline.new()
  assert(dofile((arg[0]:match("^(.*[/])") or "") .. "completion.lua")(sl, true))
  assert(sl:history_open("softline.examples.chat", os.getenv("SOFTLINE_HISTORY_DIR")))
  assert(sl:history_set_auto_add(true))
  if interactive then
    assert(themes[theme_name], "invalid SOFTLINE_PROMPT_THEME: " .. theme_name)
    assert(sl:set_prompt_theme(themes[theme_name]))
    assert(sl:set_prompt_queue(true, 64, 3))
    assert(sl:set_queue_profile("queued_turns"))
    assert(sl:set_queue_delivery("auto"))
    assert(sl:set_queue_policy(function(text, mode)
      return {enqueue = text:sub(1, 1) == "/" or text:sub(1, 1) == "!",
          independent = is_local_command(text), executable = command_ready(text, mode, false)}
    end))
    assert(sl:set_statusline(true, 0))
    assert(sl:set_status_elements({
      "gpt-5.6-terra high",
      "ctx 36%",
      "demo/project",
      "weekly 56%",
      "queue demo",
      "turn processor",
    }))
    assert(sl:bind_key(softline.KEY_CTRL_C, function()
      return softline.KEY_ACTION_CANCEL
    end))
    assert(sl:bind_key(softline.KEY_ESCAPE, function()
      return softline.KEY_ACTION_CANCEL
    end))
    assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte("q"), function()
      exit_requested = true
      return softline.KEY_ACTION_SUSPEND
    end))
    assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte("s"), function()
      if busy then
        assert(sl:set_status_message("Shell is available only between turns."))
        return softline.KEY_ACTION_HANDLED
      end
      return softline.KEY_ACTION_SUSPEND
    end))
    set_chat_busy(false)
    assert(sl:output_stream_begin())
    stream_open = true
    print_message({ "softline Lua turn processor. A staged operation streams for about four seconds. Enter sends while available and queues while an operation is running; Alt-Enter queues a steer for the next stage boundary; empty Alt-Enter marks the newest queued turn as steer; Alt-E edits it. Escape or Ctrl-C stops the operation; /quit or Alt-Q leaves. !sh or /shell opens your shell between turns; Alt-S opens it while preserving your draft.\n" })
    print_message({ "Policy demo: /status / /approve / /steer / /resume. Shell commands queue until the turn completes.\n" })
    print_message({ "Completion demo: type h for a dim suggestion; Tab accepts, more Tabs cycle. Backspace restarts matching. Try say ver + Tab on any line; Ctrl-J adds a line; /q + Tab completes /quit.\n" })
    print_message({ "/ resolves commands; @ lists one directory at a time (except .git). Tab cycles; Right/Ctrl-F commits and previews the next list. Enter or a separating space removes @.\n" })
  else
    assert(sl:output_stream_begin())
    stream_open = true
  end

  while true do
    local line, source_or_status = sl:next_prompt()
    if exit_requested then
      break
    end
    if line then
      if line == "/quit" then
        break
      end
      if is_shell_command(line) then
        if not interactive then
          print_message({ "Shell requires an interactive terminal.\n" })
        elseif busy then
          print_message({ "Shell is available only between turns.\n" })
        else
          assert(sl:output_stream_write_quoted_prompt(line))
          run_shell()
        end
      elseif interactive and handle_command(line) then
        -- Native delivery retained the identity, editing and approval gates.
      elseif line == "" then
        print_message({ "[empty turn ignored]\n" })
      else
        local prefix = source_or_status == softline.PROMPT_SOURCE_PROMOTED
            and "[promoted] " or source_or_status == softline.PROMPT_SOURCE_QUEUED
            and "[queued] " or "[turn] "
        print_message({ prefix, line, "\n" })
        if interactive and busy then
          consume_active_operation_input(line)
        elseif interactive then
          start_operation()
        end
      end
    elseif source_or_status == softline.READLINE_SUSPENDED then
      run_shell(true)
    elseif source_or_status == softline.READLINE_CANCELLED or source_or_status == softline.READLINE_INTERRUPTED then
      if interactive then
        if busy then
          cancel_operation()
          print_message({ "[operation] cancelled\n" })
        else
          print_message({ "[cancelled]\n" })
        end
      end
    elseif source_or_status == softline.READLINE_EOF then
      break
    else
      error(sl:last_error() or ("readline failed: " .. tostring(source_or_status)))
    end
  end
end

local ok, err = pcall(run)
if sl then
  if worker then
    pcall(cancel_operation)
  end
  if stream_open then
    sl:output_stream_end()
  end
  sl:close()
end
if not ok then
  if not terminal_unavailable then io.stderr:write(err, "\n") end
  os.exit(1)
end
