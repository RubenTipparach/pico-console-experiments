namespace PicoFlasher;

/// <summary>
/// The name and picture a game carries inside its own .uf2.
///
/// Every device build compiles a fixed block into a `.pse_meta` section: magic,
/// slug, title, version, and a 48x48 RGB565 icon. See tools/game_meta.py, which
/// writes it, and LAUNCHER.md for why it exists. This is the reader, so the
/// tool can show a game's name and icon without a sidecar file to keep in sync.
///
/// The layout is duplicated here rather than shared, because the alternative is
/// shipping a Python interpreter inside a single .exe. Both sides are covered by
/// tests that use the same fixed offsets, so a change on either side shows up.
/// </summary>
public sealed record GameMeta(string Slug, string Title, string Version, Bitmap? Icon);

/// <summary>One game inside a .uf2, and where in flash it lives.</summary>
public sealed record SlotEntry(int Slot, uint Address, int Bytes, GameMeta? Meta)
{
    public string Display => Meta is null
        ? $"slot {Slot}  (no metadata)  {Bytes / 1024} KB"
        : $"slot {Slot}  {Meta.Title}  {Meta.Version}  {Bytes / 1024} KB";
}

public static class GameMetaReader
{
    private static readonly byte[] Magic =
        { (byte)'P', (byte)'S', (byte)'E', (byte)'G', (byte)'A', (byte)'M',
          (byte)'E', (byte)'1' };

    public const int HeaderSize = 96;
    public const int IconWidth = 48;
    public const int IconHeight = 48;
    public const int BlockSize = HeaderSize + IconWidth * IconHeight * 2;

    private const int OffsetIconWidth = 10;
    private const int OffsetIconHeight = 12;
    private const int OffsetSlug = 16;
    private const int OffsetTitle = 40;
    private const int OffsetVersion = 72;

    private const int SlugMax = 24;
    private const int TitleMax = 32;
    private const int VersionMax = 16;

    /// <summary>
    /// Flattens a .uf2 into the flash image it would write, plus the lowest
    /// address it targets. Gaps read as zero, which is what the device sees for
    /// anything the image does not cover.
    /// </summary>
    public static bool TryReadImage(byte[] uf2, out uint baseAddress, out byte[] image)
    {
        baseAddress = 0;
        image = Array.Empty<byte>();
        if (uf2.Length == 0 || uf2.Length % 512 != 0) return false;

        var lowest = uint.MaxValue;
        var highest = 0u;
        var count = uf2.Length / 512;

        for (var i = 0; i < count; i++)
        {
            var at = i * 512;
            if (BitConverter.ToUInt32(uf2, at) != 0x0A324655u) return false;
            if (BitConverter.ToUInt32(uf2, at + 4) != 0x9E5D5157u) return false;
            var address = BitConverter.ToUInt32(uf2, at + 12);
            var size = BitConverter.ToUInt32(uf2, at + 16);
            if (size > 256) return false;
            if (address < lowest) lowest = address;
            if (address + size > highest) highest = address + size;
        }

        if (lowest == uint.MaxValue || highest <= lowest) return false;

        baseAddress = lowest;
        image = new byte[highest - lowest];
        for (var i = 0; i < count; i++)
        {
            var at = i * 512;
            var address = BitConverter.ToUInt32(uf2, at + 12);
            var size = (int)BitConverter.ToUInt32(uf2, at + 16);
            Array.Copy(uf2, at + 32, image, (int)(address - lowest), size);
        }
        return true;
    }

    /// <summary>Finds the metadata block in a flash image, or null.</summary>
    public static GameMeta? Find(byte[] image, int from = 0, int to = -1)
    {
        if (to < 0 || to > image.Length) to = image.Length;
        var last = to - BlockSize;
        // The block is four byte aligned by the compiler, so stepping by four
        // is both safe and quick over a whole slot.
        for (var i = Math.Max(0, from); i <= last; i += 4)
        {
            var hit = true;
            for (var m = 0; m < Magic.Length; m++)
            {
                if (image[i + m] != Magic[m]) { hit = false; break; }
            }
            if (hit) return Decode(image, i);
        }
        return null;
    }

    /// <summary>Reads a .uf2 from disk and returns its metadata, or null.</summary>
    public static GameMeta? FromFile(string path)
    {
        try
        {
            var bytes = File.ReadAllBytes(path);
            if (!TryReadImage(bytes, out _, out var image)) return null;
            return Find(image);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    private static GameMeta Decode(byte[] image, int at)
    {
        var slug = Text(image, at + OffsetSlug, SlugMax);
        var title = Text(image, at + OffsetTitle, TitleMax);
        var version = Text(image, at + OffsetVersion, VersionMax);

        var width = BitConverter.ToUInt16(image, at + OffsetIconWidth);
        var height = BitConverter.ToUInt16(image, at + OffsetIconHeight);
        Bitmap? icon = null;
        // Only draw an icon this build knows the size of. A future block with a
        // bigger picture must come back as no picture, never as a read past the
        // end of the one it has.
        if (width == IconWidth && height == IconHeight
            && at + BlockSize <= image.Length)
        {
            icon = DecodeIcon(image, at + HeaderSize);
        }

        return new GameMeta(slug, title, version, icon);
    }

    private static Bitmap DecodeIcon(byte[] image, int at)
    {
        var bitmap = new Bitmap(IconWidth, IconHeight);
        for (var y = 0; y < IconHeight; y++)
        {
            for (var x = 0; x < IconWidth; x++)
            {
                var index = at + (y * IconWidth + x) * 2;
                var value = (ushort)(image[index] | (image[index + 1] << 8));
                // Replicate the high bits into the low ones so full white stays
                // white rather than drifting grey.
                var r = ((value >> 11) & 0x1F) * 255 / 31;
                var g = ((value >> 5) & 0x3F) * 255 / 63;
                var b = (value & 0x1F) * 255 / 31;
                bitmap.SetPixel(x, y, Color.FromArgb(r, g, b));
            }
        }
        return bitmap;
    }

    private static string Text(byte[] image, int at, int size)
    {
        var length = 0;
        while (length < size && at + length < image.Length
               && image[at + length] != 0)
        {
            length++;
        }
        return System.Text.Encoding.UTF8.GetString(image, at, length);
    }
}
