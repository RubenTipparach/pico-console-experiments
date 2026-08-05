using System.Globalization;
using System.Text;

namespace PicomonEditor;

/// <summary>
/// A .zone file, read and written in the syntax data/FORMAT.md describes.
///
/// Round tripping is the whole point of this class. A zone file is reviewed as
/// a diff (that is why the format is text at all), so opening a file and saving
/// it again has to produce the same bytes, or the next real change arrives
/// buried in a rewrite nobody reads. Two things make that work:
///
/// - Records are written in a fixed order, which is the order the five zones
///   already use: header, tiles, warps, encounters, npcs, events. Keys inside a
///   block have a fixed order too.
/// - Everything the format allows but the model has no field for is carried
///   along rather than dropped. Blank lines and whole line comments are kept as
///   trivia on the record that followed them, and a comment on the end of a
///   line is kept in a map keyed by the line it was attached to.
///
/// The reading rules match tools/picomon_data.py exactly, because the compiler
/// is what these files are actually for: a '#' anywhere starts a comment,
/// leading and trailing whitespace is insignificant, and a key that appears
/// twice means the last one wins.
/// </summary>
public sealed class ZoneFile
{
    public string Path { get; set; } = "";
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public int Width { get; private set; }
    public int Height { get; private set; }

    /// <summary>What the size line actually said. The grid is what gets painted
    /// and written, so these only exist to be reported when they disagree:
    /// silently rewriting a size line is how a real mistake gets hidden.</summary>
    public int DeclaredWidth { get; private set; }
    public int DeclaredHeight { get; private set; }

    /// <summary>The grid, row major. This is what the palette paints onto, and
    /// what the tiles block is written from.</summary>
    public char[][] Rows { get; private set; } = Array.Empty<char[]>();

    public List<WarpDef> Warps { get; } = new();
    public List<EncounterTable> Encounters { get; } = new();
    public List<NpcDef> Npcs { get; } = new();
    public List<EventDef> Events { get; } = new();

    public bool Dirty { get; set; }

    /// <summary>Whatever separated the records, kept so it survives a save.</summary>
    private List<string> _headerTrivia = new();
    private List<string> _tilesTrivia = new();
    private List<string> _trailingTrivia = new();
    private readonly Dictionary<string, string> _suffix = new();

    /// <summary>The file's own line ending. Git on Windows can hand these files
    /// over as CRLF, and rewriting the whole file as LF would bury the one line
    /// that actually changed.</summary>
    private string _newline = "\n";

    public IEnumerable<IPlaced> Placed =>
        Warps.Cast<IPlaced>().Concat(Npcs).Concat(Events);

    public bool Contains(int x, int y) => x >= 0 && y >= 0 && x < Width && y < Height;

    /// <summary>A row shorter than the widest one is a file the compiler
    /// rejects, so it is reported rather than padded, and until it is fixed
    /// those cells are not there to read or paint.</summary>
    private bool InRow(int x, int y) => y >= 0 && y < Rows.Length && x >= 0 && x < Rows[y].Length;

    public char TileAt(int x, int y) => InRow(x, y) ? Rows[y][x] : '\0';

    public void SetTile(int x, int y, char ch)
    {
        if (!InRow(x, y) || Rows[y][x] == ch) return;
        Rows[y][x] = ch;
        Dirty = true;
    }

    // ---------------------------------------------------------------- reading

    public static ZoneFile Load(string path)
    {
        var zone = ParseText(File.ReadAllText(path), path);
        zone.Path = path;
        return zone;
    }

