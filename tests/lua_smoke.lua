local softline = require("softline")

local function assert_eq(actual, expected, label)
  if actual ~= expected then
    error(label .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual), 2)
  end
end

do
  local weak = setmetatable({}, {__mode="v"})
  local function make_cycle()
    local editor = assert(softline.new())
    assert(editor:set_completion_providers({{trigger="/", complete=function()
      assert(editor:buffer() ~= nil)
      return {"/quit"}
    end, expand=function()
      assert(editor:buffer() ~= nil)
      return {}
    end}}))
    return editor
  end
  weak[1] = make_cycle()
  collectgarbage("collect")
  collectgarbage("collect")
  assert(weak[1] == nil, "completion callbacks must not root their editor")
end

local prompt_methods = {
  "next_prompt",
  "set_live_scroll_region",
  "output_stream_begin",
  "output_stream_reconcile",
  "output_stream_write",
  "output_stream_write_quoted_prompt",
  "output_stream_end",
  "set_prompt_queue",
  "queue_count",
  "queue_capacity",
  "queue_peek",
  "queue_insert",
  "queue_append",
  "queue_replace",
  "queue_take",
  "queue_mode",
  "queue_info",
  "queue_draft_info",
  "queue_approve",
  "queue_reject",
  "last_prompt_id",
  "queue_set_mode",
  "queue_clear",
  "queue_draft",
  "set_queue_delivery",
  "queue_delivery",
  "set_queue_profile",
  "queue_profile",
  "set_queue_keys",
  "queue_keys",
  "set_prompt_theme",
  "set_completion_providers",
  "set_osc8_mode",
  "get_osc8_info",
  "set_quoted_prompt_prefix",
  "set_quoted_prompt_style",
  "set_statusline",
  "set_status_message",
  "set_status_message_prefix",
  "set_status_message_colors",
  "set_status_elements",
  "set_status_element",
  "set_status_busy",
  "set_status_spinner",
  "set_status_idle_marker",
}

for _, field in ipairs({ "bounded", "screen_x", "screen_y", "screen_height",
                         "unknown_option", "screen_wdith", "screen_width\0extra" }) do
  local ok, message = pcall(softline.new, { [field] = true })
  assert(not ok and message:find("unknown configuration field", 1, true),
         "unknown configuration option accepted: " .. field)
end
local ok, message = pcall(softline.new, { [1] = true })
assert(not ok and message:find("configuration field names must be strings", 1, true))

