using Godot;
using System;
using System.IO;

// Persist only reusable interactive settings. Timed runs, output filenames and
// auto-start are intentionally not carried into the next launch.
public static class ReceiverSettings
{
    private const string Path = "user://receiver.cfg";
    public static RuntimeOptions Load(out string warning)
    {
        warning = "";
        using var file = new ConfigFile();
        Error error = file.Load(Path);
        if (error == Error.FileNotFound) return new RuntimeOptions();
        try
        {
            if (error != Error.Ok) throw new IOException("配置文件无法读取：" + error);
            string Read(string key, string fallback)
            {
                using var value = file.GetValue("receiver", key, fallback);
                return value.AsString();
            }
            var options = RuntimeOptions.Parse(new[] { "--bind", Read("address", "0.0.0.0"), "--port", Read("port", "3334"),
                "--decoder", Read("decoder", "auto"), "--device", Read("device", "0"), "--transfer", Read("transfer", "yuv"),
                "--cadence", Read("cadence", "auto"), "--source-ip", Read("source_address", ""), "--source-port", Read("source_port", "0") });
            return options;
        }
        catch (Exception exception)
        {
            warning = "未加载保存的设置：" + exception.Message;
            return new RuntimeOptions();
        }
    }
    public static void Save(RuntimeOptions options)
    {
        options.Validate();
        using var file = new ConfigFile();
        void Put(string key, string value) => file.SetValue("receiver", key, value);
        Put("address", options.Address); Put("port", options.Port.ToString(System.Globalization.CultureInfo.InvariantCulture));
        Put("decoder", options.Decoder); Put("device", options.Device); Put("transfer", options.Transfer); Put("cadence", options.Cadence);
        Put("source_address", options.SourceAddress); Put("source_port", options.SourcePort.ToString(System.Globalization.CultureInfo.InvariantCulture));
        Error error = file.Save(Path + ".tmp");
        if (error != Error.Ok) throw new IOException("设置无法保存：" + error);
        File.Move(ProjectSettings.GlobalizePath(Path + ".tmp"), ProjectSettings.GlobalizePath(Path), true);
    }
}
