-- native 插件模板。复制本目录，改掉 target 名和 dd.mod，把它放到自己的工程里
-- 或继续放在 templates 下用 xmake build my_plugin 构建。
target("my_plugin")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("templates")

    set_version("1.0.0")
    -- 构建后复制到 $DDPath\mods\<目录>。不部署可以删掉这一行。
    -- set_values("dd.mod", "MyPlugin")

    add_files("src/*.cpp")
target_end()
