extends SceneTree

func _initialize():
    var source = OS.get_environment("RM_NOTICES_PROJECT")
    var output = OS.get_environment("RM_NOTICES_PACK")
    var packer = PCKPacker.new()
    if source.is_empty() or output.is_empty() or packer.pck_start(output) != OK:
        quit(2)
        return
    for name in ["project.godot", "notices.gd", "main.tscn"]:
        if packer.add_file("res://" + name, source.path_join(name)) != OK:
            quit(3)
            return
    quit(0 if packer.flush() == OK else 4)
