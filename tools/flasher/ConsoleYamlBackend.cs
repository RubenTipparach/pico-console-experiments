using System.Diagnostics;
using System.Text;

namespace PicoFlasher;

/// <summary>
/// The backend as the build works today: the recipe is console.yaml, and
/// making a console is build_console.bat.
///
/// Everything specific to that lives here. It shells out to the repo's own
/// script rather than running CMake itself, because the script already
/// carries the toolchain checks and the PATH fix picotool needs, and rule 2
/// puts the build in one place instead of two that drift.
///
/// The refusals are duplicated from tools/gen_library.py on purpose, and
/// only the ones that depend on nothing but the recipe: a name too wide for
/// a menu row, a character the console's font cannot draw, a game listed
/// twice, an empty menu. That generator stays the authority (it runs in CI,
/// where this tool does not), but a build that takes minutes is a bad place
/// to learn that a name is one letter too long.
/// </summary>
public sealed class ConsoleYamlBackend : IConsoleBackend
{
    // Must match tools/gen_library.py, which must match console/src/menu.cpp.
    private const int NameRoomPx = 189;
    private const int NameScale = 2;
    private const int GlyphAdvance = 6;
    private const int TitleRoomPx = 240 - 16;

    private readonly string _repositoryRoot;
    private HashSet<char>? _charset;

    public ConsoleYamlBackend(string repositoryRoot)
    {
        _repositoryRoot = repositoryRoot;
    }

    public string Description => "console.yaml, built by build_console.bat";

    private string ConfigPath => Path.Combine(_repositoryRoot, "console.yaml");
    private string GamesDirectory => Path.Combine(_repositoryRoot, "games");
    private string BuildScript => Path.Combine(_repositoryRoot, "build_console.bat");
    private string FontPath =>
        Path.Combine(_repositoryRoot, "engine", "font", "console5x7.txt");

    public bool CanBuild(out string reason)
    {
        if (!Directory.Exists(GamesDirectory) || !File.Exists(BuildScript))
        {
            reason = "This is not a checkout of the repository, so there is " +
                     "nothing to build from. Point the folder box at one.";
            return false;
        }
        reason = "";
        return true;
    }

    // ---- discovery ----

