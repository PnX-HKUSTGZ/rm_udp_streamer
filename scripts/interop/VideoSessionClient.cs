using Godot;
using System;

public sealed class VideoSessionClient : IDisposable
{
    private GodotObject? _native;
    public VideoSessionClient()
    {
        if (!ClassDB.ClassExists("RmVideoSession"))
            throw new InvalidOperationException("视频组件未加载，请检查应用安装。详情见启动日志。");
        using var instance = ClassDB.Instantiate("RmVideoSession");
        _native = instance.AsGodotObject();
    }
    public GodotObject Native => _native!;
    public bool Start(string bindAddress, int port, string decoder = "auto", string device = "0", string transfer = "yuv", bool diagnostics = false,
        string sourceAddress = "", int sourcePort = 0, byte[]? hevcParameters = null)
    {
        using var config = new Godot.Collections.Dictionary();
        config.Put("version", 1); config.Put("bind_address", bindAddress); config.Put("port", port);
        config.Put("decoder", decoder); config.Put("device", device); config.Put("transfer", transfer); config.Put("diagnostics", diagnostics);
        config.Put("source_address", sourceAddress); config.Put("source_port", sourcePort);
        if (hevcParameters != null) config.Put("hevc_parameters", hevcParameters);
        using var argument = Variant.From(config);
        using var result = _native!.Call("start", argument);
        return result.AsBool();
    }
    public void Stop() => _native?.Call("request_stop");
    public Godot.Collections.Dictionary Capabilities()
    {
        using var result = _native!.Call("capabilities");
        return result.AsGodotDictionary();
    }
    public Godot.Collections.Dictionary Snapshot(bool detailed = true) => SnapshotAccess.Read(_native!, "snapshot", detailed);
    public void Dispose()
    {
        if (_native == null) return;
        Stop(); _native.Dispose(); _native = null;
    }
}
