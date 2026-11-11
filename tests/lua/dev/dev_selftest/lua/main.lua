-- P3a 主菜单自检（rig.py deploy lua-dev）：在真实的 DDCore 服务下跑一遍沙箱用例，结果写进 DDLua.log
local passed, failed = 0, 0
local function check(name, fn)
    local ok, err = pcall(fn)
    if ok then
        passed = passed + 1
    else
        failed = failed + 1
        dd.log.error("自检失败：" .. name .. "：" .. tostring(err))
    end
end
check("io/package/dofile/loadfile 不存在", function() assert(io == nil and package == nil and dofile == nil and loadfile == nil) end)
check("string.dump 不存在", function() assert(string.dump == nil and ("").dump == nil) end)
check("字符串元表受保护", function() assert(getmetatable("") == false) end)
check("load 拒绝二进制", function() local f, e = load("\27Lua\84\0") assert(f == nil and e:find("binary")) end)
check("load 默认环境是自己的 _ENV", function() load("SELFTEST_X = 1")() assert(SELFTEST_X == 1 and load("return _G")() == _G) end)
check("setmetatable 拒绝 __gc", function() assert(not pcall(setmetatable, {}, { __gc = print })) end)
check("collectgarbage 只留查询", function() assert(not pcall(collectgarbage) and type(collectgarbage("count")) == "number") end)
check("os 只留 time/clock/date", function() assert(os.execute == nil and os.getenv == nil and os.exit == nil and type(os.time()) == "number") end)
check("os.date 拒绝负数", function() assert(not pcall(os.date, "%c", -1)) end)
check("debug 只留 traceback", function() assert(debug.getinfo == nil and debug.sethook == nil and debug.getregistry == nil) end)
check("require 拒绝 ..", function() assert(not pcall(require, "../x") and not pcall(require, "C:/x")) end)
check("require 自己的模块", function() assert(require("helper").answer == 42) end)
check("dd.fs 读自己的文件", function() assert(dd.fs.read("lua/helper.lua"):find("answer")) end)
check("dd.fs 拒绝逃逸", function()
    for _, p in ipairs({ "../dev_loop/lua/main.lua", "C:/Windows/win.ini", "/Windows/win.ini", "lua/main.lua:x", "CON" }) do
        assert(dd.fs.read(p) == nil, p)
    end
end)
check("pcall 吞不掉预算以外的普通错误语义", function() assert(not pcall(error, "x")) end)
print(("沙箱自检：通过 %d 项，失败 %d 项；Lua 内存 %.1f KB"):format(passed, failed, collectgarbage("count")))
return { passed = passed, failed = failed }