do
  local large = assert(softline.new({ prompt_queue = true }))
  local draft = string.rep("a\n", 65535) .. "é"
  assert(#draft == 131072)
  assert(large:set_buffer(draft))
  assert_eq(large:buffer(), draft, "128 KiB default draft")
  assert(not large:set_buffer(draft .. "x"))
  assert_eq(large:buffer(), draft, "oversize replacement leaves draft intact")
  local id = assert(large:queue_append(draft, true))
  assert(large:queue_approve(id))
  assert_eq(large:queue_take(1), draft, "128 KiB queue round-trip")
  large:close()
end

local sl = assert(softline.new({ line_max_len = 32 }))
assert(sl:set_image_paste_path_template("{{xdg_cache_home}}/softline/*"))
assert(sl:set_image_paste_path_template(nil))
assert(not sl:set_image_paste_path_template("relative/*"))
assert_eq(sl:last_readline_status(), softline.READLINE_NONE, "initial status")
assert(sl:history_add("history entry"))
assert(sl:set_buffer("draft"))
assert_eq(sl:buffer(), "draft", "buffer")
assert(sl:set_cursor(2))
assert_eq(sl:cursor(), 2, "cursor")
assert(sl:insert("++"))
assert_eq(sl:buffer(), "dr++aft", "insert")
assert(sl:set_buffer("å"))
assert(sl:set_cursor(1))
assert_eq(sl:cursor(), 0, "UTF-8 cursor clamp")
assert(sl:insert("x"))
assert_eq(sl:buffer(), "xå", "UTF-8 cursor insert")
assert(sl:set_idle_callback(function() end))
assert(sl:set_idle_callback(nil))
assert(sl:print_above({ "alpha", "-", "beta\n" }))
assert(sl:print_above(function(i)
  if i == 1 then
    return "gamma\n"
  end
  return nil
end))
assert(not sl:output_stream_reconcile())
assert(sl:output_stream_begin())
assert(sl:output_stream_reconcile())
assert(not sl:output_stream_begin())
assert(sl:output_stream_write("delta"))
assert(sl:output_stream_write(""))
assert(sl:output_stream_write("-echo\n"))
assert(sl:output_stream_end())
assert(not sl:output_stream_reconcile())
assert(not sl:output_stream_write("after end"))
sl:close()

local image_config = assert(softline.new({
  disable_image_paste = true,
  image_paste_path_template = "{{HOME}}/captures/*",
}))
image_config:close()
local retained_templates = {}
local collected_template = assert(softline.new(setmetatable({}, {
  __index = function(_, key)
    if key == "image_paste_path_template" then
      return "/tmp/" .. string.rep("x", 100) .. "/*"
    end
    if key == "clear_prompt_on_exit" then
      collectgarbage("collect")
      for i = 1, 50 do retained_templates[i] = string.rep("Z", 107) end
    end
  end,
})))
collected_template:close()
local image_ok = pcall(softline.new,
  { image_paste_path_template = "{{XDG_CACHE_DIR}}/*" })
assert(not image_ok, "unsupported XDG token accepted")

assert_eq(sl.set_bounds, nil, "boxed viewport method removed")

local status = assert(softline.new({
  prompt_queue = true,
  prompt_queue_max_entries = 7,
  prompt_queue_preview_entries = 2,
  prompt_theme = softline.PROMPT_THEME_RICED,
  statusline = true,
  statusline_start_element = 15,
  status_spinner = true,
  status_busy = true,
  status_idle_marker = "-",
  live_scroll_region = true,
}))
assert(status:set_statusline(true, 15))
assert(status:set_status_message("Thinking..."))
assert(status:set_status_message_prefix("? "))
assert(status:set_status_message_colors(softline.THEME_COLOR_MUTED,
                                        softline.THEME_COLOR_ELEMENT_2))
assert(status:set_status_message_prefix(""))
assert(status:set_status_message_prefix(nil))
assert(not status:set_status_message_colors(-1, softline.THEME_COLOR_MUTED))
assert(status:set_status_message("Reasoning..."))
assert(not status:set_status_message("two\nlines"))
assert(status:set_status_message(nil))
assert(status:set_live_scroll_region(true))
assert(status:set_live_scroll_region(false))
assert(status:set_status_elements({ "model", "context" }))
assert(status:set_status_element(1, "context 36%"))
assert(status:set_status_busy(false))
assert(status:set_status_spinner(false))
assert(status:set_status_idle_marker("-"))
assert(status:set_status_idle_marker(nil))
assert_eq(status:queue_count(), 0, "initial queue count")
assert_eq(status:queue_capacity(), 7, "queue capacity")
assert(status:queue_append("one"))
assert(status:queue_append("three"))
assert(status:queue_insert(2, "two"))
if string.packsize("T") < 8 then
  assert(not status:queue_take(4294967297))
  assert_eq(status:queue_peek(1), "one", "overflowing queue index changed queue")
end
assert_eq(status:queue_peek(2), "two", "queue peek")
assert_eq(status:queue_mode(2), softline.QUEUE_MODE_QUEUED, "default queue mode")
assert(status:queue_set_mode(2, softline.QUEUE_MODE_STEER))
assert_eq(status:queue_mode(2), softline.QUEUE_MODE_STEER, "steer queue mode")
assert(status:queue_replace(2, "second"))
assert_eq(status:queue_mode(2), softline.QUEUE_MODE_STEER, "replacement kept mode")
assert_eq(status:queue_take(1), "one", "queue take")
assert_eq(status:queue_count(), 2, "queue count after take")
assert(status:set_queue_delivery("manual"))
assert_eq(status:queue_delivery(), "manual", "manual queue delivery")
assert(status:set_queue_profile("queued_turns"))
assert_eq(status:queue_profile(), "queued_turns", "queued-turns profile")
assert_eq(status:queue_delivery(), "auto", "idle queued-turns delivery")
assert(status:set_status_busy(true))
assert_eq(status:queue_delivery(), "manual", "busy queued-turns delivery")
assert(status:set_status_busy(false))
assert_eq(status:queue_delivery(), "auto", "idle queued-turns delivery")
assert(status:set_queue_delivery("manual"))
assert_eq(status:queue_delivery(), "manual", "host-controlled queued turns")
assert(status:set_status_busy(true))
assert(status:set_status_busy(false))
assert_eq(status:queue_delivery(), "manual", "manual delivery survives idle")
assert(status:set_queue_delivery("auto"))
local keys = status:queue_keys()
assert_eq(keys.enqueue_draft, softline.KEY_ENTER, "queued-turns enqueue key")
assert_eq(keys.edit_newest, softline.KEY_ALT_E, "queued-turns edit key")
assert_eq(keys.submit_or_promote_newest, softline.KEY_ALT_ENTER,
  "queued-turns submit/promote key")
keys.enqueue_draft = nil
assert(status:set_queue_keys(keys))
assert(status:queue_clear())
assert_eq(status:queue_count(), 0, "queue clear")
for _, name in ipairs(prompt_methods) do
  assert_eq(type(status[name]), "function", "prompt method " .. name)
end
assert_eq(softline.PROMPT_SOURCE_NONE, 0, "no prompt source constant")
assert_eq(softline.PROMPT_SOURCE_DIRECT, 1, "direct prompt source constant")
assert_eq(softline.PROMPT_SOURCE_QUEUED, 2, "queued prompt source constant")
assert_eq(softline.PROMPT_SOURCE_PROMOTED, 3, "promoted prompt source constant")
assert_eq(softline.QUEUE_MODE_QUEUED, 0, "queued mode constant")
assert_eq(softline.QUEUE_MODE_STEER, 1, "steer mode constant")
assert_eq(softline.ERROR_FULL, -5, "queue full status constant")
assert_eq(softline.ERROR, -1, "generic error status constant")
assert_eq(softline.ERROR_INVALID, -2, "invalid argument status constant")
assert_eq(softline.ERROR_NOMEM, -3, "allocation status constant")
assert_eq(softline.ERROR_IO, -4, "I/O status constant")
assert_eq(softline.PROMPT_THEME_PLAIN, 0, "plain prompt theme constant")
assert_eq(softline.PROMPT_THEME_ACCENT, 1, "accent prompt theme constant")
assert_eq(softline.PROMPT_THEME_DRACULA, 2, "dracula prompt theme constant")
assert_eq(softline.PROMPT_THEME_GRUVBOX, 3, "gruvbox prompt theme constant")
assert_eq(softline.PROMPT_THEME_MONOCHROME, 4, "monochrome prompt theme constant")
assert_eq(softline.PROMPT_THEME_MONOGREEN, 5, "monogreen prompt theme constant")
assert_eq(softline.PROMPT_THEME_OUTRUN, 6, "outrun prompt theme constant")
assert_eq(softline.PROMPT_THEME_RICED, 7, "riced prompt theme constant")
assert_eq(softline.PROMPT_THEME_SYNTHWAVE, 8, "synthwave prompt theme constant")
assert_eq(softline.PROMPT_THEME_DEFAULT, 9, "default prompt theme constant")
assert_eq(softline.STATUS_MAX_ELEMENTS, 32, "status element limit constant")
assert_eq(softline.KEY_TAB, 9, "Tab key constant")
assert_eq(softline.KEY_CTRL_L, 12, "layout reset key constant")
assert_eq(softline.KEY_ACTION_RESET, 5, "layout reset action constant")
assert_eq(softline.KEY_ACTION_SUSPEND, 6, "editor suspend action constant")
assert_eq(softline.READLINE_SUSPENDED, 6, "suspended editor result constant")
assert_eq(softline.KEY_CTRL_T, 20, "transpose key constant")
assert_eq(softline.KEY_CTRL_X, 24, "control prefix constant")
assert_eq(softline.KEY_CTRL_Y, 25, "yank key constant")
assert_eq(softline.KEY_CTRL_UNDERSCORE, 31, "undo key constant")
assert_eq(softline.KEY_ALT_D, softline.KEY_ALT_BASE + string.byte('d'), "kill word constant")
assert_eq(softline.KEY_ALT_Y, softline.KEY_ALT_BASE + string.byte('y'), "yank pop constant")
assert_eq(softline.KEY_ALT_BACKSPACE, 1026, "backward kill word constant")
assert_eq(softline.KEY_CTRL_N, 14, "Ctrl-N key constant")
assert_eq(softline.KEY_CTRL_P, 16, "Ctrl-P key constant")
assert_eq(softline.KEY_ESCAPE, 27, "Escape key constant")
assert_eq(softline.KEY_UP, 1000, "Up key constant")
assert_eq(softline.KEY_DOWN, 1001, "Down key constant")
assert_eq(softline.KEY_CTRL_ENTER, 1022, "Ctrl-Enter key constant")
assert_eq(softline.KEY_ALT_ENTER, 1023, "Alt-Enter key constant")
assert_eq(softline.KEY_ALT_E, 4096 + string.byte("e"), "Alt-E key constant")
assert_eq(softline.KEY_ALT_A, 4096 + string.byte("a"), "Alt-A key constant")
assert_eq(softline.KEY_PAGE_UP, 1024, "Page Up key constant")
assert_eq(softline.KEY_PAGE_DOWN, 1025, "Page Down key constant")
assert_eq(softline.KEY_NONE, 0, "disabled key constant")
assert_eq(softline.KEY_CTRL_R, 18, "reverse-search key constant")
assert_eq(softline.KEY_F1, 1012, "function key constant")
assert_eq(softline.KEY_F10, 1021, "last function key constant")
assert_eq(softline.KEY_ALT_BASE, 4096, "Alt-letter base constant")
assert_eq(softline.KEY_ALT_M, 4205, "Alt-M key constant")
assert(status:queue_clear())
assert(status:set_queue_profile("default"))
assert(status:set_queue_delivery("auto"))
local held = assert(status:queue_append("held suggestion", true))
local ordinary = assert(status:queue_append("ordinary user prompt"))
assert(math.type(held) == "integer" and held > 0 and ordinary > held)
local info = assert(status:queue_info(1))
assert_eq(info.id, held, "held identity")
assert_eq(info.held, true, "per-entry hold")
assert(not status:queue_take(1))
local delivered, source, delivered_id = status:next_prompt()
assert_eq(delivered, "ordinary user prompt", "ordinary bypassed held suggestion")
assert_eq(source, softline.PROMPT_SOURCE_QUEUED, "ordinary source")
assert_eq(delivered_id, ordinary, "delivery identity")
assert_eq(status:last_prompt_id(), ordinary, "last delivery identity")
assert(status:queue_replace(1, "edited suggestion"))
assert_eq(status:queue_info(1).id, held, "replacement identity")
assert_eq(status:queue_info(1).held, true, "replacement hold")
assert(status:queue_set_mode(1, softline.QUEUE_MODE_STEER))
assert(status:queue_approve(held))
assert_eq(status:queue_mode(1), softline.QUEUE_MODE_STEER, "approval keeps steer mode")
delivered, delivered_id = status:queue_take(1)
assert_eq(delivered, "edited suggestion", "approved take text")
assert_eq(delivered_id, held, "approved take identity")
assert_eq(status:last_prompt_id(), held, "last take identity")
assert(not status:queue_approve(held))
assert(not status:queue_reject(held))
assert(not status:queue_approve(0))
assert(not status:queue_reject(-1))
assert(not status:queue_info(0))
assert(not status:queue_info(999))
assert_eq(status:queue_draft_info(), nil, "ordinary draft has no queued origin")
local rejected = assert(status:queue_append("reject me", true))
assert(status:queue_reject(rejected))
assert_eq(status:queue_count(), 0, "rejection removed entry")
assert(not status:queue_reject(rejected))
assert(status:queue_insert(1, "new", true) > rejected)
assert(status:queue_clear())
assert(not pcall(function() status:queue_append("bad hold", "yes") end))
local first_approval = assert(status:inject_queued_prompt("first approval"))
local second_approval = assert(status:inject_steer_prompt("second approval"))
local inserted_approval = assert(status:queue_insert(1, "later arrival", true))
assert(not status:queue_approve(second_approval))
assert(not status:queue_reject(inserted_approval))
assert_eq(status:queue_count(), 3, "out-of-order decisions leave queue unchanged")
assert(status:queue_approve(first_approval))
assert(not status:queue_approve(inserted_approval))
assert(status:queue_reject(second_approval))
assert(status:queue_approve(inserted_approval))
assert(status:queue_clear())
assert_eq(status:get_auto_approval(), false, "automatic approval defaults off")
local injected_q = assert(status:inject_queued_prompt("queued injection"))
local injected_s = assert(status:inject_steer_prompt("steering injection"))
assert(status:queue_info(1).held and status:queue_info(2).held)
assert_eq(status:queue_info(2).mode, softline.QUEUE_MODE_STEER, "steering injection route")
assert(status:set_auto_approval(true))
assert(status:get_auto_approval())
assert_eq(status:queue_info(1).id, injected_q, "approval keeps queue identity")
assert_eq(status:queue_info(2).id, injected_s, "approval keeps steering identity")
assert(not status:queue_info(1).held and not status:queue_info(2).held)
assert_eq(status:queue_info(2).mode, softline.QUEUE_MODE_STEER, "approval keeps steering route")
assert(status:queue_clear())
assert(status:inject_queued_prompt("future queued"))
assert(status:inject_steer_prompt("future steer"))
assert(not status:queue_info(1).held and not status:queue_info(2).held)
assert(status:set_auto_approval(false))
assert(not status:get_auto_approval())
assert(not status:queue_info(1).held, "disabling must not revoke approvals")
assert(status:inject_queued_prompt("held again"))
assert(status:queue_info(3).held)
assert(not pcall(function() status:set_auto_approval("yes") end))
assert(not pcall(function() status:inject_queued_prompt() end))
assert(status:queue_clear())
status:close()

print("lua softline smoke passed")
