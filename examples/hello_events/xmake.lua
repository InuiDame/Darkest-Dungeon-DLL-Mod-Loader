target("hello_events")
    add_rules("dd.native")
    set_values("dd.dkutil", "off")
    set_values("dd.rc", "off")
    set_default(false)
    set_group("examples")

    add_files("src/*.cpp")
target_end()
