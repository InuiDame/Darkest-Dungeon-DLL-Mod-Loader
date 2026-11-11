-- 预算：pcall 吞错的死循环，必须被打断（加载失败），游戏不卡死
print("dev_loop 开始死循环")
while true do
    pcall(function() while true do end end)
end
