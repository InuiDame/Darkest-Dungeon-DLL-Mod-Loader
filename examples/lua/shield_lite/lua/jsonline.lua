-- 把一张小表编码成一行 JSON（键按字母排序），给日志用。只支持字符串键的表、数组、字符串、数字、布尔。
local M = {}

local function quote(s)
  return '"' .. s:gsub('[%c"\\]', function(c)
    if c == '"' then return '\\"' end
    if c == "\\" then return "\\\\" end
    return string.format("\\u%04x", c:byte())
  end) .. '"'
end

local function number(x)
  if math.type(x) == "integer" then
    return tostring(x)
  end
  if x ~= x or x == math.huge or x == -math.huge then
    return "null"
  end
  return string.format("%.10g", x)
end

local function encode(v)
  local t = type(v)
  if t == "nil" then
    return "null"
  elseif t == "boolean" then
    return v and "true" or "false"
  elseif t == "number" then
    return number(v)
  elseif t == "string" then
    return quote(v)
  elseif t == "table" then
    if #v > 0 then
      local out = {}
      for i = 1, #v do
        out[i] = encode(v[i])
      end
      return "[" .. table.concat(out, ",") .. "]"
    end
    local keys = {}
    for k in pairs(v) do
      keys[#keys + 1] = tostring(k)
    end
    table.sort(keys)
    local out = {}
    for _, k in ipairs(keys) do
      out[#out + 1] = quote(k) .. ":" .. encode(v[k])
    end
    return "{" .. table.concat(out, ",") .. "}"
  end
  return quote(tostring(v))
end

M.encode = encode
return M