    public IReadOnlyList<AvailableGame> DiscoverGames()
    {
        var games = new List<AvailableGame>();
        if (!Directory.Exists(GamesDirectory)) return games;

        try
        {
            foreach (var directory in Directory.EnumerateDirectories(GamesDirectory))
            {
                var yml = Path.Combine(directory, "game.yml");
                if (!File.Exists(yml)) continue;

                var fields = ReadFlatYaml(yml);
                var slug = fields.GetValueOrDefault("slug")
                           ?? Path.GetFileName(directory);
                // Rule 6: the console links every game into one binary, which
                // only works for the one SDK. A game built against another is
                // not a choice to offer and then refuse.
                if (fields.GetValueOrDefault("sdk", "32blit") != "32blit") continue;

                var thumbnail = Path.Combine(directory, "thumbnail.png");
                games.Add(new AvailableGame(
                    slug,
                    fields.GetValueOrDefault("title") ?? slug,
                    fields.GetValueOrDefault("blurb") ?? "",
                    File.Exists(thumbnail) ? thumbnail : null));
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }

        return games.OrderBy(game => game.Title, StringComparer.OrdinalIgnoreCase)
                    .ToList();
    }

    // ---- reading and writing the recipe ----

    public ConsoleRecipe Load()
    {
        if (!File.Exists(ConfigPath)) return ConsoleRecipe.Empty;

        var title = "PICO CONSOLE";
        var entries = new List<RecipeEntry>();
        var inMenu = false;
        string? pendingSlug = null;
        string? pendingName = null;

        void FlushGame()
        {
            if (pendingSlug is null) return;
            entries.Add(new GameEntry(pendingSlug, pendingName));
            pendingSlug = null;
            pendingName = null;
        }

        foreach (var raw in SafeReadLines(ConfigPath))
        {
            var line = raw.Split('#', 2)[0].TrimEnd();
            if (line.Trim().Length == 0) continue;

            var indent = line.Length - line.TrimStart().Length;
            var stripped = line.Trim();

            if (indent == 0 && !stripped.StartsWith("-"))
            {
                FlushGame();
                var (key, value) = SplitField(stripped);
                if (key == "menu") { inMenu = true; continue; }
                inMenu = false;
                if (key == "title") title = value;
                continue;
            }

            if (!inMenu) continue;

            if (stripped.StartsWith("-"))
            {
                FlushGame();
                stripped = stripped[1..].Trim();
            }

            var (itemKey, itemValue) = SplitField(stripped);
            switch (itemKey)
            {
                case "heading":
                    FlushGame();
                    entries.Add(new HeadingEntry(itemValue));
                    break;
                case "game":
                    pendingSlug = itemValue;
                    break;
                case "name":
                    pendingName = itemValue;
                    break;
            }
        }
        FlushGame();

        return new ConsoleRecipe(title, entries);
    }

    /// <summary>
    /// Writes the recipe back, keeping whatever comment block the file opens
    /// with. That header explains the format to anyone editing by hand, and
    /// a tool that silently ate the documentation for the file it rewrites
    /// would be a bad trade for a GUI.
    /// </summary>
    public void Save(ConsoleRecipe recipe)
    {
        var text = new StringBuilder();
        foreach (var line in LeadingComment()) text.AppendLine(line);
        if (text.Length > 0) text.AppendLine();

        text.AppendLine($"title: {recipe.Title}");
        text.AppendLine();
        text.AppendLine("menu:");
        foreach (var entry in recipe.Entries)
        {
            switch (entry)
            {
                case HeadingEntry heading:
                    text.AppendLine($"  - heading: {heading.Text}");
                    break;
                case GameEntry game:
                    text.AppendLine($"  - game: {game.Slug}");
                    if (!string.IsNullOrWhiteSpace(game.Name))
                    {
                        text.AppendLine($"      name: {game.Name}");
                    }
                    break;
            }
        }

        File.WriteAllText(ConfigPath, text.ToString());
    }

    private IReadOnlyList<string> LeadingComment()
    {
        var header = new List<string>();
        foreach (var line in SafeReadLines(ConfigPath))
        {
            var trimmed = line.TrimStart();
            if (trimmed.StartsWith("#")) { header.Add(line); continue; }
            if (trimmed.Length == 0 && header.Count > 0) continue;
            break;
        }
        return header;
    }

    // ---- validation ----

    public IReadOnlyList<Problem> Validate(ConsoleRecipe recipe)
    {
        var problems = new List<Problem>();
        var charset = Charset();
        // Discovered once for the whole pass. This used to be a filesystem
        // scan per row, through NameOf, on every keystroke in the title box.
        var known = DiscoverGames().ToDictionary(game => game.Slug);

        var title = DisplayName(recipe.Title);
        if (title.Length == 0)
        {
            problems.Add(new Problem(-1, "The console needs a title."));
        }
        else if (Missing(title, charset) is { Length: > 0 } missing)
        {
            problems.Add(new Problem(-1,
                $"The title cannot be drawn: the font has no {missing}."));
        }
        else if (Width(title) > TitleRoomPx)
        {
            problems.Add(new Problem(-1,
                $"The title is {Width(title)} pixels wide and the header has " +
                $"room for {TitleRoomPx}. About {TitleRoomPx / NameScale / GlyphAdvance} " +
                "characters fit."));
        }

        var seen = new Dictionary<string, int>();
        var playable = 0;

        for (var i = 0; i < recipe.Entries.Count; i++)
        {
            switch (recipe.Entries[i])
            {
                case HeadingEntry heading:
                {
                    var text = DisplayName(heading.Text);
                    if (text.Length == 0)
                    {
                        problems.Add(new Problem(i, "A heading needs some text."));
                    }
                    else if (Missing(text, charset) is { Length: > 0 } gone)
                    {
                        problems.Add(new Problem(i,
                            $"\"{text}\" cannot be drawn: the font has no {gone}."));
                    }
                    break;
                }

                case GameEntry game:
                {
                    playable++;
                    if (!known.ContainsKey(game.Slug))
                    {
                        problems.Add(new Problem(i,
                            $"There is no game called {game.Slug} in games/."));
                    }
                    if (seen.TryGetValue(game.Slug, out var first))
                    {
                        problems.Add(new Problem(i,
                            $"{game.Slug} is already on the menu at row {first + 1}. " +
                            "Two rows running the same game share its save and " +
                            "its records."));
                    }
                    else
                    {
                        seen[game.Slug] = i;
                    }

                    var name = DisplayName(
                        !string.IsNullOrWhiteSpace(game.Name) ? game.Name!
                        : known.TryGetValue(game.Slug, out var found) ? found.Title
                        : game.Slug);
                    if (name.Length == 0)
                    {
                        problems.Add(new Problem(i, "This row has no name to draw."));
                    }
                    else if (Missing(name, charset) is { Length: > 0 } gone)
                    {
                        problems.Add(new Problem(i,
                            $"\"{name}\" cannot be drawn: the font has no {gone}."));
                    }
                    else if (Width(name) > NameRoomPx)
                    {
                        problems.Add(new Problem(i,
                            $"\"{name}\" is {Width(name)} pixels wide and a menu " +
                            $"row has room for {NameRoomPx}. Rename it to about " +
                            $"{(NameRoomPx / NameScale + 1) / GlyphAdvance} characters."));
                    }
                    break;
                }
            }
        }

        if (playable == 0)
        {
            problems.Add(new Problem(-1,
                "No games on the menu, so the console would boot to an empty one."));
        }

        return problems;
    }

    /// <summary>Upper cased, whitespace collapsed: what gen_library.py draws.</summary>
    public static string DisplayName(string text) =>
        string.Join(" ", (text ?? "").ToUpperInvariant()
                                     .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));

