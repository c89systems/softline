local softline = require('softline')
local sl = softline.new()
local inactive, inactive_status = sl:suspend()
assert(inactive == nil and inactive_status ~= nil)
assert(sl:set_prompt_theme(softline.PROMPT_THEME_PLAIN))
assert(sl:readline('word> ') == 'one! two')
assert(sl:readline('kill> ') == 'café—中文')
assert(sl:readline('again> ') == 'café')
assert(sl:readline('undo> ') == '')
assert(sl:readline('transpose> ') == 'éa')
assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte('s'), function()
  assert(sl:suspend())
  return softline.KEY_ACTION_HANDLED
end))
local line, status = sl:readline('suspend> ')
assert(line == nil and status == softline.READLINE_SUSPENDED)
assert(sl:last_readline_status() == softline.READLINE_SUSPENDED)
assert(sl:buffer() == 'abcXdef' and sl:cursor() == 4)
assert(sl:readline('resume> ') == 'abcdef') -- undo survives suspension
local mutations = {
  function() assert(sl:set_buffer('')) end,
  function() assert(sl:set_cursor(1)); assert(sl:insert('👩‍💻\nX')) end,
  function() assert(sl:set_buffer(string.rep('L', 512))) end,
  function() assert(sl:set_buffer('abc')) end,
  function() assert(sl:insert('')) end,
}
for index, mutate in ipairs(mutations) do
  line, status = sl:readline('m' .. index .. '> ')
  assert(line == nil and status == softline.READLINE_SUSPENDED)
  mutate()
  line, status = sl:readline('u' .. index .. '> ')
  assert(line == nil and status == softline.READLINE_SUSPENDED)
  assert(sl:buffer() == (index <= 3 and 'abc' or ''))
  assert(sl:readline('f' .. index .. '> ') == '')
end
local moved = false
assert(sl:set_idle_callback(function()
  if not moved and sl:buffer() == 'abc' then
    assert(sl:set_cursor(0))
    moved = true
    assert(sl:print_above('CURSOR-MOVED\n'))
  end
end))
assert(sl:readline('host-cursor> ') == 'abc')
assert(moved and sl:set_idle_callback(nil))
assert(sl:bind_key(softline.KEY_ALT_BASE + string.byte('t'), function()
  assert(sl:insert('*'))
  assert(sl:suspend())
  return softline.KEY_ACTION_HANDLED
end))
line, status = sl:readline('group> ')
assert(line == nil and status == softline.READLINE_SUSPENDED)
assert(sl:buffer() == 'abc*')
assert(sl:readline('group-resume> ') == 'abc*')
assert(sl:readline('prefix> ') == 'ok')
assert(sl:history_add('matching history'))
line, status = sl:readline('search> ')
assert(line == nil and status == softline.READLINE_SUSPENDED)
assert(sl:history_search_active() and sl:buffer() == 'matching history')
print('SEARCH-SUSPENDED')
assert(sl:readline('search-resume> ') == 'draft!')
local first, second = string.rep('a', 17), string.rep('b', 18)
assert(sl:set_screen_width(24))
assert(sl:bind_key(softline.KEY_TAB, function()
  assert(sl:set_buffer(first .. '\n' .. second))
  assert(sl:set_cursor(17))
  return softline.KEY_ACTION_HANDLED
end))
assert(sl:readline('draft> ') == first .. '!\n' .. second)
assert(sl:set_screen_width(0))
-- Destruction also owns a suspended search's query, prompt and saved draft.
line, status = sl:readline('destroy> ')
assert(line == nil and status == softline.READLINE_SUSPENDED)
sl:close()
print('READLINE-EDIT-PASS')
