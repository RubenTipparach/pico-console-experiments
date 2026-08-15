namespace PicoFlasher;

/// <summary>
/// A game's thumbnail at icon size, or a placeholder when it has none.
///
/// Shared by the Console tab and the Play tab because both show the same
/// shelf of games, and a second copy of this would be a second chance to
/// forget the file locking note below.
/// </summary>
public static class GameIcon
{
    public const int Size = 48;

    public static Image Load(string? path)
    {
        if (path is not null)
        {
            try
            {
                // Through a copy in memory so the file is not left locked:
                // the build rewrites thumbnails, and a tool holding one open
                // fails that build with a permission error nobody expects.
                using var stream = new MemoryStream(File.ReadAllBytes(path));
                using var original = Image.FromStream(stream);
                return new Bitmap(original, new Size(Size, Size));
            }
            catch (IOException) { }
            catch (ArgumentException) { }
        }

        var placeholder = new Bitmap(Size, Size);
        using var graphics = Graphics.FromImage(placeholder);
        graphics.Clear(Color.FromArgb(58, 58, 68));
        using var pen = new Pen(Color.FromArgb(150, 150, 160), 3);
        graphics.DrawLine(pen, 8, 8, Size - 8, Size - 8);
        return placeholder;
    }
}
