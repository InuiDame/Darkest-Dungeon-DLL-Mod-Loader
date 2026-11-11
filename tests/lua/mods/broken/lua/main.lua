-- 夹具：main.lua 出错
BROKEN_PARTIAL = true
local function inner()
    error("boom from broken")
end
inner()
