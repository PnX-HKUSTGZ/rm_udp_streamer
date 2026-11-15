using Godot;
using System;
using System.IO;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

public partial class Main : Control
{
    private VideoSessionClient? _session;
    private LineEdit _address = null!, _device = null!;
    private SpinBox _port = null!;
    private OptionButton _decoder = null!, _transfer = null!, _cadence = null!;
    private FrameCadence? _cadencePolicy;
    private Button _toggle = null!;
    private Label _state = null!, _metrics = null!, _source = null!, _error = null!, _path = null!, _placeholder = null!;
    private Control _videoArea = null!, _surface = null!;
    private bool _running, _cadenceActive;
    private double _elapsed, _rateTime;
    private long _lastComplete, _lastDecoded, _lastSubmitted;
    private double _receiveFps, _decodeFps, _submitFps;
    private RuntimeOptions _options = new();
    private StreamWriter? _diagnostics;
    private double _uptime, _diagnosticTime;
    private readonly Stopwatch _wallClock = new();
    private bool _readyLogged, _finishing;
    private static readonly string[] Decoders = { "auto", "software", "cuda", "d3d11va", "vaapi" };
    private static readonly string[] Transfers = { "yuv", "rgba", "gpu", "auto" };
    private static readonly string[] Cadences = { "auto", "display", "responsive", "engine" };

