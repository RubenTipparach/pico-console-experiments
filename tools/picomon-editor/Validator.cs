using System.Globalization;

namespace PicomonEditor;

public enum Severity
{
    /// <summary>The build would stop on this. Saving the zone it belongs to is
    /// refused, because a file the compiler rejects is the one thing this app
    /// exists to prevent.</summary>
    Error,

    /// <summary>Legal, but almost certainly not what was meant.</summary>
    Warning,
}

/// <summary>One thing wrong, and enough to go to it: which zone, which record,
/// and which tile.</summary>
public sealed class Problem
{
    public Severity Severity { get; init; }
    public string ZoneId { get; init; } = "";
    public string Text { get; init; } = "";
    public object? Thing { get; init; }
    public int X { get; init; } = -1;
    public int Y { get; init; } = -1;

    public bool HasPlace => X >= 0 && Y >= 0;
    public string Where => ZoneId + (HasPlace ? $" {ZoneFile.N(X)},{ZoneFile.N(Y)}" : "");
}

/// <summary>
/// The same checks tools/picomon_data.py makes, run while the map is still
/// open. Catching a bad warp here is a five second fix; catching it in CI is a
/// failed build and a round trip through a runner.
///
/// The limits are the compiler's limits, quoted from it: a dialogue page is
/// three lines of 28 at a 4 pixel advance, a zone banner holds 16 characters,
/// a party holds six, and the save block has 64 flag bits.
/// </summary>
public static class Validator
{
    public const int DialogueColumns = 28;
    public const int DialogueLines = 3;
    public const int MaxZoneName = 16;
    public const int MaxParty = 6;
    public const int MaxFlags = 64;

    public static readonly string[] Facings = { "north", "east", "south", "west" };
    public static readonly string[] NpcKinds = { "villager", "trainer", "healer", "shop" };
    public static readonly string[] EventKinds = { "sign", "item", "trigger" };

    public static List<Problem> Check(Dataset data)
    {
        var problems = new List<Problem>();
        foreach (var message in data.LoadErrors)
            problems.Add(new Problem { Severity = Severity.Error, ZoneId = "data", Text = message });

        // Flags are declared by use: any flag key defines one, and a condition
        // may only name a flag something sets. Both halves are collected across
        // the whole directory first, because the flag an NPC waits on is
        // usually set in another zone entirely.
        var set = new HashSet<string>();
        foreach (var zone in data.Zones)
        {
            foreach (var npc in zone.Npcs) if (npc.Flag.Length > 0) set.Add(npc.Flag);
            foreach (var ev in zone.Events) if (ev.Flag.Length > 0) set.Add(ev.Flag);
        }
        if (set.Count > MaxFlags)
        {
            problems.Add(new Problem
            {
                Severity = Severity.Error,
                ZoneId = "data",
                Text = $"{set.Count} flags, the save block holds {MaxFlags}",
            });
        }

        foreach (var zone in data.Zones) CheckZone(data, zone, set, problems);
        return problems;
    }

