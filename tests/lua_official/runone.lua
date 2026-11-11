-- 单独运行官方测试集的一个文件，环境和 all.lua 的 _U=true 相同（_soft、_port、_nomsg 为真，T 为 nil，debug 为 nil）。
-- 用法（工作目录是测试集目录）：lua.exe ../scripts/runone.lua <文件> [strip|plain]
--   默认像 all.lua 的 dofile 一样 string.dump 再 load；plain 表示直接 loadfile（all.lua 对 strings.lua、literals.lua 用 olddofile）
local file, mode = arg[1], arg[2]
_soft, _port, _nomsg = true, true, true
T = nil
debug = nil
function Message (m) end
assert(os.setlocale"C")
local f = assert(loadfile(file))
if mode ~= "plain" then
  f = assert(load(string.dump(f, mode == "strip")))
end
local t0 = os.clock()
local ok, r = xpcall(f, function (m) return require"debug".traceback(tostring(m), 2) end)
if ok then
  print(string.format("\n@@RESULT %s PASS return=%s time=%.2f", file, tostring(r), os.clock() - t0))
else
  print(string.format("\n@@RESULT %s FAIL time=%.2f\n%s", file, os.clock() - t0, tostring(r)))
end
