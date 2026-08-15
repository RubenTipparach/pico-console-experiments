namespace PicoFlasher;

/// <summary>Which console a build is for.</summary>
public enum TargetBoard
{
    PicoSystem,
    Tufty2350,
}

/// <summary>
/// Everything that differs between the two boards, in one place.
///
/// It is a short list and every entry on it has already been got wrong once:
/// build_console.bat hardcoded the PicoSystem's toolchain and then reported an
/// RP2350 build against the RP2040's 270,336 bytes of SRAM, which is a console
/// with half the chip spare being called an overrun.
/// </summary>
/// <remarks>
/// BootselLabel is the volume label the bootrom presents in BOOTSEL.
/// Family is the UF2 family a build for this board carries.
/// ConsoleScript and ConsoleBuildDirectory are repo relative.
/// Sram is the whole chip, which the static footprint is measured against,
/// and SramWarn is where the warning starts, leaving room for stack and heap.
/// Flash is the program region: both boards carry 16 MB with the top 4 MB
/// given to the storage partition, see STORAGE.md.
/// </remarks>
public sealed record BoardSpec(
    TargetBoard Board,
    string Name,
    string Chip,
    string BootselLabel,
    uint Family,
    string ConsoleScript,
    string ConsoleBuildDirectory,
    int Sram,
    int SramWarn,
    int Flash)
{
    public static readonly BoardSpec PicoSystem = new(
        TargetBoard.PicoSystem, "PicoSystem", "RP2040", "RPI-RP2", 0xE48BFF56,
        "build_console.bat", "build.console",
        Sram: 270_336, SramWarn: 230_000, Flash: 12 * 1024 * 1024);

    public static readonly BoardSpec Tufty2350 = new(
        TargetBoard.Tufty2350, "Tufty 2350", "RP2350", "RP2350", 0xE48BFF59,
        "build_console_tufty.bat", "build.console.tufty",
        Sram: 532_480, SramWarn: 480_000, Flash: 12 * 1024 * 1024);

    public static IReadOnlyList<BoardSpec> All { get; } =
        new[] { PicoSystem, Tufty2350 };

    public static BoardSpec For(TargetBoard board) =>
        board == TargetBoard.Tufty2350 ? Tufty2350 : PicoSystem;

    /// <summary>The board a BOOTSEL drive belongs to, or null if unrecognised.</summary>
    public static BoardSpec? ForDriveLabel(string? label) =>
        All.FirstOrDefault(spec =>
            string.Equals(spec.BootselLabel, label,
                          StringComparison.OrdinalIgnoreCase));

    public override string ToString() => Name;
}

/// <summary>
/// Reads which board a .uf2 was built for, out of the file itself.
///
/// This exists because the tool could not tell, and neither can the hardware
/// in any way you would notice: the bootrom checks the family field and
/// silently ignores every block that does not match, so flashing a PicoSystem
/// build at a Tufty copies the file, reports success, and does nothing at all.
/// Both boards now produce a console.uf2 and a catcoin.uf2, so the two are one
/// mis-click apart.
///
/// The format is Microsoft's UF2: 512 byte blocks, each with a magic number at
/// both ends and a 32 bit family id at offset 28, valid only when bit 13 of
/// the flags says so. Verified against real builds of both boards, games and
/// consoles, where every block parsed and the families came out as expected.
/// </summary>
public static class Uf2Family
{
    private const uint Magic0 = 0x0A324655;
    private const uint Magic1 = 0x9E5D5157;
    private const uint MagicEnd = 0x0AB16F30;
    private const uint FamilyIdPresent = 0x00002000;

    /// <summary>
    /// RP2350 builds carry a second family alongside their own, for blocks
    /// that are position independent data rather than code. It says nothing
    /// about which chip the file is for, so it must not be used to identify
    /// one: taken as an answer it makes every Tufty build ambiguous.
    /// </summary>
    private const uint AbsoluteFamily = 0xE48BFF57;

    /// <summary>
    /// The board this file is for, or null when it cannot be determined:
    /// an unreadable file, something that is not a .uf2 at all, or a family
    /// belonging to neither board. Null always means "do not know", never
    /// "wrong", so callers let it through rather than blocking on a guess.
    /// </summary>
    public static BoardSpec? Identify(string path)
    {
        var families = FamiliesIn(path);
        foreach (var spec in BoardSpec.All)
        {
            if (families.Contains(spec.Family)) return spec;
        }
        return null;
    }

    /// <summary>Every family id carried by the blocks of a .uf2.</summary>
    public static HashSet<uint> FamiliesIn(string path)
    {
        var families = new HashSet<uint>();
        try
        {
            using var stream = File.OpenRead(path);
            var block = new byte[512];
            // A whole console is a few thousand blocks and the answer is in
            // the first one, but a bounded scan costs nothing and survives a
            // file that starts with padding.
            var limit = 64;
            while (limit-- > 0 && ReadExactly(stream, block))
            {
                if (ReadU32(block, 0) != Magic0) break;
                if (ReadU32(block, 4) != Magic1) break;
                if (ReadU32(block, 508) != MagicEnd) break;
                if ((ReadU32(block, 8) & FamilyIdPresent) == 0) continue;

                var family = ReadU32(block, 28);
                if (family != AbsoluteFamily) families.Add(family);
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        return families;
    }

    private static bool ReadExactly(Stream stream, byte[] buffer)
    {
        var got = 0;
        while (got < buffer.Length)
        {
            var read = stream.Read(buffer, got, buffer.Length - got);
            if (read <= 0) return false;
            got += read;
        }
        return true;
    }

    private static uint ReadU32(byte[] buffer, int at) =>
        (uint)(buffer[at] | (buffer[at + 1] << 8)
               | (buffer[at + 2] << 16) | (buffer[at + 3] << 24));
}
