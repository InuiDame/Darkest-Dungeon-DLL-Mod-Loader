-- 内存上限（lua-dev 的 DDLua.cfg 设为 32 MB）：分配到失败，报内存错误而不是崩溃
local t = {}
for i = 1, 1e9 do
    t[i] = string.rep("x", 4096) .. i
end
