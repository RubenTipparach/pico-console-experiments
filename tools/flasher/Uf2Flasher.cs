namespace PicoFlasher;

public sealed record FlashResult(bool Success, string Message);

/// <summary>
/// Copies a .uf2 onto a board in BOOTSEL mode.
///
/// The important thing to understand here is that the copy is *supposed* to
/// fail at the end. The RP2040 bootrom arms its reboot the instant the last UF2
/// block is written, with a watchdog delay measured in a single second. Anything
/// the host does after that final data sector, flushing, updating the directory
/// entry, setting timestamps, closing the handle, is talking to a device that
/// has already left the bus. The resulting IOException is the success signal,
/// not an error, and every UF2 bootloader behaves this way.
///
/// The other consequence of that design: RPI-RP2 is a fake FAT16 volume with no
/// backing storage. Sectors are parsed for UF2 magic as they are written. So the
/// destination filename is irrelevant, and reading the file back to verify it is
/// meaningless because there is nothing there to read.
/// </summary>
public static class Uf2Flasher
{
    // One 4KB flash sector's worth of UF2 blocks. Used to be 64KB: the RP2040
    // bootrom erases a sector (tens of milliseconds on the W25Q parts these
    // boards actually carry) before it can program the blocks landing in it,
    // and WriteThrough only forces .NET's own buffering, it says nothing
    // about whether the device has actually finished committing a chunk
    // before the next one arrives over USB. A multi-game bundle large enough
    // to span many sectors showed exactly the symptom that mismatch would
    // produce: the first and last parts of a write landing correctly while a
    // slot in the middle came back completely erased, as if that whole
    // stretch had never been written at all. Flushing after every sector
    // sized chunk instead of every 64KB gives the device sixteen times as
    // many chances to catch up before more data queues up behind it.
    private const int BlockSize = 4 * 1024;
    private const uint Uf2Magic0 = 0x0A324655;   // "UF2\n"
    private const uint Uf2Magic1 = 0x9E5D5157;

    public static FlashResult Flash(string sourcePath, string driveRoot,
                                    IProgress<int>? progress = null)
    {
        byte[] payload;
        try
        {
            // Read it all up front. Streaming from disk while the destination is
            // disappearing turns one failure mode into two.
            payload = File.ReadAllBytes(sourcePath);
        }
        catch (Exception error)
        {
            return new FlashResult(false, $"Could not read the .uf2: {error.Message}");
        }

        if (!LooksLikeUf2(payload))
        {
            return new FlashResult(false,
                "That file is not a UF2 image. Flashing it would do nothing.");
        }

        var destination = Path.Combine(driveRoot, "FIRMWARE.UF2");
        var wroteEverything = false;

        try
        {
            // WriteThrough so blocks reach the device as they are written rather
            // than sitting in the cache until close, by which time the board is
            // already rebooting.
            var stream = new FileStream(destination, FileMode.Create, FileAccess.Write,
                                        FileShare.None, BlockSize,
                                        FileOptions.WriteThrough);
            try
            {
                var written = 0;
                while (written < payload.Length)
                {
                    var count = Math.Min(BlockSize, payload.Length - written);
                    stream.Write(payload, written, count);
                    // Flushed per chunk, not just once at the end: this is
                    // what actually gives the device a chance to finish
                    // committing one sector before the next is queued up
                    // behind it, which WriteThrough alone does not guarantee.
                    stream.Flush();
                    written += count;
                    progress?.Report((int)(100L * written / payload.Length));
                }
                wroteEverything = true;
            }
            finally
            {
                // Dispose flushes, so it throws the same IOException, and an
                // exception out of Dispose inside a `using` would propagate.
                // That is why this is not a `using`.
                try { stream.Dispose(); }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }
        catch (IOException) when (wroteEverything)
        {
            return new FlashResult(true, "Flashed. The board has rebooted.");
        }
        catch (UnauthorizedAccessException) when (wroteEverything)
        {
            return new FlashResult(true, "Flashed. The board has rebooted.");
        }
        catch (Exception error)
        {
            return new FlashResult(false, $"Flashing failed: {error.Message}");
        }

        return new FlashResult(true, "Flashed. The board has rebooted.");
    }

    /// <summary>Checks the first block's UF2 magic, so a mis-picked file is
    /// caught before the board is touched.</summary>
    public static bool LooksLikeUf2(byte[] data)
    {
        if (data.Length < 512 || data.Length % 512 != 0) return false;
        var magic0 = BitConverter.ToUInt32(data, 0);
        var magic1 = BitConverter.ToUInt32(data, 4);
        return magic0 == Uf2Magic0 && magic1 == Uf2Magic1;
    }
}