    public static ZoneFile ParseText(string text, string path)
    {
        var zone = new ZoneFile { Path = path };
        zone._newline = text.Contains("\r\n") ? "\r\n" : "\n";

        var lines = text.Replace("\r\n", "\n").Split('\n').ToList();
        if (lines.Count > 0 && lines[^1].Length == 0) lines.RemoveAt(lines.Count - 1);

        var trivia = new List<string>();
        var rows = new List<char[]>();
        var i = 0;
        while (i < lines.Count)
        {
            var (code, suffix) = SplitComment(lines[i]);
            if (code.Trim().Length == 0)
            {
                trivia.Add(lines[i]);
                i++;
                continue;
            }

            var line = code.Trim();
            var (key, rest) = Partition(line);
            var parts = Words(rest);
            var lineNumber = i + 1;

            switch (key)
            {
                case "zone":
                    zone._headerTrivia = trivia;
                    trivia = new List<string>();
                    zone.Id = rest;
                    Hang(zone._suffix, "zone " + rest, suffix);
                    break;

                case "name":
                    zone.Name = rest;
                    Hang(zone._suffix, "name " + rest, suffix);
                    break;

                case "size":
                    if (parts.Length != 2) throw Bad(path, lineNumber, "expected: size <w> <h>");
                    zone.DeclaredWidth = Number(parts[0], path, lineNumber);
                    zone.DeclaredHeight = Number(parts[1], path, lineNumber);
                    Hang(zone._suffix, $"size {N(zone.DeclaredWidth)} {N(zone.DeclaredHeight)}", suffix);
                    break;

                case "tiles":
                    zone._tilesTrivia = trivia;
                    trivia = new List<string>();
                    Hang(zone._suffix, "tiles", suffix);
                    i++;
                    while (i < lines.Count && SplitComment(lines[i]).Code.Trim() != "end")
                    {
                        rows.Add(SplitComment(lines[i]).Code.Trim().ToCharArray());
                        i++;
                    }
                    if (i >= lines.Count) throw Bad(path, lineNumber, "the tiles block is never closed with 'end'");
                    break;

                case "warp":
                {
                    if (parts.Length is not (5 or 6))
                        throw Bad(path, lineNumber, "expected: warp <x> <y> <zone> <dx> <dy> [facing]");
                    var warp = new WarpDef
                    {
                        X = Number(parts[0], path, lineNumber),
                        Y = Number(parts[1], path, lineNumber),
                        Dest = parts[2],
                        DestX = Number(parts[3], path, lineNumber),
                        DestY = Number(parts[4], path, lineNumber),
                        Facing = parts.Length == 6 ? parts[5] : "",
                    };
                    warp.Trivia = trivia;
                    trivia = new List<string>();
                    Hang(warp.Suffix, warp.HeaderLine(), suffix);
                    zone.Warps.Add(warp);
                    break;
                }

                case "encounter":
                case "npc":
                case "event":
                {
                    var record = OpenBlock(zone, key, parts, path, lineNumber);
                    Hang(record.Suffix, record.HeaderLine(), suffix);
                    record.Trivia = trivia;
                    trivia = new List<string>();
                    i++;
                    while (true)
                    {
                        if (i >= lines.Count)
                            throw Bad(path, lineNumber, $"the {key} block is never closed with 'end'");
                        var (body, bodySuffix) = SplitComment(lines[i]);
                        var trimmed = body.Trim();
                        if (trimmed == "end") break;
                        if (trimmed.Length > 0)
                        {
                            var (bodyKey, bodyValue) = Partition(trimmed);
                            var canonical = record.ReadKey(bodyKey, bodyValue, path, i + 1);
                            if (canonical is not null) Hang(record.Suffix, canonical, bodySuffix);
                        }
                        i++;
                    }
                    break;
                }

                default:
                    throw Bad(path, lineNumber, $"unknown key '{key}' at the top level of a zone");
            }
            i++;
        }

        zone._trailingTrivia = trivia;
        zone.Rows = rows.ToArray();
        if (zone.Rows.Length == 0) throw Bad(path, 1, "zone has no tiles block");

        // The grid is what the editor paints, so it is what the coordinates mean
        // and what the size line is written from. A size line that disagreed
        // with its own tiles is a file the compiler rejects, so saving fixes it,
        // and the validator says so first rather than letting the correction
        // arrive as a surprise line in the diff.
        zone.Height = zone.Rows.Length;
        zone.Width = zone.Rows.Max(r => r.Length);
        return zone;
    }

