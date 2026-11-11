-- 依赖 dev_selftest；dev_loop、dev_pattern、dev_memory 失败不影响它
local selftest = dd.import("dev_selftest")
print("dd.import(dev_selftest)：通过 " .. tostring(selftest and selftest.passed) .. " 项，失败 " .. tostring(selftest and selftest.failed) .. " 项")
assert(SELFTEST_X == nil, "全局表互相隔离")
