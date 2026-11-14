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
    public GodotObject Native => _native!;
    public bool Start(string bindAddress, int port, string decoder = "auto", string device = "0", string transfer = "yuv") => _native!.Call("start", new Godot.Collections.Dictionary
    {
        ["version"] = 1, ["bind_address"] = bindAddress, ["port"] = port,
        ["decoder"] = decoder, ["device"] = device, ["transfer"] = transfer
    }).AsBool();
    public void Stop() => _native?.Call("request_stop");
    public Godot.Collections.Dictionary Snapshot() => _native!.Call("snapshot").AsGodotDictionary();
    public void Dispose()
    {
        if (_native == null) return;
        Stop(); _native.Dispose(); _native = null;
    }
}
