local softline = require("softline")
return function(sl, chat)
  local function commands(prefix)
    if prefix == "/approve" and chat then return {"/approve on", "/approve off"} end
    if prefix ~= "/" then return {} end
    if chat then
      return {"/status", "/approve", "/steer", "/resume", "/shell", "/quit"}
    end
    return {"/quit"}
  end
  local function files(prefix)
    if #prefix > 1 and prefix:sub(-1) ~= "/" then return {} end
    local directory = #prefix > 1 and "./" .. prefix:sub(2) or "."
    local quoted = "'" .. directory:gsub("'", "'\\''") .. "'"
    -- NUL separators preserve spaces and linefeeds. Read only this directory;
    -- later Tabs request its children through the very same callback.
    local pipe = assert(io.popen([[sh -c '
      cd "$1" || exit 1
      for file in ./* ./.[!.]* ./..?*; do
        [ -e "$file" ] || [ -L "$file" ] || continue
        name=${file#./}
        [ "$name" = .git ] && continue
        if [ -d "$file" ] && [ ! -L "$file" ]; then name=$name/; fi
        printf "%s\000" "$name"
      done
    ' sh ]] .. quoted, "r"))
    local names = pipe:read("*a") or ""
    assert(pipe:close(), "could not list completion directory")
    local result = {}
    for name in names:gmatch("([^%z]+)%z") do result[#result + 1] = prefix .. name end
    return result
  end
  -- Each callback returns full strings including its trigger. Phrase spaces
  -- keep matching active; expand resolves a new directory only on a later Tab.
  return sl:set_completion_providers({
    {trigger = "/", complete = commands, expand = commands},
    {trigger = "h", complete = function() return {"hello world", "hello there"} end},
    {trigger = "v", scope = softline.COMPLETION_WORD, complete = function() return {"verification"} end},
    {trigger = "n", scope = softline.COMPLETION_WORD, complete = function() return {"next steps"} end},
    {trigger = "@", scope = softline.COMPLETION_WORD, strip_trigger = true, complete = files, expand = files},
  })
end
