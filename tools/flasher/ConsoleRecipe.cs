namespace PicoFlasher;

/// <summary>
/// One row of the console's menu, as the person building it thinks of it:
/// a game, or a heading the cursor steps over.
///
/// Deliberately says nothing about how a console gets built. This is the
/// vocabulary the UI works in, and it survived the last backend change only
/// because it does not know there was one.
/// </summary>
public abstract record RecipeEntry;

public sealed record HeadingEntry(string Text) : RecipeEntry;

/// <summary>
/// A game on the menu. <paramref name="Name"/> is null when the game's own
/// title is wanted, which is the usual case: a name here is an override,
/// so an untouched entry keeps following game.yml when that changes.
/// </summary>
public sealed record GameEntry(string Slug, string? Name) : RecipeEntry;

/// <summary>What is on the console, in order, and what it is called.</summary>
public sealed record ConsoleRecipe(string Title, IReadOnlyList<RecipeEntry> Entries)
{
    public static ConsoleRecipe Empty { get; } =
        new("PICO CONSOLE", Array.Empty<RecipeEntry>());
}

/// <summary>A game that could go on the console, and what is known about it.</summary>
public sealed record AvailableGame(string Slug, string Title, string Blurb,
                                   string? ThumbnailPath)
{
    public override string ToString() => Title;
}

/// <summary>
/// Something worth saying about this recipe.
/// <paramref name="EntryIndex"/> is the row it belongs to, or -1 for the
/// recipe as a whole, so the UI can point at the offending row instead of
/// printing a wall of text.
///
/// <paramref name="Blocking"/> false is a note rather than a fault: the thing
/// it describes is fine, it is just worth knowing. A name too wide for a menu
/// row is the case that made this necessary, since the menu scrolls one now
/// and the tool used to refuse to build at all.
/// </summary>
public sealed record Problem(int EntryIndex, string Message, bool Blocking = true);

public sealed record BuildOutcome(bool Success, string Message, string? Uf2Path);

/// <summary>
/// What a built console costs on its board, measured from the artifact.
///
/// <paramref name="StaticRam"/> is data + bss, and the ceiling it is compared
/// against is the whole of SRAM rather than what is left over, because the
/// SDK's framebuffer is counted in bss: 115,200 bytes of it on a PicoSystem
/// and 307,200 on a Tufty, which double buffers hires by default on RP2350.
///
/// <paramref name="Games"/> is how many were on the menu when this was built,
/// so the UI can say whether the number still describes the list on screen.
/// </summary>
public sealed record ConsoleSize(int Flash, int StaticRam, int Games,
                                 DateTime BuiltAt, BoardSpec Board)
{
    public bool RamIsTight => StaticRam > Board.SramWarn;
}
