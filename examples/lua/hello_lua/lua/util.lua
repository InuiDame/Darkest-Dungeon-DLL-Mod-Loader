-- hello_lua 的工具函数，由 main.lua 用 require("util") 加载。
local M = {}

---@param s string
---@return string
function M.trim(s)
  local trimmed = s:gsub("^%s+", ""):gsub("%s+$", "")
  return trimmed
end

---@param h dd.Actor
---@return string
function M.name(h)
  return string.format("%s#%d（%s）", h.is_hero and "英雄" or "怪物", h.id, h.class or "?")
end

---@param h dd.Actor
---@return string
function M.hp(h)
  if h.hp == nil then
    return "?（句柄已失效）"
  end
  return string.format("%.1f/%s", h.hp, h.max_hp and string.format("%.1f", h.max_hp) or "?")
end

return M
