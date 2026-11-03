local providers
local editor = {
  set_completion_providers = function(_, value) providers = value; return 0 end,
}
assert(dofile(arg[1] .. "/examples/completion.lua")(editor, false) == 0)
for _, provider in ipairs(providers) do
  if provider.trigger == "@" then
    local prefix = arg[2] or "@"
    local resolve = prefix == "@" and provider.complete or provider.expand
    for _, name in ipairs(resolve(prefix)) do io.write(name, "\0") end
    return
  end
end
error("missing file completion provider")
