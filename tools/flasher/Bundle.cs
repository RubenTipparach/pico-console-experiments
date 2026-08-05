namespace PicoFlasher;

/// <summary>
/// Composes the launcher and a set of games into one flashable .uf2, and reads
/// an existing one back.
///
/// A UF2 block carries its own destination address, so a bundle is the
/// launcher's blocks plus each game's blocks with the numbering redone. No
/// relinking and no patching: a game built with -DPICO_SLOT=n already points
/// where it belongs.
///
/// This mirrors tools/make_bundle.py, including its refusals, because the
/// failures it prevents all look the same from outside: a console that boots
/// into the wrong thing, or does not boot, with nothing to read but a black
/// screen. The slot map is duplicated in three places on purpose
/// (cmake/slot.cmake links to it, launcher/src/library.hpp scans by it) and a
/// test asserts all of them agree.
/// </summary>
public static class Bundle
{
    public const uint FlashBase = 0x10000000;
    public const int SlotSize = 512 * 1024;
    public const int SlotCount = 23;

    private const uint Magic0 = 0x0A324655;
    private const uint Magic1 = 0x9E5D5157;
    private const uint MagicEnd = 0x0AB16F30;
    private const uint FlagFamilyId = 0x00002000;
    private const uint Rp2040FamilyId = 0xE48BFF56;
    private const int Payload = 256;

    public static uint SlotAddress(int slot) =>
        FlashBase + (uint)slot * SlotSize;

    public sealed record Block(uint Address, byte[] Data);

    public sealed record Result(bool Success, string Message, byte[]? Uf2 = null);

    /// <summary>A game and the slot it is going into.</summary>
    /// <summary>
    /// A game and the slot it is going into. Forced means the file was not
    /// linked for that slot at all (no -DPICO_SLOT build, most likely
    /// something this project never built): Compose relocates its blocks to
    /// the slot address instead of refusing the mismatch, and skips the
    /// metadata block requirement, naming it through the launcher's own
    /// override table instead. Relocating the envelope only moves where the
    /// bytes land; it does not fix any absolute address the game's own code
    /// still carries for wherever it actually thinks it is, so a forced game
    /// is not guaranteed to run once selected. See OverrideTable.cs.
    /// </summary>
    public sealed record Placement(string Name, int Slot, string Path, bool Forced = false);

    public static IReadOnlyList<Block>? ReadBlocks(byte[] uf2)
    {
        if (uf2.Length == 0 || uf2.Length % 512 != 0) return null;
        var blocks = new List<Block>(uf2.Length / 512);
        for (var at = 0; at < uf2.Length; at += 512)
        {
            if (BitConverter.ToUInt32(uf2, at) != Magic0) return null;
            if (BitConverter.ToUInt32(uf2, at + 4) != Magic1) return null;
            if (BitConverter.ToUInt32(uf2, at + 508) != MagicEnd) return null;
            var address = BitConverter.ToUInt32(uf2, at + 12);
            var size = (int)BitConverter.ToUInt32(uf2, at + 16);
            if (size > Payload) return null;
            var data = new byte[size];
            Array.Copy(uf2, at + 32, data, 0, size);
            blocks.Add(new Block(address, data));
        }
        return blocks;
    }

    public static byte[] WriteBlocks(IReadOnlyList<Block> blocks)
    {
        var output = new byte[blocks.Count * 512];
        for (var i = 0; i < blocks.Count; i++)
        {
            var at = i * 512;
            BitConverter.GetBytes(Magic0).CopyTo(output, at);
            BitConverter.GetBytes(Magic1).CopyTo(output, at + 4);
            BitConverter.GetBytes(FlagFamilyId).CopyTo(output, at + 8);
            BitConverter.GetBytes(blocks[i].Address).CopyTo(output, at + 12);
            BitConverter.GetBytes((uint)Payload).CopyTo(output, at + 16);
            BitConverter.GetBytes((uint)i).CopyTo(output, at + 20);
            BitConverter.GetBytes((uint)blocks.Count).CopyTo(output, at + 24);
            BitConverter.GetBytes(Rp2040FamilyId).CopyTo(output, at + 28);
            blocks[i].Data.CopyTo(output, at + 32);
            BitConverter.GetBytes(MagicEnd).CopyTo(output, at + 508);
        }
        return output;
    }

