using System;
using System.Runtime.InteropServices;

// Windows 11 may ignore a windowed process's timer-resolution request when
// occluded or minimized. Own only this process's timer flag and a balanced
// multimedia timer request while the user selects responsive presentation.
public sealed class WindowsFrameTimer
{
    private const int ProcessPowerThrottling = 4;
    private const uint IgnoreTimerResolution = 4;
    [StructLayout(LayoutKind.Sequential)]
    private struct PowerState { public uint Version, ControlMask, StateMask; }
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessInformation(IntPtr process, int kind, ref PowerState value, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetProcessInformation(IntPtr process, int kind, in PowerState value, uint size);
    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();
    [DllImport("winmm.dll")]
    private static extern uint timeBeginPeriod(uint period);
    [DllImport("winmm.dll")]
    private static extern uint timeEndPeriod(uint period);

    private PowerState _initial;
    private bool _periodActive, _policyActive;
    public string Status { get; private set; } = "not requested";
    public bool Restored => !_periodActive && !_policyActive;
    public bool RequestSucceeded => _periodActive && _policyActive;

    private static bool ReadState(out PowerState state)
    {
        state = new PowerState { Version = 1 };
        return GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, ref state, 12);
    }
    public void Request()
    {
        if (!OperatingSystem.IsWindows() || !Restored) return;
        string policy;
        if (ReadState(out _initial))
        {
            var requested = _initial;
            requested.ControlMask |= IgnoreTimerResolution;
            requested.StateMask &= ~IgnoreTimerResolution;
            _policyActive = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, in requested, 12);
            policy = _policyActive ? "honor while occluded" : "policy request error " + Marshal.GetLastWin32Error();
        }
        else policy = "policy query error " + Marshal.GetLastWin32Error();
        uint result = timeBeginPeriod(1);
        _periodActive = result == 0;
        Status = (_periodActive ? "1 ms requested; " : "timer request error " + result + "; ") + policy;
    }
    public void Restore()
    {
        if (!OperatingSystem.IsWindows() || Restored) return;
        string errors = "";
        if (_periodActive)
        {
            uint result = timeEndPeriod(1);
            if (result == 0) _periodActive = false;
            else errors = "timer restore error " + result;
        }
        if (_policyActive)
        {
            if (ReadState(out var current))
            {
                // Preserve other flags that another component may have changed.
                current.ControlMask = (current.ControlMask & ~IgnoreTimerResolution) | (_initial.ControlMask & IgnoreTimerResolution);
                current.StateMask = (current.StateMask & ~IgnoreTimerResolution) | (_initial.StateMask & IgnoreTimerResolution);
                if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, in current, 12)) _policyActive = false;
                else errors += " policy restore error " + Marshal.GetLastWin32Error();
            }
            else errors += " policy restore query error " + Marshal.GetLastWin32Error();
        }
        Status = Restored ? "restored" : errors.Trim();
    }
}