    private static Record OpenBlock(ZoneFile zone, string key, string[] header, string path, int line)
    {
        switch (key)
        {
            case "encounter":
                if (header.Length != 1 || header[0].Length != 1)
                    throw Bad(path, line, "expected: encounter <tile character>");
                var table = new EncounterTable { Tile = header[0][0] };
                zone.Encounters.Add(table);
                return table;

            case "npc":
                if (header.Length != 5)
                    throw Bad(path, line, "expected: npc <id> <x> <y> <facing> <sheet>");
                var npc = new NpcDef
                {
                    Id = header[0],
                    X = Number(header[1], path, line),
                    Y = Number(header[2], path, line),
                    Facing = header[3],
                    Sheet = header[4],
                };
                zone.Npcs.Add(npc);
                return npc;

            default:
                if (header.Length < 3)
                    throw Bad(path, line, "expected: event <kind> <x> <y> [args]");
                var ev = new EventDef
                {
                    Kind = header[0],
                    X = Number(header[1], path, line),
                    Y = Number(header[2], path, line),
                    Args = header.Skip(3).ToList(),
                };
                zone.Events.Add(ev);
                return ev;
        }
    }

    // ---------------------------------------------------------------- writing

    public void Save()
    {
        File.WriteAllText(Path, Serialise(), new UTF8Encoding(false));
        Dirty = false;
    }

    public string Serialise()
    {
        var body = new List<string>();

        void Put(Dictionary<string, string> suffix, string line) =>
            body.Add(line + (suffix.TryGetValue(line, out var s) ? s : ""));

        void Lead(Record record, bool first, bool block)
        {
            var trivia = new List<string>(record.Trivia);
            // Whatever separated the records is kept, but a record that arrived
            // with none (the editor just made it, or the one that carried the
            // blank line was deleted) still gets one, so records never run
            // together.
            if ((block || first) && !trivia.Any(t => t.Trim().Length == 0)) trivia.Insert(0, "");
            body.AddRange(trivia);
        }

        body.AddRange(_headerTrivia);
        Put(_suffix, "zone " + Id);
        Put(_suffix, "name " + Name);
        Put(_suffix, $"size {N(Width)} {N(Height)}");

        var tilesTrivia = new List<string>(_tilesTrivia);
        if (!tilesTrivia.Any(t => t.Trim().Length == 0)) tilesTrivia.Insert(0, "");
        body.AddRange(tilesTrivia);
        Put(_suffix, "tiles");
        foreach (var row in Rows) body.Add(new string(row));
        body.Add("end");

        for (var i = 0; i < Warps.Count; i++)
        {
            Lead(Warps[i], i == 0, block: false);
            Put(Warps[i].Suffix, Warps[i].HeaderLine());
        }

        for (var i = 0; i < Encounters.Count; i++)
        {
            Lead(Encounters[i], i == 0, block: true);
            Encounters[i].Write(body);
        }

        for (var i = 0; i < Npcs.Count; i++)
        {
            Lead(Npcs[i], i == 0, block: true);
            Npcs[i].Write(body);
        }

        for (var i = 0; i < Events.Count; i++)
        {
            Lead(Events[i], i == 0, block: true);
            Events[i].Write(body);
        }

        body.AddRange(_trailingTrivia);
        return string.Join(_newline, body) + _newline;
    }

    // ---------------------------------------------------------------- helpers

    /// <summary>Splits a raw line into the part the compiler reads and the part
    /// it throws away. The suffix keeps the gap before the '#' so a commented
    /// line comes back out exactly as it went in.</summary>
    internal static (string Code, string Suffix) SplitComment(string raw)
    {
        var hash = raw.IndexOf('#');
        if (hash < 0)
        {
            var trimmed = raw.TrimEnd();
            return (trimmed, raw[trimmed.Length..]);
        }
        var code = raw[..hash];
        var body = code.TrimEnd();
        return (body, code[body.Length..] + raw[hash..]);
    }

