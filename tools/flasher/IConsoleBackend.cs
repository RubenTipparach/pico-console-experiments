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
