using System;
using System.Globalization;
using System.IO;

// Deployment and diagnostic options are passed after Godot's -- separator.
public sealed class RuntimeOptions
{
    public bool Start, Help;
    public string Address = "0.0.0.0", Decoder = "auto", Transfer = "yuv";
    public int Port = 3334, Device;
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
                case "--device": result.Device = int.Parse(value, CultureInfo.InvariantCulture); break;
                case "--decoder": result.Decoder = value; break;
                case "--transfer": result.Transfer = value; break;
                case "--duration": result.Duration = double.Parse(value, CultureInfo.InvariantCulture); break;
                case "--metrics": result.Metrics = Path.GetFullPath(value); break;
                case "--snapshot": result.Snapshot = Path.GetFullPath(value); break;
                default: throw new ArgumentException("Unknown receiver option: " + option);
            }
        }
        if (result.Port < 1 || result.Port > 65535 || result.Device < 0 || result.Device > 999)
            throw new ArgumentException("Port or device index is outside its valid range");
        if (Array.IndexOf(new[] { "auto", "software", "cuda", "d3d11va" }, result.Decoder) < 0 ||
            Array.IndexOf(new[] { "yuv", "rgba" }, result.Transfer) < 0)
            throw new ArgumentException("Unknown decoder or transfer mode");
        if (!double.IsFinite(result.Duration) || result.Duration < 0 || result.Duration > 86400)
            throw new ArgumentException("Duration must be 0 (unlimited) through 86400 seconds");
        if (result.Snapshot.Length > 0 && (result.Duration == 0 || File.Exists(result.Snapshot)))
            throw new ArgumentException("Snapshot needs a finite duration and a new output filename");
        return result;
    }
    public const string Usage = "Receiver options (after --): --start --bind IPv4 --port 3334 " +
        "--decoder auto|software|cuda|d3d11va --device 0 --transfer yuv|rgba " +
        "--duration seconds --metrics new-file.ndjson --snapshot new-file.png\n" +
        "Duration starts when the interface opens; 0 runs until closed. Metrics record actual new texture submissions, not screen refreshes.";
}
