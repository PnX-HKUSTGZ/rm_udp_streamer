using Godot;
using Godot.Collections;

// Snapshot dictionaries own native allocations that the managed GC cannot size.
// Dispose both the Call Variant and the dictionary copy at their actual scope.
public static class SnapshotAccess
{
    public static Dictionary Read(GodotObject source, string method, bool detailed = true)
    {
        using var name = new StringName(method);
        using var value = source.Call(name, detailed);
        return value.AsGodotDictionary();
    }
    public static string Serialize(Dictionary dictionary)
    {
        using var value = Variant.From(dictionary);
        return Json.Stringify(value);
    }
    public static string Text(this Dictionary dictionary, string key)
    {
        using var name = Variant.From(key);
        using var value = dictionary[name];
        return value.AsString();
    }
    public static long Number(this Dictionary dictionary, string key)
    {
        using var name = Variant.From(key);
        using var value = dictionary[name];
        return value.AsInt64();
    }
    public static bool Flag(this Dictionary dictionary, string key)
    {
        using var name = Variant.From(key);
        using var value = dictionary[name];
        return value.AsBool();
    }
    public static bool Has(this Dictionary dictionary, string key)
    {
        using var name = Variant.From(key);
        return dictionary.ContainsKey(name);
    }
    public static void Put<[MustBeVariant] T>(this Dictionary dictionary, string key, T value)
    {
        using var name = Variant.From(key);
        using var native = Variant.From(value);
        dictionary[name] = native;
    }
}
