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
            _session.Start(_address.Text.Trim(), (int)_port.Value, Decoders[_decoder.Selected], ((int)_device.Value).ToString(), Transfers[_transfer.Selected]);
        }
        Refresh();
    }
    private void Refresh()
    {
        if (_session == null) return;
        _session.Native.Call("poll_stop");
        var data = _session.Snapshot(); var presentation = _surface.Call("get_metrics").AsGodotDictionary();
        _running = data["running"].AsBool(); string state=data["state"].AsString(); long age=data["frame_age_ms"].AsInt64();
        _state.Text = state switch { "Starting"=>"正在启动", "WaitingConfig"=>"等待参数与关键帧", "Running"=>age>500 ? "画面信号过期" : "正在播放",
            "Recovering"=>"恢复中 · 等待关键帧", "Stopping"=>"正在停止", "Faulted"=>"接收失败", _=>"已停止" };
        _toggle.Text = _running ? "停止接收" : "开始接收"; _toggle.Disabled=state=="Stopping";
        _address.Editable = !_running; _port.Editable = !_running; _device.Editable=!_running;
        _decoder.Disabled=_running; _transfer.Disabled=_running;
        bool displayed=presentation.ContainsKey("shown_sequence"); _placeholder.Visible=!displayed;
        _placeholder.Text = _running ? age>2000 ? "视频信号已中断" : "等待可解码的视频画面" : "等待视频画面";
        string source = data["source"].AsString(); _source.Text = "来源："+(source.Length==0 ? "等待数据" : source);
        _path.Text = $"解码：{data["decoder"]} · {data["decode_device"]}  |  传输：{data["mode"]}  |  {data["width"]} × {data["height"]}";
        if (data["color_defaulted"].AsBool()) _path.Text += " · 未标注颜色采用 SDR 默认值";
        _path.Text += $"\n渲染：{RenderingServer.GetVideoAdapterName()}";
        if (_rateTime >= 1)
        {
            long complete=data["complete"].AsInt64(), decoded=data["decoded"].AsInt64(), submitted=presentation["submitted"].AsInt64();
            _receiveFps=Math.Max(0,complete-_lastComplete)/_rateTime; _decodeFps=Math.Max(0,decoded-_lastDecoded)/_rateTime;
            _submitFps=Math.Max(0,submitted-_lastSubmitted)/_rateTime;
            _lastComplete=complete; _lastDecoded=decoded; _lastSubmitted=submitted; _rateTime=0;
        }
        _metrics.Text = $"重组 {_receiveFps:F1} · 解码 {_decodeFps:F1} · 新画面提交 {_submitFps:F1} 帧/秒"+
            $"  |  缺失 {data["missing"]} · 恢复 {data["recoveries"]} · 显示替换 {data["mailbox_replaced"]} · 像素回读 {data["cpu_readbacks"]}";
        string failure=data["error"].AsString(), uploadError=presentation["error"].AsString();
        _error.Text = failure.Length>0 ? failure : uploadError.Length>0 ? uploadError :
            state=="Recovering" ? data["recovery_reason"].AsString() : data["fallback"].AsString();
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
                var data = _session.Snapshot();
                if (_options.Start && !_readyLogged && data["state"].AsString() != "Starting")
                { _readyLogged = true; GD.Print("RM_READY ", Json.Stringify(data)); }
                if (_diagnostics != null && _diagnosticTime >= 1) { RecordDiagnostics(); _diagnosticTime = 0; }
                if (_options.Duration > 0 && _uptime >= _options.Duration) { _finishing = true; FinishTimedRun(); }
            }
            catch (Exception error) { GD.PushError(error.Message); _session.Stop(); GetTree().Quit(2); }
        }
        Refresh();
    }
    private Godot.Collections.Dictionary RecordDiagnostics()
    {
        var record = new Godot.Collections.Dictionary {
            ["seconds"] = _uptime, ["core"] = _session!.Snapshot(),
            ["surface"] = _surface.Call("get_metrics"), ["render_device"] = RenderingServer.GetVideoAdapterName()
        };
        _diagnostics?.WriteLine(Json.Stringify(record)); return record;
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
            GD.Print("RM_FINAL ", Json.Stringify(final));
            if (_session!.Snapshot()["error"].AsString().Length > 0 ||
                _surface.Call("get_metrics").AsGodotDictionary()["error"].AsString().Length > 0) exitCode = 2;
        }
        catch (Exception error) { GD.PushError(error.Message); exitCode = 2; }
        var stopClock = Stopwatch.StartNew();
        string stopError = "";
        var requested = new Godot.Collections.Dictionary { ["event"] = "stop_requested", ["core"] = _session!.Snapshot() };
        _diagnostics?.WriteLine(Json.Stringify(requested)); GD.Print("RM_STOP_REQUESTED ", Json.Stringify(requested));
        _session.Stop(); _surface.Call("clear");
        Godot.Collections.Dictionary status;
        while (true)
        {
            _session.Native.Call("poll_stop"); status = _session.Snapshot();
            if (status["render_retirement_failed"].AsBool()) stopError = status["render_retirement_error"].AsString();
            if (status["stop_terminal"].AsBool()) break;
            if (stopClock.Elapsed.TotalSeconds >= 5)
            {
                stopError = "Stop deadline exceeded; outstanding resources are retained until process exit";
                break;
            }
            await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
        }
        // A failure may be reported by the render thread after RM_FINAL.
        if (stopError.Length > 0 || status["error"].AsString().Length > 0) exitCode = 2;
        var stopped = new Godot.Collections.Dictionary {
            ["event"] = stopError.Length > 0 ? "stop_failed" : "stopped", ["core"] = status,
            ["stop_elapsed_ms"] = stopClock.Elapsed.TotalMilliseconds, ["stop_error"] = stopError, ["exit_code"] = exitCode
        };
        _diagnostics?.WriteLine(Json.Stringify(stopped));
        GD.Print(stopError.Length > 0 ? "RM_STOP_FAILED " : "RM_STOPPED ", Json.Stringify(stopped));
        GetTree().Quit(exitCode);
    }
    public override void _ExitTree() { _diagnostics?.Dispose(); _session?.Dispose(); _session=null; }
}
