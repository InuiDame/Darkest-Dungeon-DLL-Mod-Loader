set_project("DD.ModLoader")

includes("xmake/native.lua")
includes("xmake/lua.lua")

-- 根目录下带 xmake.lua 的子目录即为子工程
for _, file in ipairs(os.files(path.join(os.scriptdir(), "*", "xmake.lua"))) do
    local name = path.filename(path.directory(file))
    if name ~= "xmake" and name ~= "extern" then
        includes(file)
    end
end
