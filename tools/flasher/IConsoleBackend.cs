namespace PicoFlasher;

/// <summary>
/// Everything the console tab needs, and the only thing it is allowed to
/// know about how a console is actually made.
///
/// This boundary exists because it was missing once and cost the feature.
/// The tool used to compose a multi game bundle by assigning flash slots and
/// concatenating per slot builds, with that knowledge spread through the UI:
/// slot numbers in the list, address arithmetic in the buttons. When the
/// console became one binary with every game linked in, none of that was
/// true any more, and picking games from a window went away with it, even
/// though picking games is a thing a person wants to do regardless of how
/// the bytes get made.
///
/// So the UI works in games and rows (ConsoleRecipe) and nothing else. A
/// backend turns that into a .uf2 however the build currently works. Replace
/// the implementation, keep the window.
/// </summary>
public interface IConsoleBackend
{
    /// <summary>How this backend makes a console, for the status line.</summary>
    string Description { get; }

    /// <summary>
    /// Which console the next build is for.
    ///
    /// A target is not a game or a row, so by the note above it does not
    /// obviously belong here. It is here anyway because the alternative is
    /// worse: the two boards do not share a toolchain, a build directory or an
    /// SRAM ceiling, and a backend that cannot be told which one it is making
    /// leaves the window guessing on behalf of the build. Auto detection is
    /// not available at build time either, since there may be no board plugged
    /// in at all.
    /// </summary>
    TargetBoard Board { get; set; }

    /// <summary>
    /// Named game lists that can be recalled later, newest name last.
    ///
    /// These are saved recipes and nothing more. They are NOT the bundle
    /// composer this tool used to have, which assigned flash slots and
    /// concatenated per slot builds; that is gone and is not coming back
    /// (CLAUDE.md rule 8). Every one of these builds the same single binary
    /// with every listed game linked into it, exactly as the unsaved list
    /// does. The only thing being stored is which games, in what order.
    /// </summary>
    IReadOnlyList<string> ListBundles();

    /// <summary>Recalls a saved list. Unknown names give an empty recipe.</summary>
    ConsoleRecipe LoadBundle(string name);

    /// <summary>Stores this list under a name, replacing one of that name.</summary>
    void SaveBundle(string name, ConsoleRecipe recipe);

    /// <summary>
    /// What the last build for the current board actually cost, or null when
    /// there is no build to measure. Reported rather than predicted: the
    /// console links every game into one binary, so what a game adds depends
    /// on what is already in there, and a made up number on a progress bar is
    /// worse than an empty one.
    /// </summary>
    ConsoleSize? MeasureLastBuild();

    /// <summary>
    /// Whether a build could run here at all, and if not, why in a sentence
    /// a person can act on. Building needs a toolchain that a downloaded
    /// .exe on a fresh machine will not have, and finding that out after a
    /// two minute wait is worse than being told up front.
    /// </summary>
    bool CanBuild(out string reason);

    /// <summary>Games that could go on a console, whether or not they are on one.</summary>
    IReadOnlyList<AvailableGame> DiscoverGames();

    /// <summary>What the console is currently set up to hold.</summary>
    ConsoleRecipe Load();

    /// <summary>
    /// Records this as what the console should hold, without building it.
    /// Editing a list and closing the window should not lose the edit just
    /// because there was no time to wait for a compile.
    /// </summary>
    void Save(ConsoleRecipe recipe);

    /// <summary>
    /// Everything wrong with this recipe, empty when it would build. Run as
    /// the user edits, so a name too wide for a menu row is a red row here
    /// rather than a failed build two minutes later.
    /// </summary>
    IReadOnlyList<Problem> Validate(ConsoleRecipe recipe);

    /// <summary>
    /// Makes the console. <paramref name="log"/> receives build output as it
    /// arrives, since this takes long enough that silence reads as a hang.
    /// </summary>
    Task<BuildOutcome> BuildAsync(ConsoleRecipe recipe, IProgress<string> log,
                                  CancellationToken token);
}
