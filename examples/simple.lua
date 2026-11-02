local softline = require("softline")

local sl = softline.new()
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
assert(themes[theme_name], "invalid SOFTLINE_PROMPT_THEME: " .. theme_name)
assert(sl:set_prompt_theme(themes[theme_name]))
assert(sl:set_completions({
  "/quit", "hello world", "hello there",
  {text = "verification", scope = softline.COMPLETION_WORD},
  {text = "next steps", scope = softline.COMPLETION_WORD},
}))
local interactive = os.execute("test -t 0 && test -t 1")
if interactive == true or interactive == 0 then
  print("Completion demo: type hel, Tab, then w or t and Tab to finish.")
  print("  Try say ver + Tab on any line; Ctrl-J adds a line; /q + Tab completes /quit.")
end

while true do
  local line, status = sl:readline("softline> ")
  if not line then
    if status == softline.READLINE_EOF then
      break
    end
    local err = sl:last_error()
    if err then
      io.stderr:write(err, "\n")
    end
    break
  end
  if line == "/quit" then
    break
  end
  io.write("submitted: ", line, "\n")
  sl:history_add(line)
end

sl:close()
