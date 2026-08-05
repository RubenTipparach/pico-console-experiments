using System.Text.Json;
using System.Text.Json.Serialization;

namespace PicoFlasher;

/// <summary>One .uf2 the library knows about, with whatever it says about itself.</summary>
public sealed record LibraryItem(string Path, GameMeta? Meta, int Slot, long Bytes,
                                 bool Forced = false)
{
    public string Title => Meta?.Title ?? System.IO.Path.GetFileName(Path);
    public string Version => Meta?.Version ?? "";

    /// <summary>
    /// Slot 0 means the launcher; -1 means base linked with our metadata, so
    /// a standalone build of one of our own games; -2 means base linked with
    /// no metadata and not named launcher.uf2, so something this project did
    /// not build at all: a homebrew .uf2 from anywhere else. Forced means a
    /// -2 item that was placed into a real slot anyway (see LibraryTab's
    /// force add); Slot becomes that real slot once forced, so IsSlotBuild
    /// is true for it too, and Forced is what still marks it as unverified.
    /// </summary>
    public bool IsLauncher => Slot == 0;
    public bool IsSlotBuild => Slot >= 1;
    public bool IsForeign => Slot == -2;

    public string Detail => IsLauncher ? "launcher"
        : Forced ? $"slot {Slot}, forced: not built for this project, may not boot"
        : IsSlotBuild ? $"slot {Slot} build"
        : IsForeign ? "not built by this project, cannot be bundled or used as the launcher"
        : "standalone, cannot be bundled";

    /// <summary>
    /// Same game built two different ways (a standalone build and a slot
    /// build for a bundle) lands as two same named, same icon tiles in the
    /// library. Nothing else in the icon view tells them apart, so the label
    /// carries it.
    /// </summary>
    public string ShortLabel => IsLauncher ? "launcher"
        : Forced ? $"slot {Slot}, forced"
        : IsSlotBuild ? $"slot {Slot}"
        : IsForeign ? "unrecognized"
        : "standalone";
}

/// <summary>One line of manifest.json: what a file is, written down by whatever built it.</summary>
internal sealed class ManifestEntry
{
    public string File { get; set; } = "";
    public string Role { get; set; } = "";
    public int? Slot { get; set; }
}

internal sealed class Manifest
{
    public List<ManifestEntry> Entries { get; set; } = new();
}

/// <summary>
/// The local library: built .uf2 files, with the name and icon read out of
/// each, plus manifest.json, which says what a file actually is when
/// something reliable already knows.
///
/// A file's role used to be guessed from its own bytes alone: where it
/// linked, whether it carried a metadata block. That guess is fundamentally
/// ambiguous for a slot 0, meta-less image (the real launcher and any other
/// base linked binary look identical), and it also meant an ordinary
/// standalone build, made for a completely unrelated reason, showed up here
/// uninvited just for existing in the wrong folder. build_bundle.bat knows
/// for a fact which slot a game was linked for, because it just told CMake
/// to link it there, so it writes the answer into manifest.json the moment
/// the file exists. A file the manifest has no entry for, or explicitly
/// calls "imported" (dropped in through Add file / drag and drop, so its
/// origin is unknown), still gets the byte based guess, because nothing else
/// knows what it is.
/// </summary>
public static class GameLibrary
{
    private const string ManifestFileName = "manifest.json";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public static string DefaultDirectory(string repositoryRoot) =>
        Path.Combine(repositoryRoot, "build.launcher", "library");

    /// <summary>Everything in the one canonical library directory.</summary>
    public static IReadOnlyList<LibraryItem> Scan(string directory)
    {
        var items = new List<LibraryItem>();
        if (!Directory.Exists(directory)) return items;

        var trusted = new Dictionary<string, ManifestEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in LoadManifest(directory).Entries)
        {
            if (entry.Role is "launcher" or "slot") trusted[entry.File] = entry;
        }

