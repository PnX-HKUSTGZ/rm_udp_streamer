using Godot;
using System;

// This standalone receiver controls its own main-loop/display policy while a
// session is active. Restore the launch settings when the session stops.
public sealed class FrameCadence
{
    private readonly int _initialMaxFps = Engine.MaxFps;
    private readonly DisplayServer.VSyncMode _initialVsync = DisplayServer.WindowGetVsyncMode();
    private readonly string _automatic;
    public string Applied { get; private set; } = "engine";
    public string Description { get; private set; } = "沿用引擎设置";
    public bool LaunchSettingsRestored => Engine.MaxFps == _initialMaxFps && DisplayServer.WindowGetVsyncMode() == _initialVsync;

    public FrameCadence()
    {
        string adapter = RenderingServer.GetVideoAdapterName().ToLowerInvariant();
        bool software = RenderingServer.GetVideoAdapterType() == RenderingDevice.DeviceType.Cpu ||
            adapter.Contains("llvmpipe") || adapter.Contains("lavapipe") || adapter.Contains("swiftshader") ||
            adapter.Contains("software") || adapter.Contains("microsoft basic render");
        // Godot removes processed engine flags from GetCmdlineArgs. Inspect
        // their effective settings instead; use --cadence engine to preserve
        // even a deliberately uncapped, otherwise-default launch policy.
        bool explicitEnginePolicy = _initialMaxFps != 0 || _initialVsync != DisplayServer.VSyncMode.Enabled;
        // Current evidence covers Windows hardware rendering. Keep Linux and
        // software-rendered desktops on their launch policy until validated.
        _automatic = OperatingSystem.IsWindows() && DisplayServer.GetName() != "headless" &&
            adapter.Length > 0 && !software && !explicitEnginePolicy ? "responsive" : "engine";
    }
    public void Apply(string requested, bool active)
    {
        string policy = !active ? "engine" : requested == "auto" ? _automatic : requested;
        if (policy == Applied) return;
        switch (policy)
        {
            case "responsive":
                bool compatibility = RenderingServer.GetCurrentRenderingMethod() == "gl_compatibility";
                DisplayServer.WindowSetVsyncMode(compatibility ? DisplayServer.VSyncMode.Disabled : DisplayServer.VSyncMode.Mailbox);
                Engine.MaxFps = 120;
                Description = compatibility ? "低延迟 · 120 次/秒处理上限，关闭显示同步" : "低延迟 · 120 次/秒处理上限，Mailbox 显示同步";
                break;
            case "display":
                DisplayServer.WindowSetVsyncMode(DisplayServer.VSyncMode.Enabled); Engine.MaxFps = 0;
                Description = "显示同步";
                break;
            default:
                DisplayServer.WindowSetVsyncMode(_initialVsync); Engine.MaxFps = _initialMaxFps;
                Description = "沿用引擎设置";
                break;
        }
        Applied = policy;
    }
    public void Restore() => Apply("engine", false);
}