    internal static void Hang(Dictionary<string, string> suffix, string canonical, string comment)
    {
        if (comment.Trim().Length > 0) suffix[canonical] = comment;
    }

    internal static (string Key, string Value) Partition(string line)
    {
        var space = line.IndexOf(' ');
        return space < 0 ? (line, "") : (line[..space], line[(space + 1)..].Trim());
    }

    internal static string[] Words(string value) =>
        value.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);

    /// <summary>Invariant on purpose: these files are read by a Python compiler
    /// on a Linux runner, so a locale that formats digits its own way must not
    /// reach them.</summary>
    internal static string N(int value) => value.ToString(CultureInfo.InvariantCulture);

    internal static int Number(string token, string path, int line)
    {
        if (!int.TryParse(token, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value))
            throw Bad(path, line, $"expected a number, got '{token}'");
        return value;
    }

    internal static ZoneFormatException Bad(string path, int line, string message) =>
        new($"{System.IO.Path.GetFileName(path)}:{line}: {message}");
}

public sealed class ZoneFormatException : Exception
{
    public ZoneFormatException(string message) : base(message) { }
}

/// <summary>Anything that sits on a tile and can be dragged to another one.</summary>
public interface IPlaced
{
    int X { get; set; }
    int Y { get; set; }
    string Label { get; }
}

/// <summary>
/// The parts of a record the model has no field for: the blank lines and
/// comments that came before it, and any comment hung off the end of one of its
/// lines. Both are kept so a save is a diff of what changed and nothing else.
/// </summary>
public abstract class Record
{
    public List<string> Trivia { get; set; } = new();
    public Dictionary<string, string> Suffix { get; } = new();

    public abstract string HeaderLine();

    /// <summary>Applies one body line. Returns the canonical text that line
    /// will be written back as, so a trailing comment can be hung off it, or
    /// null when the line has no stable text to key on.</summary>
    public abstract string? ReadKey(string key, string value, string path, int line);

    protected void Put(List<string> body, string line) =>
        body.Add(line + (Suffix.TryGetValue(line, out var s) ? s : ""));

    /// <summary>A line inside a block. The suffix map is keyed on the
    /// unindented text, which is what the parser saw.</summary>
    protected void PutBody(List<string> body, string line) =>
        body.Add("  " + line + (Suffix.TryGetValue(line, out var s) ? s : ""));
}

public sealed class WarpDef : Record, IPlaced
{
    public int X { get; set; }
    public int Y { get; set; }
    public string Dest { get; set; } = "";
    public int DestX { get; set; }
    public int DestY { get; set; }

    /// <summary>Empty means the file left it out, which the compiler reads as
    /// south. Kept as written rather than filled in, so opening a file does not
    /// add a word to every warp in it.</summary>
    public string Facing { get; set; } = "";

    public string EffectiveFacing => Facing.Length > 0 ? Facing : "south";

    /// <summary>Only writes the word when it says something the default does
    /// not, which is the same trick NpcDef.SetKind plays.</summary>
    public void SetFacing(string facing)
    {
        if (facing == EffectiveFacing) return;
        Facing = facing;
    }

    public string Label => $"warp {ZoneFile.N(X)},{ZoneFile.N(Y)} to {Dest}";

    public override string HeaderLine()
    {
        var line = $"warp {ZoneFile.N(X)} {ZoneFile.N(Y)} {Dest} {ZoneFile.N(DestX)} {ZoneFile.N(DestY)}";
        return Facing.Length > 0 ? line + " " + Facing : line;
    }

    public override string? ReadKey(string key, string value, string path, int line) =>
        throw ZoneFile.Bad(path, line, "a warp is a single line and has no keys");
}

public sealed class EncounterRow
{
    public string Species { get; set; } = "";
    public int MinLevel { get; set; } = 1;
    public int MaxLevel { get; set; } = 1;
    public int Weight { get; set; } = 1;

