using Godot;
using System;

public partial class Main : Control
{
    private VideoSessionClient? _session;
    private LineEdit _address = null!;
    private SpinBox _port = null!;
    private Button _toggle = null!;
    private Label _state = null!, _metrics = null!, _source = null!, _error = null!;
    private bool _running;
    private double _elapsed;

    public override void _Ready()
    {
        BuildInterface();
        try { _session = new VideoSessionClient(); Refresh(); }
        catch (Exception error) { _error.Text = error.Message; _toggle.Disabled = true; }
    }
    private static Label Text(string value, int size = 16)
    {
        var label = new Label { Text = value };
        label.AddThemeFontSizeOverride("font_size", size);
        return label;
    }
    private void BuildInterface()
    {
        var background = new ColorRect { Color = new Color("10151d"), MouseFilter = MouseFilterEnum.Ignore };
        AddChild(background); background.SetAnchorsAndOffsetsPreset(LayoutPreset.FullRect);
        var margin = new MarginContainer();
        AddChild(margin); margin.SetAnchorsAndOffsetsPreset(LayoutPreset.FullRect);
        foreach (string side in new[] { "left", "right", "top", "bottom" }) margin.AddThemeConstantOverride("margin_" + side, 24);
        var layout = new VBoxContainer(); layout.AddThemeConstantOverride("separation", 16); margin.AddChild(layout);
        var heading = new HBoxContainer(); layout.AddChild(heading);
        var title = Text("RM 视频接收", 26); title.SizeFlagsHorizontal = SizeFlags.ExpandFill; heading.AddChild(title);
        _state = Text("已停止"); heading.AddChild(_state);
        var inputs = new HBoxContainer(); inputs.AddThemeConstantOverride("separation", 12); layout.AddChild(inputs);
        inputs.AddChild(Text("监听地址"));
        _address = new LineEdit { Text = "0.0.0.0", CustomMinimumSize = new Vector2(220, 40) }; inputs.AddChild(_address);
        inputs.AddChild(Text("端口"));
        _port = new SpinBox { MinValue = 1, MaxValue = 65535, Step = 1, Value = 3334, CustomMinimumSize = new Vector2(140,40) }; inputs.AddChild(_port);
        _toggle = new Button { Text = "开始接收", CustomMinimumSize = new Vector2(140,40) }; inputs.AddChild(_toggle);
        _toggle.Pressed += Toggle;
        var viewport = new PanelContainer { SizeFlagsVertical = SizeFlags.ExpandFill };
        var style = new StyleBoxFlat { BgColor = new Color("080c12"), BorderColor = new Color("293342") };
        style.SetBorderWidthAll(1); style.SetCornerRadiusAll(12); viewport.AddThemeStyleboxOverride("panel", style);
        layout.AddChild(viewport);
        var center = new CenterContainer(); viewport.AddChild(center);
        var placeholder = new VBoxContainer(); center.AddChild(placeholder);
        var waiting = Text("等待视频画面", 24); waiting.HorizontalAlignment = HorizontalAlignment.Center; placeholder.AddChild(waiting);
        var phase = Text("当前阶段：协议接收与诊断，解码尚未启用", 14); phase.Modulate = new Color("91a1b5"); placeholder.AddChild(phase);
        _source = Text("来源：等待数据"); layout.AddChild(_source);
        _metrics = Text("数据包 0 · 完整传输组 0 · 缓冲 0 KiB"); layout.AddChild(_metrics);
        _error = Text(""); _error.Modulate = new Color("ffb4a9"); _error.AutowrapMode = TextServer.AutowrapMode.WordSmart; layout.AddChild(_error);
    }
    private void Toggle()
    {
        if (_session == null) return;
        if (_running) _session.Stop(); else _session.Start(_address.Text.Trim(), (int)_port.Value);
        Refresh();
    }
    private void Refresh()
    {
        if (_session == null) return;
        var data = _session.Snapshot();
        _running = data["running"].AsBool();
        _state.Text = _running ? "正在接收 · 协议模式" : "已停止";
        _toggle.Text = _running ? "停止接收" : "开始接收";
        _address.Editable = !_running; _port.Editable = !_running;
        string source = data["source"].AsString();
        _source.Text = "来源：" + (source.Length == 0 ? "等待数据" : source);
        _metrics.Text = $"数据包 {data["packets"]} · 完整传输组 {data["complete"]} · 重组缓冲 {data["pending_bytes"].AsInt64()/1024} KiB" +
            $" · 超时 {data["timeouts"]} · 异常包 {data["invalid"]} · 来源不符 {data["foreign"]}";
        _error.Text = data["error"].AsString();
    }
    public override void _Process(double delta)
    {
        _elapsed += delta;
        if (_elapsed < .25) return;
        _elapsed = 0; Refresh();
    }
    public override void _ExitTree() { _session?.Dispose(); _session = null; }
}
