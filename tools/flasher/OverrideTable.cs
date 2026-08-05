namespace PicoFlasher;

/// <summary>
/// The launcher's own name-for-a-game-that-cannot-name-itself table
/// (launcher/src/override_table.hpp): a reserved, all-zero-by-default region
/// baked into every launcher build. Bundle.Compose patches a slot's title
/// directly into these bytes for a "force added" game with no metadata block
/// of its own, which is how the on-device menu can show a name for it at
/// all. The layout here is a byte offset contract with that C++ header, not
/// a struct one compiler could lay out differently from another: magic,
/// then one k_max_slots run of TitleSize byte fields, slot n at [n - 1].
/// </summary>
public static class OverrideTable
{
    private static readonly byte[] Magic =
        { (byte)'P', (byte)'S', (byte)'E', (byte)'O', (byte)'V', (byte)'R',
          (byte)'0', (byte)'1' };

    public const int MagicSize = 8;
    public const int TitleSize = 32;
    public const int MaxSlots = 23;
    public const int TableSize = MagicSize + MaxSlots * TitleSize;

    /// <summary>Finds the table inside a flattened image, or -1.</summary>
    public static int Find(byte[] image)
    {
        var last = image.Length - TableSize;
        for (var i = 0; i <= last; i += 4)
        {
            var hit = true;
            for (var m = 0; m < MagicSize; m++)
            {
                if (image[i + m] != Magic[m]) { hit = false; break; }
            }
            if (hit) return i;
        }
        return -1;
    }

    /// <summary>
    /// A title field, NUL padded, silently truncated to fit: the same
    /// contract copy_field on the device side already applies to every other
    /// field a foreign source could hand it.
    /// </summary>
    public static byte[] EncodeTitle(string title)
    {
        var field = new byte[TitleSize];
        var encoded = System.Text.Encoding.UTF8.GetBytes(title);
        Array.Copy(encoded, field, Math.Min(encoded.Length, TitleSize - 1));
        return field;
    }
}