    private static (uint Low, uint High) Extent(IReadOnlyList<Block> blocks)
    {
        var low = uint.MaxValue;
        var high = 0u;
        foreach (var block in blocks)
        {
            if (block.Address < low) low = block.Address;
            var end = block.Address + (uint)block.Data.Length;
            if (end > high) high = end;
        }
        return (low, high);
    }

    /// <summary>
    /// Builds a bundle. Every refusal here is a console that would boot into
    /// the wrong thing, so none of them are warnings.
    /// </summary>
    public static Result Compose(string launcherPath,
                                 IReadOnlyList<Placement> games)
    {
        byte[] launcherBytes;
        try
        {
            launcherBytes = File.ReadAllBytes(launcherPath);
        }
        catch (Exception error)
        {
            return new Result(false, $"Could not read the launcher: {error.Message}");
        }

        var blocks = ReadBlocks(launcherBytes);
        if (blocks is null)
        {
            return new Result(false, "The launcher is not a UF2 image.");
        }

        var launcherExtent = Extent(blocks);
        if (launcherExtent.Low != FlashBase)
        {
            return new Result(false,
                $"The launcher starts at 0x{launcherExtent.Low:X8}, not the base " +
                "of flash. It has to own the reset vector.");
        }
        if (launcherExtent.High > SlotAddress(1))
        {
            return new Result(false,
                $"The launcher runs to 0x{launcherExtent.High:X8} and would " +
                $"overwrite slot 1 at 0x{SlotAddress(1):X8}.");
        }

        var all = new List<Block>(blocks);
        var used = new Dictionary<int, string>();

        // Every game gets its title patched into the launcher's own
        // override table, not only a forced one: the launcher checks that
        // table first and trusts it without needing to find this slot's own
        // metadata block by scanning raw flash (launcher/src/library.cpp
        // read_slot). Looked up once, lazily, so a bundle with no games at
        // all never touches it.
        var titles = new Dictionary<int, string>();
        int overrideTableOffset = -1;

        foreach (var game in games)
        {
            if (game.Slot < 1 || game.Slot > SlotCount)
            {
                return new Result(false,
                    $"{game.Name}: slot {game.Slot} is outside 1..{SlotCount}.");
            }
            if (used.TryGetValue(game.Slot, out var already))
            {
                return new Result(false,
                    $"Slot {game.Slot} is claimed by both {already} and {game.Name}.");
            }
            used[game.Slot] = game.Name;

            byte[] gameBytes;
            try
            {
                gameBytes = File.ReadAllBytes(game.Path);
            }
            catch (Exception error)
            {
                return new Result(false, $"Could not read {game.Name}: {error.Message}");
            }

            var gameBlocks = ReadBlocks(gameBytes);
            if (gameBlocks is null)
            {
                return new Result(false, $"{game.Name} is not a UF2 image.");
            }

            var extent = Extent(gameBlocks);
            var want = SlotAddress(game.Slot);

            if (overrideTableOffset == -1)
            {
                var launcherImage = FlattenBlocks(blocks, launcherExtent.Low,
                                                   launcherExtent.High);
                overrideTableOffset = OverrideTable.Find(launcherImage);
                if (overrideTableOffset == -1)
                {
                    return new Result(false,
                        $"This launcher build has no override table to name " +
                        $"{game.Name} with. Rebuild the launcher with build_bundle.bat.");
                }
            }
            titles[game.Slot] = game.Name;

            if (game.Forced)
            {
                // Not linked for this slot at all, most likely a file this
                // project never built. Shifting every block's address by the
                // same delta is the one thing fixable without the game's own
                // source: it is what stops the flash write from landing on
                // top of the launcher or another game. It does not touch any
                // absolute address the game's own instructions still carry
                // for wherever they actually think they are, which is why
                // this is not a substitute for a real -DPICO_SLOT build.
                var delta = unchecked(want - extent.Low);
                gameBlocks = gameBlocks
                    .Select(b => new Block(unchecked(b.Address + delta), b.Data))
                    .ToList();
                extent = (want, unchecked(extent.High + delta));
            }
            else if (extent.Low != want)
            {
                return new Result(false,
                    $"{game.Name} was linked at 0x{extent.Low:X8} but is going " +
                    $"into slot {game.Slot} at 0x{want:X8}. It needs building " +
                    $"with -DPICO_SLOT={game.Slot}.");
            }

            if (extent.High > want + SlotSize)
            {
                return new Result(false,
                    $"{game.Name} needs {extent.High - extent.Low} bytes and a " +
                    $"slot holds {SlotSize}.");
            }

            if (!game.Forced)
            {
                // Every game this project actually builds carries its own
                // metadata block (tools/game_meta.py); a properly slot
                // linked game with none is a build misconfiguration worth
                // refusing, even though the override table patched in below
                // would still make it listed and nameable on its own. A
                // forced game never had a block to check in the first
                // place, so it skips straight to that override.
                if (!GameMetaReader.TryReadImage(gameBytes, out _, out var image)
                    || GameMetaReader.Find(image) is null)
                {
                    return new Result(false,
                        $"{game.Name} has no metadata block. It should have been " +
                        "built with build_bundle.bat, which never produces one " +
                        "without it.");
                }
            }

            all.AddRange(gameBlocks);
        }

        // BOOTSEL flashing is incremental: a slot you did not include in this
        // bundle is left exactly as it was, not cleared. That is normal UF2
        // behaviour, not a bug, and it is not this method's job to guess at
        // fixing it: composing a bundle should write what you asked for,
        // nothing more. (An earlier version of this tried to defensively
        // blank every unused slot on every flash, which turned a routine
        // flash into a multi-megabyte write over a slow interface for no
        // benefit most bundles will ever need.) If a stale slot from a
        // previous flash ever needs clearing, that is a separate, deliberate
        // action, not automatic overhead on every compose.

        // Patch every game's title into the launcher's own blocks, not only
        // a forced one's: see the field comment on `titles` above.
        // `blocks` and `all` share the same Block objects for the launcher's
        // portion, so patching Data in place here is visible in `all` too.
        foreach (var (slot, name) in titles)
        {
            var titleAddress = unchecked(launcherExtent.Low + (uint)overrideTableOffset
                + OverrideTable.MagicSize + (uint)(slot - 1) * OverrideTable.TitleSize);
            if (!PatchBytes(blocks, titleAddress, OverrideTable.EncodeTitle(name)))
            {
                return new Result(false,
                    $"{name}: could not patch its title into the launcher's " +
                    "override table.");
            }
        }

        // Written in ascending address order regardless of what order games
        // were added or reordered in: Move up/down changes menu presentation
        // only (Build's own comment says so), but the RP2040 bootloader
        // erases and programs flash as blocks arrive, and there is nothing
        // documented that says it tolerates them arriving out of address
        // order. A bundle recomposed after reordering games was going out
        // block for block identical except for this ordering, which is
        // exactly the kind of thing that looks fine here and corrupts a
        // slot's flash on the actual device.
        var ordered = all.OrderBy(block => block.Address).ToList();

        return new Result(true,
            $"Bundle ready: {games.Count} game(s), {ordered.Count * 512 / 1024} KB.",
            WriteBlocks(ordered));
    }

