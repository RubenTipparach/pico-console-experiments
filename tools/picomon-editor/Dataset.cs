namespace PicomonEditor;

/// <summary>
/// One line of tileset.txt. The palette is built from these, so a tile added to
/// the tileset appears in the editor with no change here: nothing in this app
/// knows the name of a single tile.
/// </summary>
public sealed class TileDef
{
    public char Ch { get; init; }
    public string Name { get; init; } = "";
    public IReadOnlyList<string> Flags { get; init; } = Array.Empty<string>();
    public Color Colour { get; init; }

    public bool Walk => Flags.Contains("walk");
    public bool Encounter => Flags.Contains("encounter");
    public bool Door => Flags.Contains("door");
    public string FlagText => string.Join(" ", Flags);

    /// <summary>What a dropdown shows. Left as ToString rather than a
    /// DisplayMember so the list cannot be filled without it.</summary>
    public override string ToString() => $"{Ch}   {Name}";
}

public sealed class NamedDef
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public override string ToString() => Id;
}

/// <summary>
/// Where a new game begins, out of start.txt. The editor never writes this
/// file, but it can break it: the player's first tile is an ordinary tile in an
/// ordinary zone, and painting a tree over it is a failed build with nothing in
/// the zone itself to show for it.
/// </summary>
public sealed class StartPoint
{
    public string Zone { get; init; } = "";
    public int X { get; init; }
    public int Y { get; init; }
    public string Facing { get; init; } = "south";
}

/// <summary>
/// The whole data directory: the tileset, the species and item lists the
/// dropdowns are built from, and every zone. Every zone, because the checks
/// that matter most cross files. A warp is only correct with reference to the
/// zone it lands in, and a flag is only set somewhere else.
/// </summary>
public sealed class Dataset
{
    public string Root { get; }
    public List<TileDef> Tiles { get; } = new();
    public Dictionary<char, TileDef> TileByChar { get; } = new();
    public List<NamedDef> Species { get; } = new();
    public List<NamedDef> Items { get; } = new();
    public List<ZoneFile> Zones { get; } = new();
    public Dictionary<string, ZoneFile> ZoneById { get; } = new();

    /// <summary>Null when start.txt is missing or says nothing useful, which is
    /// itself reported.</summary>
    public StartPoint? Start { get; private set; }

    /// <summary>Problems from reading the directory itself, as opposed to from
    /// checking what is in it: a tileset line that makes no sense, a zone file
    /// that will not parse. Kept rather than thrown so one broken zone does not
    /// stop the other four being edited.</summary>
    public List<string> LoadErrors { get; } = new();

    private Dataset(string root) => Root = root;

    public static bool LooksLikeDataDirectory(string path) =>
        path.Length > 0
        && File.Exists(Path.Combine(path, "tileset.txt"))
        && Directory.Exists(Path.Combine(path, "zones"));

    public static Dataset Load(string root)
    {
        var data = new Dataset(root);
        data.LoadTileset();
        data.LoadNames("species.txt", "species", data.Species);
        data.LoadNames("items.txt", "item", data.Items);
        data.LoadZones();
        data.LoadStart();
        return data;
    }

    /// <summary>Every sheet any NPC in the data already uses. The sheet name is
    /// free text as far as the compiler is concerned, so this seeds the box
    /// rather than restricting it.</summary>
    public IEnumerable<string> Sheets => Zones
        .SelectMany(z => z.Npcs).Select(n => n.Sheet)
        .Where(s => s.Length > 0).Distinct().OrderBy(s => s);

    /// <summary>Every flag something sets. onlyif and hideif may only name one
    /// of these, which is why the condition boxes are filled from here.</summary>
    public IEnumerable<string> SetFlags => Zones
        .SelectMany(z => z.Npcs.Select(n => n.Flag).Concat(z.Events.Select(e => e.Flag)))
        .Where(f => f.Length > 0).Distinct().OrderBy(f => f);

    public bool HasSpecies(string id) => Species.Any(s => s.Id == id);
    public bool HasItem(string id) => Items.Any(i => i.Id == id);