        try
        {
            foreach (var path in Directory.EnumerateFiles(directory, "*.uf2",
                                                          SearchOption.TopDirectoryOnly))
            {
                var item = trusted.TryGetValue(Path.GetFileName(path), out var entry)
                    ? ReadTrusted(path, entry)
                    : Read(path);
                if (item is not null) items.Add(item);
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }

        return items
            .OrderByDescending(item => item.IsLauncher)
            .ThenBy(item => item.Title, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    /// <summary>
    /// A file the manifest has a verified answer for: its role and slot come
    /// from there, not from address guessing. The file itself is still read
    /// for its title and icon, since the manifest does not carry those.
    /// </summary>
    private static LibraryItem? ReadTrusted(string path, ManifestEntry entry)
    {
        byte[] bytes;
        try
        {
            bytes = File.ReadAllBytes(path);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }

        if (!GameMetaReader.TryReadImage(bytes, out _, out var image)) return null;

        var slot = entry.Role == "launcher" ? 0 : entry.Slot ?? -2;
        var meta = GameMetaReader.Find(image);
        return new LibraryItem(path, meta, slot, bytes.Length);
    }

    /// <summary>
    /// Reads one file with no manifest entry to trust: the byte based guess,
    /// same as before this existed. This is what a foreign .uf2 gets, and
    /// what anything gets if manifest.json is missing or stale.
    /// </summary>
    public static LibraryItem? Read(string path)
    {
        byte[] bytes;
        try
        {
            bytes = File.ReadAllBytes(path);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }

        if (!GameMetaReader.TryReadImage(bytes, out var baseAddress, out var image))
        {
            return null;
        }

        // Where it was linked decides what can be done with it. A bundle needs
        // images at their slot addresses; anything at the base of flash is a
        // standalone game or the launcher itself.
        var slot = -1;
        if (baseAddress >= Bundle.FlashBase)
        {
            var offset = baseAddress - Bundle.FlashBase;
            if (offset % Bundle.SlotSize == 0)
            {
                var index = (int)(offset / Bundle.SlotSize);
                if (index >= 0 && index <= Bundle.SlotCount) slot = index;
            }
        }

        var meta = GameMetaReader.Find(image);
        // A base linked image is ambiguous by address alone: the launcher, an
        // ordinary standalone build of one of our own games, and literally
        // any other PicoSystem .uf2 someone hands the tool all link at the
        // very base of flash. Metadata narrows it to two cases, not one: our
        // own games always carry it, so meta present means a standalone
        // build, never the launcher. With no metadata it could genuinely be
        // the launcher, or it could be any other base linked binary with
        // nothing this tool put there, and address alone cannot tell those
        // apart. The name is the only remaining signal, so it is trusted
        // only when it matches what this project's own build always calls
        // the file: trusting any base linked, meta-less file as "the
        // launcher" would let Build() prepend a stranger's game in its place,
        // which is a bundle with a broken reset vector, not a warning.
        if (slot == 0)
        {
            if (meta is not null)
            {
                slot = -1;
            }
            else if (!string.Equals(Path.GetFileName(path), "launcher.uf2",
                                    StringComparison.OrdinalIgnoreCase))
            {
                slot = -2;
            }
        }

        return new LibraryItem(path, meta, slot, bytes.Length);
    }

    /// <summary>
    /// Copies a file into the library directory, which is how a downloaded CI
    /// artifact, or anything from outside this project, joins it. No manifest
    /// entry is written: an imported file's role is unknown by definition, so
    /// it gets the same byte based guess Read() has always made.
    /// </summary>
    public static string? Import(string sourcePath, string libraryDirectory)
    {
        try
        {
            Directory.CreateDirectory(libraryDirectory);
            var destination = Path.Combine(libraryDirectory,
                                           Path.GetFileName(sourcePath));
            File.Copy(sourcePath, destination, overwrite: true);
            return destination;
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    private static Manifest LoadManifest(string directory)
    {
        var path = Path.Combine(directory, ManifestFileName);
        try
        {
            if (!File.Exists(path)) return new Manifest();
            var json = File.ReadAllText(path);
            return JsonSerializer.Deserialize<Manifest>(json, JsonOptions) ?? new Manifest();
        }
        catch (IOException) { return new Manifest(); }
        catch (JsonException) { return new Manifest(); }
    }
}
