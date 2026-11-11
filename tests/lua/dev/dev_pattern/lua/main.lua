-- 预算：模式匹配回溯（C 函数内部的长循环），必须被打断
print("dev_pattern 开始回溯")
string.find(string.rep("a", 50000), ".-.-.-.-.-b")