    private static void CheckZone(Dataset data, ZoneFile zone, HashSet<string> setFlags,
                                  List<Problem> problems)
    {
        void Add(Severity severity, string text, object? thing = null, int x = -1, int y = -1) =>
            problems.Add(new Problem
            {
                Severity = severity, ZoneId = zone.Id, Text = text, Thing = thing, X = x, Y = y,
            });

        var expected = Path.GetFileNameWithoutExtension(zone.Path);
        if (zone.Id != expected)
            Add(Severity.Error, $"zone id '{zone.Id}' does not match the filename '{expected}'");
        if (zone.Name.Length == 0)
            Add(Severity.Error, "zone has no name");
        else if (zone.Name.Length > MaxZoneName)
            Add(Severity.Error, $"zone name is {zone.Name.Length} characters, the banner holds {MaxZoneName}");
        if (zone.Width is < 1 or > 255 || zone.Height is < 1 or > 255)
            Add(Severity.Error, "a zone is at most 255 by 255 tiles");
        if (zone.DeclaredWidth != zone.Width || zone.DeclaredHeight != zone.Height)
        {
            Add(Severity.Warning,
                $"the size line says {ZoneFile.N(zone.DeclaredWidth)} {ZoneFile.N(zone.DeclaredHeight)} "
                + $"but the grid is {ZoneFile.N(zone.Width)} by {ZoneFile.N(zone.Height)}; "
                + "saving writes the grid's size");
        }

        // --- tiles
        var unknown = new HashSet<char>();
        for (var y = 0; y < zone.Height; y++)
        {
            if (zone.Rows[y].Length != zone.Width)
            {
                Add(Severity.Error,
                    $"row {ZoneFile.N(y)} is {ZoneFile.N(zone.Rows[y].Length)} characters, "
                    + $"the widest row is {ZoneFile.N(zone.Width)}", null, 0, y);
                continue;
            }
            for (var x = 0; x < zone.Width; x++)
            {
                var ch = zone.Rows[y][x];
                if (!data.TileByChar.ContainsKey(ch) && unknown.Add(ch))
                    Add(Severity.Error, $"tile character '{ch}' is not in tileset.txt", null, x, y);
            }
        }

        // --- warps
        var warped = new HashSet<(int, int)>();
        foreach (var warp in zone.Warps)
        {
            if (!zone.Contains(warp.X, warp.Y))
            {
                Add(Severity.Error, "warp is outside its own zone", warp, warp.X, warp.Y);
            }
            else
            {
                warped.Add((warp.X, warp.Y));
                if (!Walkable(data, zone.TileAt(warp.X, warp.Y)))
                    Add(Severity.Error, "the warp tile is not walkable, so the player can never step on it",
                        warp, warp.X, warp.Y);
            }

            if (!Facings.Contains(warp.EffectiveFacing))
                Add(Severity.Error, $"unknown facing '{warp.Facing}'", warp, warp.X, warp.Y);

            if (!data.ZoneById.TryGetValue(warp.Dest, out var dest))
            {
                Add(Severity.Error, $"warps to unknown zone '{warp.Dest}'", warp, warp.X, warp.Y);
                continue;
            }
            if (!dest.Contains(warp.DestX, warp.DestY))
                Add(Severity.Error, "warp lands outside the destination zone", warp, warp.X, warp.Y);
            else if (!Walkable(data, dest.TileAt(warp.DestX, warp.DestY)))
                Add(Severity.Error, "warp lands on a tile that is not walkable, which strands the player",
                    warp, warp.X, warp.Y);
        }

        for (var y = 0; y < zone.Height; y++)
        {
            for (var x = 0; x < zone.Rows[y].Length; x++)
            {
                if (data.TileByChar.TryGetValue(zone.Rows[y][x], out var tile)
                    && tile.Door && !warped.Contains((x, y)))
                {
                    Add(Severity.Error, "door tile has no warp on it, so it opens onto nothing", null, x, y);
                }
            }
        }

        // --- encounters
        foreach (var table in zone.Encounters)
        {
            if (!data.TileByChar.TryGetValue(table.Tile, out var tile))
            {
                Add(Severity.Error, $"encounter tile '{table.Tile}' is not in tileset.txt", table);
            }
            else
            {
                if (!tile.Encounter)
                    Add(Severity.Error,
                        $"tile '{table.Tile}' ({tile.Name}) has no encounter flag, so this table would never be rolled",
                        table);
                var present = false;
                for (var y = 0; y < zone.Height && !present; y++)
                    present = Array.IndexOf(zone.Rows[y], table.Tile) >= 0;
                if (!present)
                    Add(Severity.Error, $"no '{table.Tile}' tile in this zone, so this table can never be rolled",
                        table);
            }
            if (table.Rate is < 1 or > 100)
                Add(Severity.Error, "rate is a percentage, 1 to 100", table);
            if (table.Slots.Count == 0)
                Add(Severity.Error, "an encounter table with no species never spawns", table);
            foreach (var slot in table.Slots)
            {
                if (!data.HasSpecies(slot.Species))
                    Add(Severity.Error, $"encounter '{table.Tile}' rolls unknown species '{slot.Species}'", table);
                if (slot.MinLevel < 1 || slot.MaxLevel < slot.MinLevel || slot.MaxLevel > 100)
                    Add(Severity.Error,
                        $"level range {ZoneFile.N(slot.MinLevel)}-{ZoneFile.N(slot.MaxLevel)} makes no sense", table);
                if (slot.Weight <= 0)
                    Add(Severity.Error, $"'{slot.Species}' has no weight, so it never comes up", table);
            }
        }

        // --- npcs
        foreach (var npc in zone.Npcs)
        {
            if (!zone.Contains(npc.X, npc.Y))
                Add(Severity.Error, $"npc '{npc.Id}' is outside the zone", npc, npc.X, npc.Y);
            if (!Facings.Contains(npc.Facing))
                Add(Severity.Error, $"npc '{npc.Id}' has unknown facing '{npc.Facing}'", npc, npc.X, npc.Y);
            if (npc.Kind.Length > 0 && !NpcKinds.Contains(npc.Kind))
                Add(Severity.Error, $"unknown npc kind '{npc.Kind}'", npc, npc.X, npc.Y);
            if (npc.Sheet.Length == 0)
                Add(Severity.Error, $"npc '{npc.Id}' has no sheet", npc, npc.X, npc.Y);

            var trainer = npc.EffectiveKind == "trainer";
            if (trainer && npc.Party.Count == 0)
                Add(Severity.Error, $"trainer '{npc.Id}' has nothing to send out", npc, npc.X, npc.Y);
            if (trainer && npc.Flag.Length == 0)
                Add(Severity.Error,
                    $"trainer '{npc.Id}' has no flag, so it would challenge the player again every time they walked past",
                    npc, npc.X, npc.Y);
            if (!trainer && npc.Party.Count > 0)
                Add(Severity.Error, $"'{npc.Id}' has a party but is not a trainer", npc, npc.X, npc.Y);
            if (npc.Party.Count > MaxParty)
                Add(Severity.Error, $"a party holds {MaxParty}", npc, npc.X, npc.Y);
            foreach (var member in npc.Party)
            {
                if (!data.HasSpecies(member.Species))
                    Add(Severity.Error, $"npc '{npc.Id}' has unknown species '{member.Species}'", npc, npc.X, npc.Y);
                if (member.Level is < 1 or > 100)
                    Add(Severity.Error, $"npc '{npc.Id}' has a party member at level {ZoneFile.N(member.Level)}",
                        npc, npc.X, npc.Y);
            }
            if (npc.Say.Count == 0)
                Add(Severity.Error, $"npc '{npc.Id}' has nothing to say", npc, npc.X, npc.Y);
            if (npc.Cond.Length > 0 && !setFlags.Contains(npc.Cond))
                Add(Severity.Error,
                    $"'{npc.Id}' waits on flag '{npc.Cond}', which nothing ever sets, so it is switched off forever",
                    npc, npc.X, npc.Y);
            if (!Walkable(data, zone.TileAt(npc.X, npc.Y)) && zone.Contains(npc.X, npc.Y))
                Add(Severity.Warning, $"npc '{npc.Id}' stands on a tile that is not walkable", npc, npc.X, npc.Y);

            CheckPages(npc.Say, "say", npc, npc.X, npc.Y, npc.Id, Add);
            CheckPages(npc.Win, "win", npc, npc.X, npc.Y, npc.Id, Add);
            CheckPages(npc.Lose, "lose", npc, npc.X, npc.Y, npc.Id, Add);
        }

        // --- events
        foreach (var ev in zone.Events)
        {
            if (!zone.Contains(ev.X, ev.Y))
                Add(Severity.Error, $"{ev.Kind} event is outside the zone", ev, ev.X, ev.Y);
            if (!EventKinds.Contains(ev.Kind))
            {
                Add(Severity.Error, $"unknown event kind '{ev.Kind}'", ev, ev.X, ev.Y);
            }
            else if (ev.Kind == "item")
            {
                if (ev.Args.Count != 2)
                    Add(Severity.Error, "expected: event item <x> <y> <item> <count>", ev, ev.X, ev.Y);
                else
                {
                    if (!data.HasItem(ev.Item))
                        Add(Severity.Error, $"unknown item '{ev.Item}'", ev, ev.X, ev.Y);
                    if (!int.TryParse(ev.Count, NumberStyles.Integer, CultureInfo.InvariantCulture, out var count))
                        Add(Severity.Error, $"'{ev.Count}' is not a count", ev, ev.X, ev.Y);
                    else if (count < 1)
                        Add(Severity.Warning, "an item event that gives nothing", ev, ev.X, ev.Y);
                }
                if (ev.Flag.Length == 0)
                    Add(Severity.Error,
                        "an item event needs a flag, or the player can pick it up again every time they walk back",
                        ev, ev.X, ev.Y);
            }
            else if (ev.Args.Count > 0)
            {
                Add(Severity.Error, $"event {ev.Kind} takes no extra arguments", ev, ev.X, ev.Y);
            }

            if (ev.Kind == "sign" && ev.Say.Count == 0)
                Add(Severity.Error, "a sign with nothing on it is a blank sign", ev, ev.X, ev.Y);
            if (ev.Kind == "trigger" && ev.Say.Count == 0 && ev.Flag.Length == 0)
                Add(Severity.Warning, "a trigger that says nothing and sets no flag does nothing", ev, ev.X, ev.Y);
            CheckPages(ev.Say, "say", ev, ev.X, ev.Y, ev.Kind, Add);
        }
    }