    public override string ToString() =>
        $"{Species} {ZoneFile.N(MinLevel)}-{ZoneFile.N(MaxLevel)} weight {ZoneFile.N(Weight)}";
}

public sealed class EncounterTable : Record
{
    public char Tile { get; set; }
    public int Rate { get; set; } = 10;
    public List<EncounterRow> Slots { get; } = new();

    public override string HeaderLine() => "encounter " + Tile;

    public override string? ReadKey(string key, string value, string path, int line)
    {
        if (key == "rate")
        {
            Rate = ZoneFile.Number(value, path, line);
            return "rate " + ZoneFile.N(Rate);
        }
        var parts = ZoneFile.Words(key + " " + value);
        if (parts.Length != 4)
            throw ZoneFile.Bad(path, line, "expected: <species> <min level> <max level> <weight>");
        Slots.Add(new EncounterRow
        {
            Species = parts[0],
            MinLevel = ZoneFile.Number(parts[1], path, line),
            MaxLevel = ZoneFile.Number(parts[2], path, line),
            Weight = ZoneFile.Number(parts[3], path, line),
        });
        // The species column is padded on write, so the row has no stable text
        // to hang a comment off.
        return null;
    }

    public void Write(List<string> body)
    {
        Put(body, HeaderLine());
        PutBody(body, "rate " + ZoneFile.N(Rate));
        var width = Slots.Count == 0 ? 0 : Slots.Max(s => s.Species.Length) + 1;
        foreach (var slot in Slots)
        {
            body.Add("  " + slot.Species.PadRight(width)
                     + $"{ZoneFile.N(slot.MinLevel)} {ZoneFile.N(slot.MaxLevel)} {ZoneFile.N(slot.Weight)}");
        }
        body.Add("end");
    }
}

public sealed class PartyEntry
{
    public string Species { get; set; } = "";
    public int Level { get; set; } = 1;
}

public sealed class NpcDef : Record, IPlaced
{
    public string Id { get; set; } = "";
    public int X { get; set; }
    public int Y { get; set; }
    public string Facing { get; set; } = "south";
    public string Sheet { get; set; } = "villager";

    /// <summary>Empty means the file left the key out. The kind is not stored
    /// as "villager" in that case, because writing the word back would put a
    /// line into every NPC that never had one.</summary>
    public string Kind { get; set; } = "";

    /// <summary>Null means absent. Zero is not the same thing: a 'sight' key
    /// makes an NPC a trainer whatever its value, so 'sight 0' is a trainer you
    /// have to walk up to and talk to.</summary>
    public int? Sight { get; set; }

    public List<PartyEntry> Party { get; } = new();

    /// <summary>onlyif and hideif are one condition, exactly as the compiler
    /// stores it: they write the same field, so a block carrying both keeps
    /// only the last one anyway.</summary>
    public string Cond { get; set; } = "";
    public bool CondHide { get; set; }

    public List<string> Say { get; } = new();
    public List<string> Win { get; } = new();
    public List<string> Lose { get; } = new();
    public int Reward { get; set; }
    public string Flag { get; set; } = "";

    public string EffectiveKind => Sight.HasValue ? "trainer" : (Kind.Length > 0 ? Kind : "villager");

    /// <summary>Sets the kind without writing a line that says what the file
    /// already means. Picking 'trainer' for an NPC that has a sight line
    /// changes nothing, so nothing is written.</summary>
    public void SetKind(string kind)
    {
        if (kind == EffectiveKind) return;
        // A sight line is what makes an NPC a trainer, so it cannot survive the
        // NPC becoming something else: the compiler would read it back as a
        // trainer no matter what the kind line says.
        if (kind != "trainer") Sight = null;
        Kind = kind == "villager" && Kind.Length == 0 ? "" : kind;
    }

    public string Label => $"npc {Id} ({EffectiveKind}) {ZoneFile.N(X)},{ZoneFile.N(Y)}";

    public override string HeaderLine() =>
        $"npc {Id} {ZoneFile.N(X)} {ZoneFile.N(Y)} {Facing} {Sheet}";

