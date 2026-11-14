extends SceneTree
func _initialize():
    var path=OS.get_environment("RM_ENGINE_NOTICES")
    var output=FileAccess.open(path,FileAccess.WRITE)
    if output==null:quit(2);return
    output.store_string(JSON.stringify({"version":Engine.get_version_info(),"license":Engine.get_license_text(),"copyright":Engine.get_copyright_info(),"third_party_licenses":Engine.get_license_info()},"  "))
    output.close();quit()
