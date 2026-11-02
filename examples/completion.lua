local softline = require("softline")
return function(sl, chat)
  local function commands()
    if chat then
      return {"/status", "/approve on", "/approve off", "/steer", "/resume", "/shell", "/quit"}
    end
    return {"/quit"}
  end
  local function files()
    -- NUL separators preserve spaces and linefeeds in directory entry names.
    local pipe = assert(io.popen([[for file in ./* ./.[!.]* ./..?*; do
      [ -e "$file" ] || continue
      printf '%s\000' "${file#./}"
    done]], "r"))
    local names = pipe:read("*a") or ""
    pipe:close()
    local result = {}
    for name in names:gmatch("([^%z]+)%z") do result[#result + 1] = "@" .. name end
    return result
  end
  return sl:set_completion_providers({
    {trigger = "/", complete = commands},
    {trigger = "h", complete = function() return {"hello world", "hello there"} end},
    {trigger = "v", scope = softline.COMPLETION_WORD, complete = function() return {"verification"} end},
    {trigger = "n", scope = softline.COMPLETION_WORD, complete = function() return {"next steps"} end},
    {trigger = "@", scope = softline.COMPLETION_WORD, strip_trigger = true, complete = files},
  })
end
