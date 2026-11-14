using Godot;
using System;

public sealed class VideoSessionClient : IDisposable
{
    private GodotObject? _native;
    public VideoSessionClient()
    {
        if (!ClassDB.ClassExists("RmVideoSession"))
            throw new InvalidOperationException("视频组件未加载，请检查应用安装。详情见启动日志。");
        _native = ClassDB.Instantiate("RmVideoSession").AsGodotObject();
    }
    public bool Start(string bindAddress, int port) => _native!.Call("start", new Godot.Collections.Dictionary
    {
        ["version"] = 1, ["bind_address"] = bindAddress, ["port"] = port
    }).AsBool();
    public void Stop() => _native?.Call("stop");
    public Godot.Collections.Dictionary Snapshot() => _native!.Call("snapshot").AsGodotDictionary();
    public void Dispose()
    {
        if (_native == null) return;
        Stop(); _native.Dispose(); _native = null;
    }
}
