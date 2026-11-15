using Godot;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;

public partial class Main
{
    private ReceiverCapabilities? _capabilities;
    private OptionButton _devicePicker = null!, _renderer = null!;
    private LineEdit _sourceFilter = null!;
    private SpinBox _sourcePort = null!;
    private Button _refreshDevices = null!, _restartRenderer = null!;
    private Label _capabilityHint = null!;
    private readonly List<string> _selectors = new();
    private bool _updatingDevices, _restarting, _interactiveSettings, _configurationInitializing=true;
    private string _configurationBlock = "", _settingsNotice = "";
    private static readonly string[] Renderers = { "mobile", "gl_compatibility" };

    private void BuildConfiguration(VBoxContainer layout)
    {
        HBoxContainer Row()
        {
            var row = new HBoxContainer(); row.AddThemeConstantOverride("separation", 10); layout.AddChild(row); return row;
        }
        var inputs = Row(); inputs.AddChild(Text("监听地址"));
        _address = new LineEdit { Text="0.0.0.0", CustomMinimumSize=new Vector2(180,36) }; inputs.AddChild(_address);
        inputs.AddChild(Text("端口"));
        _port = new SpinBox { MinValue=1, MaxValue=65535, Step=1, Value=3334, CustomMinimumSize=new Vector2(100,36) }; inputs.AddChild(_port);
        _toggle = new Button { Text="开始接收", CustomMinimumSize=new Vector2(140,36) }; inputs.AddChild(_toggle); _toggle.Pressed += Toggle;

        var modes = Row(); modes.AddChild(Text("解码")); _decoder = new OptionButton(); modes.AddChild(_decoder);
        foreach (string value in new[] { "自动", "软件", "NVIDIA CUDA", "Windows D3D11", "Linux VAAPI" }) _decoder.AddItem(value);
        modes.AddChild(Text("设备"));
        var deviceArea = new VBoxContainer { SizeFlagsHorizontal=SizeFlags.ExpandFill }; modes.AddChild(deviceArea);
        _devicePicker = new OptionButton { FitToLongestItem=false, TextOverrunBehavior=TextServer.OverrunBehavior.TrimEllipsis,
            CustomMinimumSize=new Vector2(260,36) }; deviceArea.AddChild(_devicePicker);
        _device = new LineEdit { Text="0", MaxLength=512, Visible=false }; deviceArea.AddChild(_device);
        _device.TooltipText = "输入序号或完整标识。序号只属于所选解码方式；Windows LUID 仅在本次系统启动期间有效。";
        _refreshDevices = new Button { Text="刷新设备" }; modes.AddChild(_refreshDevices); _refreshDevices.Pressed += ReloadDevices;
        _decoder.ItemSelected += _ => { _settingsNotice=""; RebuildDevicePicker(); UpdateCapabilityHints(); };
        _devicePicker.ItemSelected += index =>
        {
            _updatingDevices=true;
            string selector=_selectors[(int)index]; _device.Visible=selector.Length==0;
            if (selector.Length>0) _device.Text=selector;
            _updatingDevices=false; _settingsNotice=""; UpdateCapabilityHints();
        };
        _device.TextChanged += _ => { if (!_updatingDevices) { _settingsNotice=""; UpdateCapabilityHints(); } };

        var paths = Row(); paths.AddChild(Text("传输")); _transfer=new OptionButton(); paths.AddChild(_transfer);
        foreach (string value in new[] { "CPU YUV", "CPU RGBA", "GPU CUDA / OpenGL", "自动 GPU / CPU" }) _transfer.AddItem(value);
        paths.AddChild(Text("节奏")); _cadence=new OptionButton(); paths.AddChild(_cadence);
        foreach (string value in new[] { "自动", "显示同步", "低延迟", "沿用引擎" }) _cadence.AddItem(value);
        _cadence.TooltipText="低延迟模式增加处理频率和功耗；OpenGL 下关闭显示同步，可能出现撕裂。处理频率不代表视频帧率。";
        _cadence.ItemSelected += _ => _cadencePolicy?.Apply(Cadences[_cadence.Selected], _cadenceActive);
        _transfer.ItemSelected += _ => { _settingsNotice=""; UpdateCapabilityHints(); };
        paths.AddChild(Text("渲染")); _renderer=new OptionButton(); paths.AddChild(_renderer);
        _renderer.AddItem("Vulkan Mobile"); _renderer.AddItem("OpenGL 兼容");
        string current=RenderingServer.GetCurrentRenderingMethod();
        int rendererIndex=Array.IndexOf(Renderers,current);
        if (rendererIndex<0) { _renderer.AddItem("当前："+current); rendererIndex=2; }
        _renderer.Select(rendererIndex);
        _restartRenderer=new Button { Text="重启并切换", TooltipText="停止当前接收，保留连接、来源过滤和设备设置，然后重启应用。重启后手动开始接收。" };
        paths.AddChild(_restartRenderer); _restartRenderer.Pressed += RestartRenderer;
        _renderer.ItemSelected += _ => UpdateCapabilityHints();

        var sources=Row(); sources.AddChild(Text("限定来源 IP"));
        _sourceFilter=new LineEdit { PlaceholderText="留空：任意来源", MaxLength=15, CustomMinimumSize=new Vector2(180,36) }; sources.AddChild(_sourceFilter);
        sources.AddChild(Text("来源端口"));
        _sourcePort=new SpinBox { MinValue=0, MaxValue=65535, Step=1, CustomMinimumSize=new Vector2(100,36) }; sources.AddChild(_sourcePort);
        sources.AddChild(Text("0：任意端口",14));
        _sourceFilter.TooltipText="填写发送端的数字 IPv4 地址；与来源端口过滤同时生效。";
        _capabilityHint=Text("正在读取设备能力…",14); _capabilityHint.AutowrapMode=TextServer.AutowrapMode.WordSmart; layout.AddChild(_capabilityHint);
        _address.TextChanged += _ => ClearConfigurationNotice(); _port.ValueChanged += _ => ClearConfigurationNotice();
        _sourceFilter.TextChanged += _ => ClearConfigurationNotice(); _sourcePort.ValueChanged += _ => ClearConfigurationNotice();
    }
    private void ClearConfigurationNotice() { if (!_configurationInitializing) _settingsNotice=""; UpdateCapabilityHints(); }

