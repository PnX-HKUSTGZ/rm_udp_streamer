using Godot;
using System;
using System.IO;
using System.Diagnostics;

public partial class Main : Control
{
    private VideoSessionClient? _session;
    private LineEdit _address = null!;
    private SpinBox _port = null!, _device = null!;
    private OptionButton _decoder = null!, _transfer = null!;
    private Button _toggle = null!;
    private Label _state = null!, _metrics = null!, _source = null!, _error = null!, _path = null!, _placeholder = null!;
    private Control _videoArea = null!, _surface = null!;
    private bool _running;
    private double _elapsed, _rateTime;
    private long _lastComplete, _lastDecoded, _lastSubmitted;
    private double _receiveFps, _decodeFps, _submitFps;
    private RuntimeOptions _options = new();
    private StreamWriter? _diagnostics;
    private double _uptime, _diagnosticTime;
    private bool _readyLogged, _finishing;
    private static readonly string[] Decoders = { "auto", "software", "cuda", "d3d11va" };
    private static readonly string[] Transfers = { "yuv", "rgba", "gpu", "auto" };

    public override void _Ready()
    {
        BuildInterface();
        try
        {
            _options = RuntimeOptions.Parse(OS.GetCmdlineUserArgs());
            if (_options.Help) { GD.Print(RuntimeOptions.Usage); GetTree().Quit(); return; }
            if (_options.Metrics.Length > 0)
                _diagnostics = new StreamWriter(new FileStream(_options.Metrics, FileMode.CreateNew, System.IO.FileAccess.Write, FileShare.Read)) { AutoFlush = true };
            _session = new VideoSessionClient();
            _surface = (Control)ClassDB.Instantiate("RmVideoSurface").AsGodotObject();
            _videoArea.AddChild(_surface); _videoArea.MoveChild(_surface, 1);
            _surface.SetAnchorsAndOffsetsPreset(LayoutPreset.FullRect);
            _surface.Call("set_session", _session.Native);
            _address.Text = _options.Address; _port.Value = _options.Port; _device.Value = _options.Device;
            _decoder.Select(Array.IndexOf(Decoders, _options.Decoder)); _transfer.Select(Array.IndexOf(Transfers, _options.Transfer));
            Refresh();
            if (_options.Start) Toggle();
        }
        catch (Exception error) { _error.Text = error.Message; _toggle.Disabled = true; GD.PushError(error.Message); GetTree().Quit(2); }
    }
    private static Label Text(string value, int size = 16)
    {
        var label = new Label { Text = value };
        label.AddThemeFontSizeOverride("font_size", size); return label;
    }
    private void BuildInterface()
    {
        var background = new ColorRect { Color = new Color("10151d"), MouseFilter = MouseFilterEnum.Ignore };
        AddChild(background); background.SetAnchorsAndOffsetsPreset(LayoutPreset.FullRect);
        var margin = new MarginContainer(); AddChild(margin); margin.SetAnchorsAndOffsetsPreset(LayoutPreset.FullRect);
        foreach (string side in new[] { "left", "right", "top", "bottom" }) margin.AddThemeConstantOverride("margin_" + side, 24);
        var layout = new VBoxContainer(); layout.AddThemeConstantOverride("separation", 12); margin.AddChild(layout);
        var heading = new HBoxContainer(); layout.AddChild(heading);
        var title = Text("RM 视频接收", 26); title.SizeFlagsHorizontal = SizeFlags.ExpandFill; heading.AddChild(title);
        _state = Text("已停止"); heading.AddChild(_state);
        var inputs = new HBoxContainer(); inputs.AddThemeConstantOverride("separation", 12); layout.AddChild(inputs);
        inputs.AddChild(Text("监听地址"));
        _address = new LineEdit { Text = "0.0.0.0", CustomMinimumSize = new Vector2(180, 36) }; inputs.AddChild(_address);
        inputs.AddChild(Text("端口"));
        _port = new SpinBox { MinValue = 1, MaxValue = 65535, Step = 1, Value = 3334, CustomMinimumSize = new Vector2(120,36) }; inputs.AddChild(_port);
        _toggle = new Button { Text = "开始接收", CustomMinimumSize = new Vector2(140,36) }; inputs.AddChild(_toggle); _toggle.Pressed += Toggle;
        var modes = new HBoxContainer(); modes.AddThemeConstantOverride("separation", 12); layout.AddChild(modes);
        modes.AddChild(Text("解码")); _decoder = new OptionButton(); modes.AddChild(_decoder);
        foreach (string value in new[] { "自动", "软件", "NVIDIA CUDA", "Windows D3D11" }) _decoder.AddItem(value);
        modes.AddChild(Text("设备序号")); _device = new SpinBox { MinValue=0, MaxValue=999, Step=1, Value=0 }; modes.AddChild(_device);
        modes.AddChild(Text("画面传输")); _transfer = new OptionButton(); modes.AddChild(_transfer);
        _transfer.AddItem("CPU YUV"); _transfer.AddItem("CPU RGBA"); _transfer.AddItem("GPU CUDA / OpenGL");
        _transfer.AddItem("自动 GPU / CPU");
        _videoArea = new Control { SizeFlagsVertical = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(0,200) }; layout.AddChild(_videoArea);
        var black = new ColorRect { Color = new Color("080c12"), MouseFilter = MouseFilterEnum.Ignore };
        _videoArea.AddChild(black); black.SetAnchorsAndOffsetsPreset(LayoutPreset.FullRect);
        _placeholder = Text("等待视频画面", 24); _placeholder.HorizontalAlignment=HorizontalAlignment.Center;
        _placeholder.VerticalAlignment=VerticalAlignment.Center; _placeholder.MouseFilter=MouseFilterEnum.Ignore;
        _videoArea.AddChild(_placeholder); _placeholder.SetAnchorsAndOffsetsPreset(LayoutPreset.FullRect);
        _source = Text("来源：等待数据"); layout.AddChild(_source);
        _path = Text("选择解码方式后开始接收", 14); _path.AutowrapMode = TextServer.AutowrapMode.WordSmart; layout.AddChild(_path);
        _metrics = Text("重组 0.0 · 解码 0.0 · 新画面提交 0.0 帧/秒", 14); layout.AddChild(_metrics);
        _error = Text("", 14); _error.Modulate = new Color("ffb4a9"); _error.AutowrapMode = TextServer.AutowrapMode.WordSmart; layout.AddChild(_error);
    }
    private void Toggle()
    {
        if (_session == null) return;
        _surface.Call("clear");
        if (_running) _session.Stop();
        else
        {
            _lastComplete=_lastDecoded=_lastSubmitted=0; _rateTime=0; _receiveFps=_decodeFps=_submitFps=0;
            _session.Start(_address.Text.Trim(), (int)_port.Value, Decoders[_decoder.Selected], ((int)_device.Value).ToString(), Transfers[_transfer.Selected],
                _options.Diagnostics ?? (_options.Metrics.Length > 0));
        }
        Refresh();
    }
    private void Refresh()
    {
        if (_session == null) return;
        _session.Native.Call("poll_stop");
        using var data = _session.Snapshot(); using var presentation = SnapshotAccess.Read(_surface, "get_metrics");
        _running = data.Flag("running"); string state=data.Text("state"); long age=data.Number("frame_age_ms");
        _state.Text = state switch { "Starting"=>"正在启动", "WaitingConfig"=>"等待参数与关键帧", "Running"=>age>500 ? "画面信号过期" : "正在播放",
            "Recovering"=>"恢复中 · 等待关键帧", "Stopping"=>"正在停止", "Faulted"=>"接收失败", _=>"已停止" };
        _toggle.Text = _running ? "停止接收" : "开始接收"; _toggle.Disabled=state=="Stopping";
        _address.Editable = !_running; _port.Editable = !_running; _device.Editable=!_running;
        _decoder.Disabled=_running; _transfer.Disabled=_running;
        bool displayed=presentation.Has("shown_sequence"); _placeholder.Visible=!displayed;
        _placeholder.Text = _running ? age>2000 ? "视频信号已中断" : "等待可解码的视频画面" : "等待视频画面";
        string source = data.Text("source"); _source.Text = "来源："+(source.Length==0 ? "等待数据" : source);
        _path.Text = $"解码：{data.Text("decoder")} · {data.Text("decode_device")}  |  传输：{data.Text("mode")}  |  {data.Number("width")} × {data.Number("height")}";
        if (data.Flag("color_defaulted")) _path.Text += " · 未标注颜色采用 SDR 默认值";
        _path.Text += $"\n渲染：{RenderingServer.GetVideoAdapterName()}";
        if (_rateTime >= 1)
        {
            long complete=data.Number("complete"), decoded=data.Number("decoded"), submitted=data.Number("submitted_total");
            _receiveFps=Math.Max(0,complete-_lastComplete)/_rateTime; _decodeFps=Math.Max(0,decoded-_lastDecoded)/_rateTime;
            _submitFps=Math.Max(0,submitted-_lastSubmitted)/_rateTime;
            _lastComplete=complete; _lastDecoded=decoded; _lastSubmitted=submitted; _rateTime=0;
        }
        _metrics.Text = $"重组 {_receiveFps:F1} · 解码 {_decodeFps:F1} · 新画面提交 {_submitFps:F1} 帧/秒"+
            $"  |  缺失 {data.Number("missing")} · 恢复 {data.Number("recoveries")} · 显示替换 {data.Number("mailbox_replaced")} · 像素回读 {data.Number("cpu_readbacks")}";
        string failure=data.Text("error"), uploadError=presentation.Text("error");
        _error.Text = failure.Length>0 ? failure : uploadError.Length>0 ? uploadError :
            state=="Recovering" ? data.Text("recovery_reason") : data.Text("fallback");
    }
    public override void _Process(double delta)
    {
        _uptime += delta; _diagnosticTime += delta;
        _elapsed += delta; _rateTime += delta;
        if (_elapsed < .25) return; _elapsed = 0;
        if (_session != null && !_finishing)
        {
            try
            {
                using var data = _session.Snapshot();
                if (_options.Start && !_readyLogged && data.Text("state") != "Starting")
                { _readyLogged = true; GD.Print("RM_READY ", SnapshotAccess.Serialize(data)); }
                if (_diagnostics != null && _diagnosticTime >= 1) { RecordDiagnostics(); _diagnosticTime = 0; }
                if (_options.Duration > 0 && _uptime >= _options.Duration) { _finishing = true; FinishTimedRun(); }
            }
            catch (Exception error) { GD.PushError(error.Message); _session.Stop(); GetTree().Quit(2); }
        }
        Refresh();
    }
    private string RecordDiagnostics()
    {
        using var core = _session!.Snapshot(); using var surface = SnapshotAccess.Read(_surface, "get_metrics");
        using var record = new Godot.Collections.Dictionary();
        record.Put("seconds", _uptime); record.Put("core", core); record.Put("surface", surface);
        record.Put("render_device", RenderingServer.GetVideoAdapterName());
        string json = SnapshotAccess.Serialize(record);
        _diagnostics?.WriteLine(json); return json;
    }
    private async void FinishTimedRun()
    {
        int exitCode = 0;
        try
        {
            await ToSignal(RenderingServer.Singleton, RenderingServer.SignalName.FramePostDraw);
            if (_options.Snapshot.Length > 0)
            {
                if (File.Exists(_options.Snapshot)) throw new IOException("Snapshot output already exists");
                using var image = GetViewport().GetTexture().GetImage();
                if (image.SavePng(_options.Snapshot) != Error.Ok) throw new IOException("Could not write snapshot");
            }
            var final = RecordDiagnostics();
            GD.Print("RM_FINAL ", final);
            using var finalCore = _session!.Snapshot(); using var finalSurface = SnapshotAccess.Read(_surface, "get_metrics");
            if (finalCore.Text("error").Length > 0 || finalSurface.Text("error").Length > 0) exitCode = 2;
        }
        catch (Exception error) { GD.PushError(error.Message); exitCode = 2; }
        var stopClock = Stopwatch.StartNew();
        string stopError = "";
        using var requestedCore = _session!.Snapshot(); using var requested = new Godot.Collections.Dictionary();
        requested.Put("event", "stop_requested"); requested.Put("core", requestedCore);
        string requestedJson = SnapshotAccess.Serialize(requested);
        _diagnostics?.WriteLine(requestedJson); GD.Print("RM_STOP_REQUESTED ", requestedJson);
        _session.Stop(); _surface.Call("clear");
        var status = _session.Snapshot();
        try
        {
            while (true)
            {
                _session.Native.Call("poll_stop"); status.Dispose(); status = _session.Snapshot();
                if (status.Flag("render_retirement_failed")) stopError = status.Text("render_retirement_error");
                if (status.Flag("stop_terminal")) break;
                if (stopClock.Elapsed.TotalSeconds >= 5)
                {
                    stopError = "Stop deadline exceeded; outstanding resources are retained until process exit";
                    break;
                }
                await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
            }
            // A failure may be reported by the render thread after RM_FINAL.
            if (stopError.Length > 0 || status.Text("error").Length > 0) exitCode = 2;
            using var stopped = new Godot.Collections.Dictionary();
            stopped.Put("event", stopError.Length > 0 ? "stop_failed" : "stopped"); stopped.Put("core", status);
            stopped.Put("stop_elapsed_ms", stopClock.Elapsed.TotalMilliseconds); stopped.Put("stop_error", stopError); stopped.Put("exit_code", exitCode);
            string stoppedJson = SnapshotAccess.Serialize(stopped);
            _diagnostics?.WriteLine(stoppedJson);
            GD.Print(stopError.Length > 0 ? "RM_STOP_FAILED " : "RM_STOPPED ", stoppedJson);
            GetTree().Quit(exitCode);
        }
        finally { status.Dispose(); }
    }
    public override void _ExitTree() { _diagnostics?.Dispose(); _session?.Dispose(); _session=null; }
}
