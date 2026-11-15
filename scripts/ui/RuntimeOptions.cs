using System;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;

// Deployment and diagnostic options are passed after Godot's -- separator.
public sealed class RuntimeOptions
{
    public bool Start, Help;
    public bool? Diagnostics;
    public string Address = "0.0.0.0", Decoder = "auto", Transfer = "yuv", Cadence = "auto";
    public int Port = 3334;
    public string SourceAddress = "";
    public int SourcePort;
    public string Device = "0";
    public double Duration;
    public string Metrics = "", Snapshot = "";
    public static RuntimeOptions Parse(string[] args)
    {
        var result = new RuntimeOptions();
        for (int i = 0; i < args.Length; ++i)
        {
            string option = args[i];
            if (option == "--start") { result.Start = true; continue; }
            if (option == "--help") { result.Help = true; continue; }
            if (++i == args.Length) throw new ArgumentException("Missing value for " + option);
            string value = args[i];
            switch (option)
            {
                case "--bind": result.Address = value; break;
                case "--port": result.Port = int.Parse(value, CultureInfo.InvariantCulture); break;
                case "--source-ip": result.SourceAddress = value; break;
                case "--source-port": result.SourcePort = int.Parse(value, CultureInfo.InvariantCulture); break;
                case "--device": result.Device = value; break;
                case "--decoder": result.Decoder = value; break;
                case "--transfer": result.Transfer = value; break;
                case "--cadence": result.Cadence = value; break;
                case "--duration": result.Duration = double.Parse(value, CultureInfo.InvariantCulture); break;
                case "--metrics": result.Metrics = Path.GetFullPath(value); break;
                case "--snapshot": result.Snapshot = Path.GetFullPath(value); break;
                case "--diagnostics": result.Diagnostics = value switch { "on" => true, "off" => false, _ => throw new ArgumentException("Diagnostics must be on or off") }; break;
                default: throw new ArgumentException("Unknown receiver option: " + option);
            }
        }
        result.Validate();
        return result;
    }
    public void Validate()
    {
        if (Port < 1 || Port > 65535 || Device.Length == 0 || Device.Length > 512)
            throw new ArgumentException("Port or device selector is outside its valid range");
        if (SourcePort < 0 || SourcePort > 65535)
            throw new ArgumentException("Source port must be 0 (any) through 65535");
        if (SourceAddress.Length > 0 && (!IPAddress.TryParse(SourceAddress, out var source) ||
            source.AddressFamily != AddressFamily.InterNetwork || source.ToString() != SourceAddress))
            throw new ArgumentException("Source IP must be a complete numeric IPv4 address, or empty for any source");
        if (Array.IndexOf(new[] { "auto", "software", "cuda", "d3d11va", "vaapi" }, Decoder) < 0 ||
            Array.IndexOf(new[] { "yuv", "rgba", "gpu", "auto" }, Transfer) < 0)
            throw new ArgumentException("Unknown decoder or transfer mode");
        if (Array.IndexOf(new[] { "auto", "display", "responsive", "engine" }, Cadence) < 0)
            throw new ArgumentException("Cadence must be auto, display, responsive or engine");
        if (!double.IsFinite(Duration) || Duration < 0 || Duration > 86400)
            throw new ArgumentException("Duration must be 0 (unlimited) through 86400 seconds");
        if (Snapshot.Length > 0 && (Duration == 0 || File.Exists(Snapshot)))
            throw new ArgumentException("Snapshot needs a finite duration and a new output filename");
    }
    public const string Usage = "Receiver options (after --): --start --bind IPv4 --port 3334 --source-ip IPv4 --source-port 0 " +
        "--decoder auto|software|cuda|d3d11va|vaapi --device index-or-identity --transfer yuv|rgba|gpu|auto " +
        "--duration seconds --metrics new-file.ndjson --snapshot new-file.png --diagnostics on|off --cadence auto|display|responsive|engine\n" +
        "Auto uses a bounded 120 Hz loop on Windows hardware renderers (Mailbox on Vulkan; VSync off on OpenGL). " +
        "Linux/software rendering, nonzero launch FPS caps and nondefault VSync retain their launch policy. Display uses VSync; engine always preserves launch settings.\n" +
        "Device accepts a backend-local index, cuda:GPU-UUID, d3d11va:LUID=high:low (current boot), " +
        "vaapi:pci:domain:bus:slot.function or a /dev/dri render path. Auto with an identity tries only that backend, then software.\n" +
        "Stage timings default to on when a metrics file is requested; off keeps counters and disables extra timing.\n" +
        "An empty source IP and source port 0 accept any source; nonempty filters are checked before assembly.\n" +
        "Interactive launches restore saved connection/device settings. Explicit command-line options start from documented defaults.\n" +
        "Duration starts when the interface opens; 0 runs until closed. Metrics record actual new texture submissions, not screen refreshes.";
}
