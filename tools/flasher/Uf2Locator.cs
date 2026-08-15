namespace PicoFlasher;

public sealed record Uf2File(string Path, string Display)
{
    public override string ToString() => Display;
}

/// <summary>
/// Finds .uf2 files worth offering, newest first.
///
/// Looks in the usual build output directories relative to the repository root,
/// then anywhere the user pointed the app. Nothing here is clever on purpose:
/// the point of this tool is that it already knows where the build put things.
/// </summary>
public static class Uf2Locator
{
    private static readonly string[] SearchGlobs =
    {
        // Where build_console.bat puts console.uf2, first because the console
        // is the thing most likely to be wanted.
        "build.console",
        "build.pico",
        // build_uf2s_tufty.bat. A separate directory rather than a second
        // board writing into build.pico, because the two produce a .uf2 of
        // the same name for different CPUs and only one of them will boot the
        // device in your hand. BootselWatcher already tells them apart by
        // volume label (RPI-RP2 against RP2350), so the wrong pairing is
        // visible in the window rather than silent.
        "build.tufty",
        "build",
        "dist",
    };

    public static IReadOnlyList<Uf2File> Find(string rootDirectory)
    {
        var results = new List<Uf2File>();
        if (!Directory.Exists(rootDirectory)) return results;

        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var relative in SearchGlobs)
        {
            var directory = Path.Combine(rootDirectory, relative);
            if (!Directory.Exists(directory)) continue;
            Collect(directory, results, seen);
        }

        // Also take anything sitting directly in the root, which is where a
        // download from the gallery or a CI artifact usually lands.
        Collect(rootDirectory, results, seen, recurse: false);

        return results
            .OrderByDescending(file => SafeWriteTime(file.Path))
            .ToList();
    }

    private static void Collect(string directory, List<Uf2File> results,
                                HashSet<string> seen, bool recurse = true)
    {
        try
        {
            var option = recurse ? SearchOption.AllDirectories
                                 : SearchOption.TopDirectoryOnly;
            foreach (var path in Directory.EnumerateFiles(directory, "*.uf2", option))
            {
                if (!seen.Add(path)) continue;
                var stamp = SafeWriteTime(path).ToString("yyyy-MM-dd HH:mm");
                var size = SafeLength(path) / 1024;
                results.Add(new Uf2File(path,
                    $"{Path.GetFileName(path)}   {size} KB   {stamp}"));
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static DateTime SafeWriteTime(string path)
    {
        try { return File.GetLastWriteTime(path); }
        catch (IOException) { return DateTime.MinValue; }
    }

    private static long SafeLength(string path)
    {
        try { return new FileInfo(path).Length; }
        catch (IOException) { return 0; }
    }
}
