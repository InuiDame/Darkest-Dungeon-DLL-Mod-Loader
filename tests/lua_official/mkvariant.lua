-- 生成按设计差异调整过的测试集副本 lua-5.4.9-tests-dd（原目录不动）。
-- 每一处修改对应 xmake/lua.lua 的一条补丁，见 EDITS 里的说明。锚点必须唯一，否则报错。
-- 用法（工作目录 build/luatests）：lua_vanilla.exe scripts/mkvariant.lua
local src, dst = "lua-5.4.9-tests", "lua-5.4.9-tests-dd"
local EDITS = {
  { "errors.lua", "local idsize = 60 - 1", "local idsize = 160 - 1  -- DD: LUA_IDSIZE 160（补丁第 4 条）",
    "LUA_IDSIZE 改成 160" },
  { "errors.lua", "for i = 60 - 10, 60 + 10 do   -- check border cases around 60",
    "for i = 160 - 10, 160 + 10 do   -- DD: border cases around LUA_IDSIZE 160", "LUA_IDSIZE 改成 160" },
  { "gc.lua", "for k,v in pairs(a) do assert(k==v or k..'#'==v); i=i+1 end",
    "for k,v in pairs(a) do if type(k) ~= 'table' then assert(k==v or k..'#'==v); i=i+1 end end  -- DD: weak keys only -> strong",
    "只有弱键的表按强引用处理（补丁第 5 条）：可回收的表键不被清除" },
  { "gc.lua", "assert(next(C) == nil and next(C1) == nil)",
    "assert(next(C) ~= nil and next(C1) ~= nil)  -- DD: weak keys only -> strong (C1 keeps t, so C keeps it too)",
    "同上：C1 是只有弱键的表，强引用 t，弱值表 C 里的 t 也就不被清除" },
  { "gc.lua", "for i = 1, 4 do assert(a[i][1] == i * 10); a[i] = undef end\nassert(next(a) == nil)",
    "for i = 1, 4 do assert(a[i][1] == i * 10); a[i] = undef end\nassert(next(a) ~= nil)  -- DD: weak keys only -> strong",
    "同上：ephemeron 表 a 的 100 个链式键保留" },
}
local function read (p) local f = assert(io.open(p, "rb")); local d = f:read("a"); f:close(); return d end
local function write (p, d) local f = assert(io.open(p, "wb")); f:write(d); f:close() end
assert(os.execute('rm -rf "' .. dst .. '" && cp -r "' .. src .. '" "' .. dst .. '"'))
for _, e in ipairs(EDITS) do
  local file, old, new = dst .. "/" .. e[1], e[2], e[3]
  local data = read(file)
  local a, b = data:find(old, 1, true)
  assert(a, "anchor not found: " .. e[1] .. ": " .. old)
  assert(not data:find(old, b + 1, true), "anchor not unique: " .. e[1] .. ": " .. old)
  write(file, data:sub(1, a - 1) .. new .. data:sub(b + 1))
  print(string.format("edited %-12s %s", e[1], e[4]))
end