    private static void CheckPages(List<string> pages, string key, object thing, int x, int y,
                                   string owner, Action<Severity, string, object?, int, int> add)
    {
        foreach (var page in pages)
        {
            if (!WrapsWithin(page, DialogueColumns, DialogueLines))
                add(Severity.Error,
                    $"'{owner}' {key} does not fit {ZoneFile.N(DialogueLines)} lines of "
                    + $"{ZoneFile.N(DialogueColumns)}: \"{page}\"", thing, x, y);
            // The compiler reads a '#' as the start of a comment wherever it
            // appears, so a page containing one arrives on the device cut short
            // at that character with nothing to say it happened.
            if (page.Contains('#'))
                add(Severity.Warning,
                    $"'{owner}' {key} contains a '#', which the compiler reads as a comment and cuts the page there",
                    thing, x, y);
        }
    }

    /// <summary>
    /// The same greedy wrap the game does, copied from picomon_data.py so the
    /// answer here and the answer in CI are the same answer. A single word
    /// longer than a line can never fit, however few words there are.
    /// </summary>
    public static bool WrapsWithin(string text, int columns, int lines)
    {
        var used = 1;
        var current = 0;
        foreach (var word in text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
        {
            if (word.Length > columns) return false;
            if (current == 0) current = word.Length;
            else if (current + 1 + word.Length <= columns) current += 1 + word.Length;
            else { used++; current = word.Length; }
        }
        return used <= lines;
    }

    private static bool Walkable(Dataset data, char ch) =>
        data.TileByChar.TryGetValue(ch, out var tile) && tile.Walk;
}