    public static int Width(string text) =>
        text.Length == 0 ? 0 : (text.Length * GlyphAdvance - 1) * NameScale;

    private static string Missing(string text, IReadOnlyCollection<char> charset)
    {
        var gone = text.Where(c => !charset.Contains(c)).Distinct().ToList();
        if (gone.Count == 0) return "";
        return string.Join(", ", gone.Select(c => c == ' ' ? "space" : $"'{c}'"));
    }

    /// <summary>
    /// The characters engine/font/console5x7.txt has a picture for. Parsed
    /// rather than assumed: the font is editable art, so a glyph added there
    /// should widen what this accepts without anyone remembering to come
    /// here. Falls back to A-Z, 0-9 and space when the file cannot be read,
    /// which under-accepts rather than promising something undrawable.
    /// </summary>
    private HashSet<char> Charset()
    {
        if (_charset is not null) return _charset;

        var found = new HashSet<char>();
        try
        {
            var lines = File.ReadAllLines(FontPath);
            for (var i = 0; i < lines.Length; i++)
            {
                var line = lines[i];
                if (!line.StartsWith("@")) continue;
                found.Add(line.Length > 1 ? line[1] : ' ');
                i += 7;   // the glyph's 7 picture rows are not headers
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }

        if (found.Count == 0)
        {
            foreach (var c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ") found.Add(c);
        }
        _charset = found;
        return found;
    }

    // ---- building ----

    public async Task<BuildOutcome> BuildAsync(ConsoleRecipe recipe,
                                               IProgress<string> log,
                                               CancellationToken token)
    {
        if (!CanBuild(out var why)) return new BuildOutcome(false, why, null);

        var problems = Validate(recipe);
        if (problems.Count > 0)
        {
            return new BuildOutcome(false, problems[0].Message, null);
        }

        try
        {
            Save(recipe);
        }
        catch (Exception error)
        {
            return new BuildOutcome(false,
                $"Could not write console.yaml: {error.Message}", null);
        }
        log.Report($"Wrote {ConfigPath}");

        var start = new ProcessStartInfo
        {
            FileName = "cmd.exe",
            // /c with the script quoted: the repository path can contain spaces.
            Arguments = $"/c \"\"{BuildScript}\"\"",
            WorkingDirectory = _repositoryRoot,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        try
        {
            using var process = new Process { StartInfo = start };
            process.OutputDataReceived += (_, e) =>
            {
                if (e.Data is not null) log.Report(e.Data);
            };
            process.ErrorDataReceived += (_, e) =>
            {
                if (e.Data is not null) log.Report(e.Data);
            };

            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync(token).ConfigureAwait(false);

            if (process.ExitCode != 0)
            {
                return new BuildOutcome(false,
                    $"The build failed (exit code {process.ExitCode}). The log " +
                    "above says why.", null);
            }
        }
        catch (OperationCanceledException)
        {
            return new BuildOutcome(false, "Build cancelled.", null);
        }
        catch (Exception error)
        {
            return new BuildOutcome(false, $"Could not run the build: {error.Message}", null);
        }

        var uf2 = NewestConsoleUf2();
        if (uf2 is null)
        {
            return new BuildOutcome(false,
                "The build reported success but produced no console.uf2.", null);
        }

        var games = recipe.Entries.OfType<GameEntry>().Count();
        return new BuildOutcome(true,
            $"Built {Path.GetFileName(uf2)}: {games} game(s), " +
            $"{new FileInfo(uf2).Length / 1024} KB.", uf2);
    }

    private string? NewestConsoleUf2()
    {
        var directory = Path.Combine(_repositoryRoot, "build.console");
        if (!Directory.Exists(directory)) return null;
        try
        {
            return Directory
                .EnumerateFiles(directory, "console.uf2", SearchOption.AllDirectories)
                .OrderByDescending(File.GetLastWriteTime)
                .FirstOrDefault();
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    // ---- small helpers ----

    private static (string Key, string Value) SplitField(string text)
    {
        var at = text.IndexOf(':');
        if (at < 0) return (text.Trim(), "");
        return (text[..at].Trim(), text[(at + 1)..].Trim().Trim('"').Trim('\''));
    }

    private static Dictionary<string, string> ReadFlatYaml(string path)
    {
        var fields = new Dictionary<string, string>();
        foreach (var raw in SafeReadLines(path))
        {
            var line = raw.Split('#', 2)[0].TrimEnd();
            if (line.Length == 0 || char.IsWhiteSpace(line[0]) || !line.Contains(':')) continue;
            var (key, value) = SplitField(line);
            fields[key] = value;
        }
        return fields;
    }

    private static string[] SafeReadLines(string path)
    {
        try { return File.Exists(path) ? File.ReadAllLines(path) : Array.Empty<string>(); }
        catch (IOException) { return Array.Empty<string>(); }
        catch (UnauthorizedAccessException) { return Array.Empty<string>(); }
    }
}