    /// <summary>
    /// A separate, deliberate action for the thing Compose used to do on
    /// every flash and shouldn't: wipe every game slot back to empty.
    ///
    /// Not the full 512KB of every slot. The whole device is a 16MB flash
    /// chip (confirmed against Pimoroni's own spec, not assumed), BOOTSEL
    /// exposes it as a fake drive with that same 16MB ceiling, and the UF2
    /// format costs 512 bytes on disk per 256 bytes of real flash data,
    /// exactly doubling a payload's size. Fully blanking all 23 slots is
    /// 23 * 512KB = 11.5MB of real data, a 23.0MB file (measured, not
    /// estimated: WriteBlocks produced exactly 24,117,248 bytes), bigger
    /// than the drive it would have to fit through. Not slow, not stuck:
    /// too large to write at all, full stop.
    ///
    /// ClearBytesPerSlot has to reach past wherever a game's own metadata
    /// block landed, not just its vector table: clearing only the 4KB
    /// vector table sector left a stale game unbootable but still listed in
    /// the menu, which does not read as "cleared" from the screen. Measured
    /// real flash content (uf2 file size / 2) of every game actually built
    /// or imported into the library as of this writing:
    ///
    ///   launcher.uf2     101,376 B   chicken.uf2      110,080 B
    ///   pico-santa.uf2   118,528 B   dustrider.uf2    132,096 B
    ///   kingfisher.uf2   141,056 B   raycaster.uf2    146,432 B
    ///   Daft-Freak.uf2   153,344 B   celeste.uf2      300,288 B
    ///   pico3d.uf2       323,584 B (largest, a foreign import)
    ///
    /// The largest game this project actually builds is kingfisher at
    /// 141,056 bytes; 262,144 (256KB) covers it with 121,088 bytes to
    /// spare. 23 slots * 256KB = 5.75MB real data, an 11.5MB file: well
    /// under the 16MB ceiling, with margin, and a number that is on the
    /// record rather than picked to sound safe. A foreign game force added
    /// at a size larger than this (celeste and pico3d both are) is not
    /// fully covered by this bound, but a forced game's name is never
    /// read from its own slot in the first place, only from the launcher's
    /// override table (see OverrideTable.cs), so its own uncleared tail is
    /// display-inert: reaching its vector table is what matters, and 256KB
    /// clears that for every slot regardless of what was there.
    /// The launcher itself (slot 0) is never touched.
    /// </summary>
    public static byte[] ComposeClearAllSlots()
    {
        const int ClearBytesPerSlot = 256 * 1024;
        var blocks = new List<Block>();
        for (var slot = 1; slot <= SlotCount; slot++)
        {
            var address = SlotAddress(slot);
            for (var offset = 0; offset < ClearBytesPerSlot; offset += Payload)
            {
                var data = new byte[Payload];
                Array.Fill(data, (byte)0xFF);
                blocks.Add(new Block(unchecked(address + (uint)offset), data));
            }
        }
        return WriteBlocks(blocks);
    }