    public override void _Ready()
    {
        _wallClock.Start();
        BuildInterface();
        try
        {
            string[] arguments = OS.GetCmdlineUserArgs();
            _options = RuntimeOptions.Parse(arguments);
            if (_options.Help) { GD.Print(RuntimeOptions.Usage); GetTree().Quit(); return; }
            if (arguments.Length == 0) _options = ReceiverSettings.Load(out _settingsNotice);
            _interactiveSettings = !_options.Start && _options.Duration == 0 && _options.Metrics.Length == 0;
            _cadencePolicy = new FrameCadence();
            if (_options.Metrics.Length > 0)
                _diagnostics = new StreamWriter(new FileStream(_options.Metrics, FileMode.CreateNew, System.IO.FileAccess.Write, FileShare.Read)) { AutoFlush = true };
            _session = new VideoSessionClient();
            _surface = (Control)ClassDB.Instantiate("RmVideoSurface").AsGodotObject();
            _videoArea.AddChild(_surface); _videoArea.MoveChild(_surface, 1);
            _surface.SetAnchorsAndOffsetsPreset(LayoutPreset.FullRect);
            _surface.Call("set_session", _session.Native);
            _address.Text = _options.Address; _port.Value = _options.Port; _device.Text = _options.Device;
            _decoder.Select(Array.IndexOf(Decoders, _options.Decoder)); _transfer.Select(Array.IndexOf(Transfers, _options.Transfer));
            _cadence.Select(Array.IndexOf(Cadences, _options.Cadence));
            _sourceFilter.Text = _options.SourceAddress; _sourcePort.Value = _options.SourcePort;
            _configurationInitializing=false;
            ReloadDevices();
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
        BuildConfiguration(layout);
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
            var settings = ReadConfiguration();
            try
            {
                settings.Validate();
                if (_configurationBlock.Length > 0) throw new ArgumentException(_configurationBlock);
                if (_interactiveSettings) ReceiverSettings.Save(settings);
            }
            catch (Exception error)
            {
                _settingsNotice = error.Message; UpdateCapabilityHints();
                if (_options.Start) throw;
                return;
            }
            _lastComplete=_lastDecoded=_lastSubmitted=0; _rateTime=0; _receiveFps=_decodeFps=_submitFps=0;
            _session.Start(settings.Address, settings.Port, settings.Decoder, settings.Device, settings.Transfer,
                _options.Diagnostics ?? (_options.Metrics.Length > 0), settings.SourceAddress, settings.SourcePort);
        }
        Refresh();
    }
    private void Refresh()
    {
        if (_session == null) return;
        _session.Native.Call("poll_stop");
        using var data = _session.Snapshot(false); using var presentation = SnapshotAccess.Read(_surface, "get_metrics", false);
        _running = data.Flag("running"); string state=data.Text("state"); long age=data.Number("frame_age_ms");
        _cadenceActive = _running && state != "Faulted";
        _cadencePolicy?.Apply(Cadences[_cadence.Selected], _cadenceActive);
        _state.Text = state switch { "Starting"=>"正在启动", "WaitingConfig"=>"等待参数与关键帧", "Running"=>age>500 ? "画面信号过期" : "正在播放",
            "Recovering"=>"恢复中 · 等待关键帧", "Stopping"=>"正在停止", "Faulted"=>"接收失败", _=>"已停止" };
        _toggle.Text = _running ? "停止接收" : "开始接收";
        _toggle.Disabled=state=="Stopping" || _restarting || (!_running && _configurationBlock.Length > 0);
        SetConfigurationEditable(!_running && !_restarting);
        bool displayed=presentation.Has("shown_sequence"); _placeholder.Visible=!displayed;
        _placeholder.Text = _running ? age>2000 ? "视频信号已中断" : "等待可解码的视频画面" : "等待视频画面";
        string source = data.Text("source"); _source.Text = "来源："+(source.Length==0 ? "等待数据" : source);
        _path.Text = $"解码：{data.Text("decoder")} · {data.Text("decode_device")}  |  传输：{data.Text("mode")}  |  {data.Number("width")} × {data.Number("height")}";
        if (data.Flag("color_defaulted")) _path.Text += " · 未标注颜色采用 SDR 默认值";
        _path.Text += $"\n渲染：{RenderingServer.GetVideoAdapterName()}";
        if (_cadencePolicy != null) _path.Text += "  |  " + _cadencePolicy.Description;
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
        // Godot's simulation delta may be clamped after a stalled frame or
        // scaled by Engine.TimeScale. Duration and reported rates use real time.
        double now = _wallClock.Elapsed.TotalSeconds;
        double elapsed = Math.Max(0, now - _uptime); _uptime = now;
        _diagnosticTime += elapsed; _elapsed += elapsed; _rateTime += elapsed;
        if (_elapsed < .25) return; _elapsed = 0;
        if (_session != null && !_finishing)
        {
            try
            {
                using var data = _session.Snapshot(false);
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
        record.Put("cadence_requested", Cadences[_cadence.Selected]); record.Put("cadence_applied", _cadencePolicy?.Applied ?? "engine");
        record.Put("engine_max_fps", Engine.MaxFps); record.Put("vsync_mode", (int)DisplayServer.WindowGetVsyncMode());
        record.Put("engine_process_frames", Engine.GetProcessFrames());
        record.Put("frame_timer_status", _cadencePolicy?.TimerStatus ?? "not requested");
        record.Put("window_mode", (int)DisplayServer.WindowGetMode());
        record.Put("window_can_draw", DisplayServer.WindowCanDraw());
        record.Put("display_refresh_hz", DisplayServer.ScreenGetRefreshRate(DisplayServer.WindowGetCurrentScreen()));
        string json = SnapshotAccess.Serialize(record);
        _diagnostics?.WriteLine(json); return json;
    }
    private async Task CaptureSnapshot()
    {
        if (File.Exists(_options.Snapshot)) throw new IOException("Snapshot output already exists");
        int drawn = 0;
        void OnFrameDrawn() => Interlocked.Exchange(ref drawn, 1);
        var clock = Stopwatch.StartNew();
        RenderingServer.FramePostDraw += OnFrameDrawn;
        try
        {
            // A minimized or non-drawing window may never emit FramePostDraw.
            // Keep the deadline on the main loop so capture cannot prevent stop.
            while (Volatile.Read(ref drawn) == 0)
            {
                if (clock.Elapsed.TotalSeconds >= 2)
                    throw new TimeoutException("Snapshot timed out waiting for a rendered frame");
                await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
            }
            using var image = GetViewport().GetTexture().GetImage();
            if (image.SavePng(_options.Snapshot) != Error.Ok) throw new IOException("Could not write snapshot");
        }
        finally { RenderingServer.FramePostDraw -= OnFrameDrawn; }
    }
    private async void FinishTimedRun()
    {
        int exitCode = 0;
        try
        {
            if (_options.Snapshot.Length > 0) await CaptureSnapshot();
        }
        catch (Exception error) { GD.PushError(error.Message); exitCode = 2; }
        try
        {
            var final = RecordDiagnostics();
            GD.Print("RM_FINAL ", final);
            using var finalCore = _session!.Snapshot(false); using var finalSurface = SnapshotAccess.Read(_surface, "get_metrics", false);
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
            _cadencePolicy?.Restore();
            using var stopped = new Godot.Collections.Dictionary();
            stopped.Put("event", stopError.Length > 0 ? "stop_failed" : "stopped"); stopped.Put("core", status);
            stopped.Put("cadence_restored", _cadencePolicy?.LaunchSettingsRestored ?? true);
            stopped.Put("frame_timer_status", _cadencePolicy?.TimerStatus ?? "not requested");
            stopped.Put("stop_elapsed_ms", stopClock.Elapsed.TotalMilliseconds); stopped.Put("stop_error", stopError); stopped.Put("exit_code", exitCode);
            string stoppedJson = SnapshotAccess.Serialize(stopped);
            _diagnostics?.WriteLine(stoppedJson);
            GD.Print(stopError.Length > 0 ? "RM_STOP_FAILED " : "RM_STOPPED ", stoppedJson);
            GetTree().Quit(exitCode);
        }
        finally { status.Dispose(); }
    }
    public override void _ExitTree() { _cadencePolicy?.Restore(); _diagnostics?.Dispose(); _session?.Dispose(); _session=null; }
}