    public string PartyLine() =>
        "party " + string.Join(", ", Party.Select(p => $"{p.Species} {ZoneFile.N(p.Level)}"));

    public override string? ReadKey(string key, string value, string path, int line)
    {
        switch (key)
        {
            case "kind": Kind = value; return "kind " + value;
            case "sight": Sight = ZoneFile.Number(value, path, line); return "sight " + ZoneFile.N(Sight.Value);
            case "party":
                foreach (var entry in value.Split(','))
                {
                    var (species, level) = ZoneFile.Partition(entry.Trim());
                    if (species.Length == 0) continue;
                    Party.Add(new PartyEntry
                    {
                        Species = species,
                        Level = level.Length == 0 ? 1 : ZoneFile.Number(level, path, line),
                    });
                }
                return PartyLine();
            case "onlyif": Cond = value; CondHide = false; return "onlyif " + value;
            case "hideif": Cond = value; CondHide = true; return "hideif " + value;
            case "say": Say.Add(value); return "say " + value;
            case "win": Win.Add(value); return "win " + value;
            case "lose": Lose.Add(value); return "lose " + value;
            case "reward": Reward = ZoneFile.Number(value, path, line); return "reward " + ZoneFile.N(Reward);
            case "flag": Flag = value; return "flag " + value;
            default: throw ZoneFile.Bad(path, line, $"unknown key '{key}' in an npc block");
        }
    }

    public void Write(List<string> body)
    {
        Put(body, HeaderLine());
        if (Kind.Length > 0) PutBody(body, "kind " + Kind);
        if (Sight.HasValue) PutBody(body, "sight " + ZoneFile.N(Sight.Value));
        if (Party.Count > 0) PutBody(body, PartyLine());
        if (Cond.Length > 0) PutBody(body, (CondHide ? "hideif " : "onlyif ") + Cond);
        foreach (var page in Say) PutBody(body, "say " + page);
        foreach (var page in Win) PutBody(body, "win " + page);
        foreach (var page in Lose) PutBody(body, "lose " + page);
        if (Reward != 0) PutBody(body, "reward " + ZoneFile.N(Reward));
        if (Flag.Length > 0) PutBody(body, "flag " + Flag);
        body.Add("end");
    }
}

public sealed class EventDef : Record, IPlaced
{
    public string Kind { get; set; } = "sign";
    public int X { get; set; }
    public int Y { get; set; }

    /// <summary>The extra header arguments for this kind: an item event carries
    /// its item and count here, and the other kinds carry nothing.</summary>
    public List<string> Args { get; set; } = new();

    public string Flag { get; set; } = "";
    public List<string> Say { get; } = new();

    public string Item
    {
        get => Args.Count > 0 ? Args[0] : "";
        set { while (Args.Count < 1) Args.Add(""); Args[0] = value; }
    }

    public string Count
    {
        get => Args.Count > 1 ? Args[1] : "";
        set { while (Args.Count < 2) Args.Add(""); Args[1] = value; }
    }

    public string Label => $"event {Kind} {ZoneFile.N(X)},{ZoneFile.N(Y)}"
                           + (Args.Count > 0 ? " " + string.Join(" ", Args) : "");

    public override string HeaderLine()
    {
        var head = $"event {Kind} {ZoneFile.N(X)} {ZoneFile.N(Y)}";
        return Args.Count > 0 ? head + " " + string.Join(" ", Args) : head;
    }

    public override string? ReadKey(string key, string value, string path, int line)
    {
        switch (key)
        {
            case "flag": Flag = value; return "flag " + value;
            case "say": Say.Add(value); return "say " + value;
            default: throw ZoneFile.Bad(path, line, $"unknown key '{key}' in an event block");
        }
    }

    public void Write(List<string> body)
    {
        Put(body, HeaderLine());
        if (Flag.Length > 0) PutBody(body, "flag " + Flag);
        foreach (var page in Say) PutBody(body, "say " + page);
        body.Add("end");
    }
}
