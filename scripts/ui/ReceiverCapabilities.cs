using Godot;
using System;
using System.Collections.Generic;
using System.Linq;

// Copy native enumeration into managed values once, outside the frame loop.
// Enumeration is not a promise about a stream profile or GPU interoperability.
public sealed class ReceiverCapabilities
{
    public sealed record Device(string Backend, int Ordinal, string Selector, string Name, string Error);
    public sealed record Backend(string Name, bool Compiled, string Error, IReadOnlyList<Device> Devices);
    private readonly Dictionary<string, Backend> _backends = new();
    public static ReceiverCapabilities Read(VideoSessionClient session)
    {
        var result = new ReceiverCapabilities();
        using var data = session.Capabilities();
        using var listValue = data["backends"];
        using var list = listValue.AsGodotArray();
        for (int index = 0; index < list.Count; ++index)
        {
            using var entryValue = list[index]; using var entry = entryValue.AsGodotDictionary();
            string name = entry.Text("name"); var devices = new List<Device>();
            using var devicesValue = entry["devices"]; using var nativeDevices = devicesValue.AsGodotArray();
            for (int deviceIndex = 0; deviceIndex < nativeDevices.Count; ++deviceIndex)
            {
                using var deviceValue = nativeDevices[deviceIndex]; using var device = deviceValue.AsGodotDictionary();
                devices.Add(new Device(name, (int)device.Number("ordinal"), device.Text("selector"), device.Text("name"), device.Text("error")));
            }
            result._backends[name] = new Backend(name, entry.Flag("compiled"), entry.Text("error"), devices);
        }
        return result;
    }
    public string UnavailableReason(string decoder)
    {
        if (decoder == "auto") return "";
        if (!_backends.TryGetValue(decoder, out var backend) || !backend.Compiled) return "此安装包未编入该解码方式";
        if (decoder == "software") return "";
        if (backend.Devices.Any(device => device.Error.Length == 0)) return "";
        return backend.Error.Length > 0 ? backend.Error : "未枚举到可访问设备；检查驱动或权限后刷新设备";
    }
    public IEnumerable<Device> Devices(string decoder) => _backends.Values
        .Where(backend => backend.Compiled && backend.Name != "software" && (decoder == "auto" || backend.Name == decoder))
        .SelectMany(backend => backend.Devices);

    public string GpuUnavailableReason(string decoder, string device, string renderer)
    {
        if (renderer != "gl_compatibility") return "GPU 传输需要 OpenGL 兼容渲染；可选择 OpenGL 后重启";
        if (decoder != "cuda") return "强制 GPU 传输需要明确选择 NVIDIA CUDA 解码";
        if (device.StartsWith("d3d11va:", StringComparison.Ordinal) || device.StartsWith("vaapi:", StringComparison.Ordinal) || device.StartsWith("/dev/", StringComparison.Ordinal))
            return "当前设备标识属于其他解码方式；GPU 传输需要 CUDA 设备";
        string cuda = UnavailableReason("cuda");
        return cuda.Length > 0 ? "CUDA 不可用：" + cuda : "";
    }
}
