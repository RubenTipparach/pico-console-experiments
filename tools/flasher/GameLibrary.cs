namespace PicoFlasher;

/// <summary>One .uf2 the library knows about, with whatever it says about itself.</summary>
public sealed record LibraryItem(string Path, GameMeta? Meta, int Slot, long Bytes)
{
    public string Title => Meta?.Title ?? System.IO.Path.GetFileName(Path);
    public string Version => Meta?.Version ?? "";

    /// <summary>Slot 0 means the launcher; -1 means base linked, so standalone.</summary>
    public bool IsLauncher => Slot == 0;
    public bool IsSlotBuild => Slot >= 1;

    public string Detail => IsLauncher
        ? "launcher"
        : IsSlotBuild ? $"slot {Slot} build" : "standalone, cannot be bundled";
}

/// <summary>
/// The local library: built .uf2 files, with the name and icon read out of each.
///
/// It is a plain directory rather than a database, so it can be filled by
/// dropping CI artifacts into it, and a game is only ever a file. The default
/// lives in `library/` at the repo root, which is gitignored.
///
/// Bundling needs slot linked builds, so this records which slot each image was
/// linked for, taken from the address its blocks actually target. A standalone
/// .uf2 (the kind you flash on its own) is linked at the base of flash and is
/// listed as such rather than silently offered for a bundle it cannot join.
/// </summary>
public static class GameLibrary
{
    public static string DefaultDirectory(string repositoryRoot) =>
        Path.Combine(repositoryRoot, "library");

    /// <summary>
    /// Everything in the directory, plus anything under the build tree that
    /// looks like a slot build, so a fresh local build shows up without being
    /// copied anywhere first.
    /// </summary>
    public static IReadOnlyList<LibraryItem> Scan(string directory,
                                                  string? repositoryRoot = null)
    {
        var items = new List<LibraryItem>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        Collect(directory, items, seen);

        if (repositoryRoot is not null)
        {
            // A slot build lands here when the workflow's cmake line is run by
            // hand, which is how anyone iterating on a game gets one.
            foreach (var name in new[] { "build.slot", "build.launcher", "build.pico" })
            {
                Collect(Path.Combine(repositoryRoot, name), items, seen);
            }
        }

        return items
            .OrderByDescending(item => item.IsLauncher)
            .ThenBy(item => item.Title, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    private static void Collect(string directory, List<LibraryItem> items,
                                HashSet<string> seen)
    {
        if (!Directory.Exists(directory)) return;
        try
        {
            foreach (var path in Directory.EnumerateFiles(directory, "*.uf2",
                                                          SearchOption.AllDirectories))
            {
                if (!seen.Add(path)) continue;
                var item = Read(path);
                if (item is not null) items.Add(item);
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    /// <summary>Reads one file: where it is linked, and what it says it is.</summary>
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
        // The launcher carries no metadata block of its own, so a base linked
        // image with no block is what a launcher looks like from here.
        if (slot == 0 && meta is null && bytes.Length > 0)
        {
            return new LibraryItem(path, null, 0, bytes.Length);
        }

        return new LibraryItem(path, meta, slot, bytes.Length);
    }

    /// <summary>
    /// Copies a file into the library directory, which is how a downloaded CI
    /// artifact joins it.
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
}
