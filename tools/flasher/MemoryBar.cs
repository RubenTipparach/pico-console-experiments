namespace PicoFlasher;

/// <summary>
/// What the last console build cost, as two bars against the board's ceilings.
///
/// Reported, never predicted, and the distinction is the whole design. Every
/// game is linked into one binary against one shared copy of the engine and
/// the SDK, so what the second game adds is nothing like what the first one
/// did, and there is no honest way to total a list of games into a number
/// without building it. A bar that guessed would be wrong in a way nobody
/// could check, which is worse than a bar that says "not built yet".
///
/// So it shows the real figures from the newest .elf in the selected board's
/// build directory, and greys itself when the menu on screen is not the menu
/// those figures came from.
///
/// RAM is the bar worth watching. The ceiling is the whole of SRAM rather than
/// what is left over, because the SDK's framebuffer is counted in bss: 115,200
/// bytes of it on a PicoSystem, 307,200 on a Tufty, which double buffers hires
/// by default on RP2350. Flash is on a 12 MB program region and is nowhere
/// near tight, so it is drawn thinner and mostly there for scale.
/// </summary>
public sealed class MemoryBar : Control
{
    private ConsoleSize? _size;
    private bool _stale;
    private string _note = "not built yet";

    public MemoryBar()
    {
        // Painted rather than composed out of ProgressBars: a themed
        // ProgressBar animates on value change and cannot be coloured by
        // threshold without owner drawing it anyway.
        SetStyle(ControlStyles.AllPaintingInWmPaint
                 | ControlStyles.OptimizedDoubleBuffer
                 | ControlStyles.UserPaint
                 | ControlStyles.ResizeRedraw, true);
        Width = 260;
        Height = 40;
        Margin = new Padding(18, 0, 0, 0);
    }

    /// <summary>
    /// <paramref name="stale"/> when the menu has been edited since this was
    /// built, so the numbers describe a different console to the one on screen.
    /// </summary>
    public void Show(ConsoleSize? size, bool stale, string note)
    {
        _size = size;
        _stale = stale;
        _note = note;
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(BackColor);

        using var font = new Font(Font.FontFamily, 7.5F);
        using var text = new SolidBrush(_stale
            ? SystemColors.GrayText
            : SystemColors.ControlText);

        if (_size is null)
        {
            g.DrawString(_note, font, text, 0, 12);
            return;
        }

        var ram = Fraction(_size.StaticRam, _size.Board.Sram);
        var flash = Fraction(_size.Flash, _size.Board.Flash);

        // Amber past the warning line, red at the ceiling. The linker catches a
        // real overflow on its own; this is the earlier warning worth having,
        // because by then the message is `region RAM overflowed` with nothing
        // in it about which game grew.
        var ramColour = _size.StaticRam >= _size.Board.Sram
            ? Color.FromArgb(198, 48, 48)
            : _size.RamIsTight
                ? Color.FromArgb(214, 152, 24)
                : Color.FromArgb(56, 142, 88);
        if (_stale) ramColour = Color.FromArgb(150, ramColour);

        DrawBar(g, 0, ram, ramColour, 12);
        DrawBar(g, 16, flash, _stale
            ? Color.FromArgb(150, 120, 130, 150)
            : Color.FromArgb(120, 130, 150), 7);

        var label = $"RAM {Kb(_size.StaticRam)} / {Kb(_size.Board.Sram)}   " +
                    $"flash {Kb(_size.Flash)}";
        if (_stale) label += "   (menu edited since)";
        g.DrawString(label, font, text, 0, 26);
    }

    private void DrawBar(Graphics g, int top, double fraction, Color colour,
                         int height)
    {
        var width = Math.Max(40, Width - 2);
        using var back = new SolidBrush(Color.FromArgb(232, 232, 236));
        using var fill = new SolidBrush(colour);
        using var edge = new Pen(Color.FromArgb(200, 200, 206));
        g.FillRectangle(back, 0, top, width, height);
        g.FillRectangle(fill, 0, top, (int)Math.Round(width * fraction), height);
        g.DrawRectangle(edge, 0, top, width, height);
    }

    private static double Fraction(int used, int total) =>
        total <= 0 ? 0 : Math.Clamp((double)used / total, 0, 1);

    private static string Kb(int bytes) => $"{bytes / 1024}K";
}