    private RuntimeOptions ReadConfiguration() => new()
    {
        Address=_address.Text.Trim(), Port=(int)_port.Value, Decoder=Decoders[_decoder.Selected], Device=_device.Text.Trim(),
        Transfer=Transfers[_transfer.Selected], Cadence=Cadences[_cadence.Selected], SourceAddress=_sourceFilter.Text.Trim(), SourcePort=(int)_sourcePort.Value
    };
    private void ReloadDevices()
    {
        if (_session==null) return;
        try
        {
            _capabilities=ReceiverCapabilities.Read(_session);
            for (int index=0;index<Decoders.Length;++index)
            {
                string reason=_capabilities.UnavailableReason(Decoders[index]);
                _decoder.SetItemDisabled(index,reason.Length>0); _decoder.SetItemTooltip(index,reason);
            }
            RebuildDevicePicker(); UpdateCapabilityHints();
        }
        catch (Exception error) { _settingsNotice="设备枚举失败："+error.Message; UpdateCapabilityHints(); }
    }
    private void RebuildDevicePicker()
    {
        if (_capabilities==null) return;
        _updatingDevices=true;
        try
        {
            string selected=_device.Text.Trim(); _devicePicker.Clear(); _selectors.Clear();
            void Add(string title,string selector,string tooltip,bool disabled=false)
            {
                int index=_selectors.Count; _selectors.Add(selector); _devicePicker.AddItem(title);
                _devicePicker.SetItemTooltip(index,tooltip); _devicePicker.SetItemDisabled(index,disabled);
            }
            Add("默认设备（序号 0）","0","自动解码按各后端的序号 0 尝试；固定设备请从列表选择完整标识。");
            foreach (var device in _capabilities.Devices(Decoders[_decoder.Selected]))
                Add(device.Backend+" · "+device.Name,device.Selector,device.Selector+(device.Error.Length>0?"\n"+device.Error:""),device.Error.Length>0);
            Add("手动输入标识或序号…","","未枚举到的设备仍须通过实际初始化核对，不会悄悄替换为另一张硬件设备。");
            int index=_selectors.IndexOf(selected); if(index<0) index=_selectors.Count-1;
            _devicePicker.Select(index); _device.Visible=index==_selectors.Count-1;
        }
        finally { _updatingDevices=false; }
    }
    private void UpdateCapabilityHints()
    {
        if (_capabilityHint==null) return;
        string current=RenderingServer.GetCurrentRenderingMethod();
        _restartRenderer.Disabled=_restarting || _renderer.Selected>=Renderers.Length || Renderers[_renderer.Selected]==current || _options.Duration>0;
        if (_capabilities==null) { _capabilityHint.Text=_settingsNotice.Length>0?_settingsNotice:"正在读取设备能力…"; return; }
        string decoder=Decoders[_decoder.Selected], transfer=Transfers[_transfer.Selected];
        string backendError=_capabilities.UnavailableReason(decoder);
        string gpuError=_capabilities.GpuUnavailableReason(decoder,_device.Text.Trim(),current);
        _transfer.SetItemDisabled(2,gpuError.Length>0); _transfer.SetItemTooltip(2,gpuError.Length>0?gpuError:"需要实际验证 CUDA 与渲染设备匹配及 NV12 8 位输入。");
        _configurationBlock=backendError.Length>0?backendError:transfer=="gpu"?gpuError:"";
        string message=_configurationBlock.Length>0?_configurationBlock:
            transfer=="auto"?"尝试 GPU 传输；设备、渲染或输入不满足时回退 CPU YUV，实际路径与原因显示在下方。":
            transfer=="gpu"?"GPU 传输候选：启动后核对 CUDA 与渲染设备匹配；不满足时明确失败。":
            "设备列表只证明可枚举；输入格式、硬件回读和性能以实际接收结果为准。";
        if (_device.Text.StartsWith("d3d11va:",StringComparison.Ordinal)) message+=" Windows 设备标识仅在本次系统启动期间有效。";
        _capabilityHint.Text=(_settingsNotice.Length>0?_settingsNotice+"\n":"")+message;
    }
    private void SetConfigurationEditable(bool editable)
    {
        _address.Editable=editable; _port.Editable=editable; _device.Editable=editable; _sourceFilter.Editable=editable; _sourcePort.Editable=editable;
        _decoder.Disabled=!editable; _transfer.Disabled=!editable; _devicePicker.Disabled=!editable; _refreshDevices.Disabled=!editable;
        _renderer.Disabled=_restarting; _cadence.Disabled=_restarting;
    }
    private async void RestartRenderer()
    {
        if (_session==null || _restarting || _renderer.Selected>=Renderers.Length) return;
        string renderer=Renderers[_renderer.Selected];
        if (renderer==RenderingServer.GetCurrentRenderingMethod()) return;
        try
        {
            var settings=ReadConfiguration(); settings.Validate(); ReceiverSettings.Save(settings);
            _restarting=true; _finishing=true; UpdateCapabilityHints();
            _session.Stop(); _surface.Call("clear");
            var clock=Stopwatch.StartNew();
            while (true)
            {
                _session.Native.Call("poll_stop");
                using var state=_session.Snapshot(false);
                if(state.Flag("stop_terminal")) break;
                if(clock.Elapsed.TotalSeconds>=5) throw new TimeoutException("停止未在 5 秒内完成；未自动重启，请关闭应用后重新打开。");
                await ToSignal(GetTree(),SceneTree.SignalName.ProcessFrame);
            }
            _cadencePolicy?.Restore();
            var args=new List<string>();
            if(OS.HasFeature("editor")) args.AddRange(new[] { "--path",ProjectSettings.GlobalizePath("res://") });
            args.AddRange(new[] { "--rendering-method",renderer,"--","--bind",settings.Address,"--port",settings.Port.ToString(CultureInfo.InvariantCulture),
                "--decoder",settings.Decoder,"--device",settings.Device,"--transfer",settings.Transfer,"--cadence",settings.Cadence,
                "--source-port",settings.SourcePort.ToString(CultureInfo.InvariantCulture) });
            if(settings.SourceAddress.Length>0) args.AddRange(new[] { "--source-ip",settings.SourceAddress });
            OS.SetRestartOnExit(true,args.ToArray()); GetTree().Quit();
        }
        catch(Exception error)
        {
            _restarting=false; _finishing=false; _settingsNotice=error.Message; UpdateCapabilityHints();
        }
    }
}
