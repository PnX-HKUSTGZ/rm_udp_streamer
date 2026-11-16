using Godot;
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json;

// The bundled hint was captured from this fixed sender, not synthesized.
// Provenance and the user's opt-in are recorded in docs/usage.md.
public sealed class HevcParameterCache
{
    public const string CachePath = "user://hevc-parameters.json";
    private const string BundledSource = "192.168.12.1";
    private const string BundledBytes = "AAAAAUABDAH//wFgAAADALAAAAMAAAMAnCwJAAAAAUIBAQFgAAADALAAAAMAAAMAnKADwIARB8vLkkKVkuP/AAEAAWoCAgIIAAADAAgAAAMB5iAAAAABRAHg98BMkA==";
    public string Source { get; private set; } = BundledSource;
    public byte[] Bytes { get; private set; } = Convert.FromBase64String(BundledBytes);
    public string Origin { get; private set; } = "内置启动参数";

    private static void ValidateSource(string source)
    {
        if (!IPAddress.TryParse(source, out var ip) || ip.AddressFamily != AddressFamily.InterNetwork ||
            ip.ToString() != source || source == "0.0.0.0")
            throw new IOException("参数缓存的发送端地址无效");
    }
    private static void ValidateBytes(VideoSessionClient session, byte[] bytes)
    {
        string error = session.ValidateHevcParameters(bytes);
        if (error.Length > 0) throw new IOException("参数缓存无效：" + error);
    }
    public static HevcParameterCache Load(VideoSessionClient session, out string warning)
    {
        var result = new HevcParameterCache(); warning = "";
        string path = ProjectSettings.GlobalizePath(CachePath);
        try
        {
            if (System.IO.File.Exists(path))
            {
                using var stream = System.IO.File.OpenRead(path);
                if (stream.Length is <= 0 or > 524288) throw new IOException("参数缓存文件大小无效");
                using var document = JsonDocument.Parse(stream);
                var value = document.RootElement;
                if (value.GetProperty("schema").GetInt32() != 1) throw new IOException("参数缓存版本无效");
                string source = value.GetProperty("source_address").GetString() ?? "";
                var bytes = value.GetProperty("parameters").GetBytesFromBase64();
                ValidateSource(source); ValidateBytes(session, bytes);
                if (Convert.ToHexString(SHA256.HashData(bytes)) != value.GetProperty("sha256").GetString())
                    throw new IOException("参数缓存校验失败");
                result.Source = source; result.Bytes = bytes; result.Origin = "已保存的启动参数";
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or FormatException or InvalidOperationException or KeyNotFoundException)
        {
            warning = "未加载参数缓存，使用内置启动参数：" + error.Message;
        }
        ValidateBytes(session, result.Bytes);
        return result;
    }
    public void SaveValidated(VideoSessionClient session, string source, byte[] bytes)
    {
        ValidateSource(source); ValidateBytes(session, bytes);
        string path = ProjectSettings.GlobalizePath(CachePath);
        if (Source == source && Bytes.AsSpan().SequenceEqual(bytes) && System.IO.File.Exists(path) && Origin != "内置启动参数") return;
        var record = new { schema = 1, source_address = source, parameters = bytes,
            sha256 = Convert.ToHexString(SHA256.HashData(bytes)), saved_at_utc = DateTimeOffset.UtcNow };
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path)!);
        // A unique sibling and atomic rename preserve the prior complete record
        // on interruption and keep concurrent receiver processes from sharing a temp file.
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var output = new FileStream(temporary, FileMode.CreateNew, System.IO.FileAccess.Write, FileShare.None))
            {
                JsonSerializer.Serialize(output, record); output.Flush(true);
            }
            System.IO.File.Move(temporary, path, true);
            Source = source; Bytes = bytes; Origin = "已保存的启动参数";
        }
        finally { if (System.IO.File.Exists(temporary)) System.IO.File.Delete(temporary); }
    }
}