    private static byte[] FlattenBlocks(IReadOnlyList<Block> blocks, uint low, uint high)
    {
        var image = new byte[high - low];
        foreach (var block in blocks)
        {
            Array.Copy(block.Data, 0, image, (int)(block.Address - low), block.Data.Length);
        }
        return image;
    }

    /// <summary>
    /// Writes `data` at `address` across whichever blocks cover that range.
    /// A block's Data array is mutated in place, which is visible through
    /// every reference to that same Block, not just this list's.
    /// </summary>
    private static bool PatchBytes(IReadOnlyList<Block> blocks, uint address, byte[] data)
    {
        var written = 0;
        while (written < data.Length)
        {
            var target = unchecked(address + (uint)written);
            var block = blocks.FirstOrDefault(
                b => target >= b.Address && target < b.Address + (uint)b.Data.Length);
            if (block is null) return false;

            var offsetInBlock = (int)(target - block.Address);
            var count = Math.Min(data.Length - written, block.Data.Length - offsetInBlock);
            Array.Copy(data, written, block.Data, offsetInBlock, count);
            written += count;
        }
        return true;
    }

    /// <summary>
    /// Reads what is in a bundle (or a single game): which slots are filled and
    /// what is in each. This is how the tool shows the contents of a .uf2 that
    /// came from CI rather than from here.
    /// </summary>
    public static IReadOnlyList<SlotEntry> Describe(byte[] uf2)
    {
        var entries = new List<SlotEntry>();
        var blocks = ReadBlocks(uf2);
        if (blocks is null) return entries;

        var bySlot = new Dictionary<int, List<Block>>();
        foreach (var block in blocks)
        {
            var slot = (int)((block.Address - FlashBase) / SlotSize);
            if (!bySlot.TryGetValue(slot, out var list))
            {
                list = new List<Block>();
                bySlot[slot] = list;
            }
            list.Add(block);
        }

        foreach (var slot in bySlot.Keys.OrderBy(key => key))
        {
            var list = bySlot[slot];
            var extent = Extent(list);
            var image = new byte[extent.High - extent.Low];
            foreach (var block in list)
            {
                block.Data.CopyTo(image, (int)(block.Address - extent.Low));
            }
            var meta = slot == 0 ? null : GameMetaReader.Find(image);
            entries.Add(new SlotEntry(slot, extent.Low,
                                      (int)(extent.High - extent.Low), meta));
        }
        return entries;
    }
}