    private void LoadTileset()
    {
        var path = Path.Combine(Root, "tileset.txt");
        if (!File.Exists(path))
        {
            LoadErrors.Add("tileset.txt: not found in the data directory");
            return;
        }
        foreach (var (line, number) in Meaningful(path))
        {
            var parts = ZoneFile.Words(line);
            if (parts.Length < 6)
            {
                LoadErrors.Add($"tileset.txt:{number}: expected <char> <name> <flags...> <r> <g> <b>");
                continue;
            }
            if (parts[0].Length != 1)
            {
                LoadErrors.Add($"tileset.txt:{number}: a tile character is one character, got '{parts[0]}'");
                continue;
            }
            if (!TryHex(parts[^3], out var r) || !TryHex(parts[^2], out var g) || !TryHex(parts[^1], out var b))
            {
                LoadErrors.Add($"tileset.txt:{number}: colour must be three hex bytes");
                continue;
            }
            var tile = new TileDef
            {
                Ch = parts[0][0],
                Name = parts[1],
                Flags = parts[2..^3],
                Colour = Color.FromArgb(r, g, b),
            };
            if (TileByChar.ContainsKey(tile.Ch))
            {
                LoadErrors.Add($"tileset.txt:{number}: tile character '{tile.Ch}' is defined twice");
                continue;
            }
            Tiles.Add(tile);
            TileByChar[tile.Ch] = tile;
        }
        if (Tiles.Count == 0) LoadErrors.Add("tileset.txt: no tiles defined");
    }

    /// <summary>Reads the ids and display names out of a block file. The editor
    /// only ever needs to know what exists and what it is called; everything
    /// else in species.txt and items.txt is the game's business.</summary>
    private void LoadNames(string file, string opener, List<NamedDef> into)
    {
        var path = Path.Combine(Root, file);
        if (!File.Exists(path))
        {
            LoadErrors.Add($"{file}: not found in the data directory");
            return;
        }
        string id = "", name = "";
        foreach (var (line, _) in Meaningful(path))
        {
            var (key, value) = ZoneFile.Partition(line);
            if (key == opener) { id = value; name = ""; }
            else if (key == "name") name = value;
            else if (key == "end" && id.Length > 0)
            {
                into.Add(new NamedDef { Id = id, Name = name.Length > 0 ? name : id });
                id = "";
            }
        }
    }

    /// <summary>The `zone` and `at` lines of start.txt, and nothing else. The
    /// starting party and bag are checked against species.txt and items.txt by
    /// the compiler, but no edit made here can break those: the only part of
    /// this file the editor can invalidate is the tile the player stands on.</summary>
    private void LoadStart()
    {
        var path = Path.Combine(Root, "start.txt");
        if (!File.Exists(path))
        {
            LoadErrors.Add("start.txt: not found in the data directory");
            return;
        }
        string zone = "", facing = "south";
        int x = 0, y = 0;
        var placed = false;
        foreach (var (line, number) in Meaningful(path))
        {
            var (key, value) = ZoneFile.Partition(line);
            if (key == "zone") zone = value;
            else if (key != "at") continue;
            else
            {
                var parts = ZoneFile.Words(value);
                if (parts.Length != 3
                    || !int.TryParse(parts[0], System.Globalization.NumberStyles.Integer,
                                     System.Globalization.CultureInfo.InvariantCulture, out x)
                    || !int.TryParse(parts[1], System.Globalization.NumberStyles.Integer,
                                     System.Globalization.CultureInfo.InvariantCulture, out y))
                {
                    LoadErrors.Add($"start.txt:{number}: expected: at <x> <y> <facing>");
                    continue;
                }
                facing = parts[2];
                placed = true;
            }
        }
        if (zone.Length == 0)
        {
            LoadErrors.Add("start.txt: the start block has no zone");
            return;
        }
        if (!placed) LoadErrors.Add("start.txt: the start block has no 'at' line");
        Start = new StartPoint { Zone = zone, X = x, Y = y, Facing = facing };
    }

    private void LoadZones()
    {
        var directory = Path.Combine(Root, "zones");
        if (!Directory.Exists(directory))
        {
            LoadErrors.Add("zones/: not found in the data directory");
            return;
        }
        foreach (var path in Directory.GetFiles(directory, "*.zone").OrderBy(p => p, StringComparer.Ordinal))
        {
            try
            {
                var zone = ZoneFile.Load(path);
                Zones.Add(zone);
                if (zone.Id.Length > 0) ZoneById[zone.Id] = zone;
            }
            catch (ZoneFormatException e)
            {
                LoadErrors.Add(e.Message);
            }
            catch (IOException e)
            {
                LoadErrors.Add($"{Path.GetFileName(path)}: {e.Message}");
            }
        }
    }

    /// <summary>The lines a compiler would read: comments stripped the way
    /// picomon_data.py strips them, blank lines gone, with the line number kept
    /// so a complaint can point at the file.</summary>
    private static IEnumerable<(string Line, int Number)> Meaningful(string path)
    {
        var number = 0;
        foreach (var raw in File.ReadLines(path))
        {
            number++;
            var code = ZoneFile.SplitComment(raw).Code.Trim();
            if (code.Length > 0) yield return (code, number);
        }
    }

    private static bool TryHex(string token, out int value) =>
        int.TryParse(token, System.Globalization.NumberStyles.HexNumber,
                     System.Globalization.CultureInfo.InvariantCulture, out value);
}
