namespace PicomonEditor;

public enum Tool
{
    Paint,
    Fill,
    Rectangle,
}

/// <summary>
/// The grid: tiles painted from the palette, and the placed things drawn on top
/// of them as markers that can be dragged.
///
/// Markers win the mouse wherever they are, so a left click on one picks it up
/// instead of painting under it. That costs the ability to paint the tile a
/// marker is standing on with the brush, which the fill and rectangle tools
/// still reach, and buys a marker that can always be grabbed. A marker that
/// sometimes moves and sometimes gets painted over would be worse.
/// </summary>
public sealed class MapView : Control
{
    public const int MinZoom = 6;
    public const int MaxZoom = 40;

    private Dataset? _data;
    private ZoneFile? _zone;
    private int _zoom = 16;
    private object? _selected;

    private bool _painting;
    private IPlaced? _dragging;
    private Point _dragFrom;
    private Point? _rectFrom;
    private Point _mouse = new(-1, -1);

    public Tool Tool { get; set; } = Tool.Paint;
    public char BrushChar { get; set; } = '.';

    /// <summary>Where the last left click landed, which is where a new NPC,
    /// warp or event gets placed. Putting it under the last click beats putting
    /// it at 0,0 and making the first act on every new thing a drag.</summary>
    public Point LastClick { get; private set; } = new(0, 0);

    public event Action? Edited;
    public event Action<object?>? SelectionChanged;
    public event Action<char>? Sampled;
    public event Action<string>? Hovered;

    public MapView()
    {
        DoubleBuffered = true;
        BackColor = Color.FromArgb(24, 24, 28);
    }

    public ZoneFile? Zone
    {
        get => _zone;
        set { _zone = value; _selected = null; _rectFrom = null; UpdateExtent(); Invalidate(); }
    }

    public Dataset? Data
    {
        get => _data;
        set { _data = value; Invalidate(); }
    }

    public int Zoom
    {
        get => _zoom;
        set { _zoom = Math.Clamp(value, MinZoom, MaxZoom); UpdateExtent(); Invalidate(); }
    }

    public object? Selected
    {
        get => _selected;
        set { _selected = value; Invalidate(); }
    }

    /// <summary>The control is the whole map, and the panel around it does the
    /// scrolling, so its size is the map's size in pixels.</summary>
    private void UpdateExtent()
    {
        Size = _zone is null
            ? new Size(1, 1)
            : new Size(Math.Max(1, _zone.Width * _zoom), Math.Max(1, _zone.Height * _zoom));
    }

    /// <summary>Puts a tile in the middle of the view, which is how the problem
    /// list jumps to the thing it is complaining about.</summary>
    public void EnsureVisible(int x, int y)
    {
        if (Parent is not ScrollableControl scroller) return;
        var view = scroller.ClientSize;
        var target = new Point(x * _zoom + _zoom / 2, y * _zoom + _zoom / 2);
        scroller.AutoScrollPosition = new Point(
            Math.Max(0, target.X - view.Width / 2),
            Math.Max(0, target.Y - view.Height / 2));
    }

    // ----------------------------------------------------------------- paint

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        if (_zone is null || _data is null) return;

        var g = e.Graphics;
        var clip = e.ClipRectangle;
        var x0 = Math.Max(0, clip.Left / _zoom);
        var y0 = Math.Max(0, clip.Top / _zoom);
        var x1 = Math.Min(_zone.Width - 1, (clip.Right - 1) / _zoom);
        var y1 = Math.Min(_zone.Height - 1, (clip.Bottom - 1) / _zoom);
        var labelled = _zoom >= 12;
        using var font = new Font(FontFamily.GenericMonospace, Math.Max(6f, _zoom * 0.5f));
        using var gridPen = new Pen(Color.FromArgb(40, 0, 0, 0));

        for (var y = y0; y <= y1; y++)
        {
            for (var x = x0; x <= x1; x++)
            {
                var cell = new Rectangle(x * _zoom, y * _zoom, _zoom, _zoom);
                var ch = _zone.TileAt(x, y);
                // Magenta is not a colour any tileset would choose, which is the
                // point: a character with no tile behind it has to look wrong.
                var colour = _data.TileByChar.TryGetValue(ch, out var tile)
                    ? tile.Colour
                    : Color.Magenta;
                using (var fill = new SolidBrush(colour)) g.FillRectangle(fill, cell);
                if (labelled && ch != '\0')
                {
                    TextRenderer.DrawText(g, ch.ToString(), font, cell,
                        Readable(colour),
                        TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                }
                if (_zoom >= 10) g.DrawRectangle(gridPen, cell);
            }
        }

        DrawSightLines(g);

        foreach (var npc in _zone.Npcs) DrawMarker(g, npc, npc.X, npc.Y, Color.FromArgb(60, 110, 220), Initial(npc.Id));
        foreach (var ev in _zone.Events) DrawMarker(g, ev, ev.X, ev.Y, Color.FromArgb(210, 140, 30), Initial(ev.Kind));
        foreach (var warp in _zone.Warps) DrawMarker(g, warp, warp.X, warp.Y, Color.FromArgb(40, 160, 90), "W");

        if (_rectFrom is { } from && Tool == Tool.Rectangle)
        {
            var box = Span(from, _mouse);
            using var pen = new Pen(Color.White, 2) { DashStyle = System.Drawing.Drawing2D.DashStyle.Dot };
            g.DrawRectangle(pen, new Rectangle(box.X * _zoom, box.Y * _zoom,
                                               box.Width * _zoom, box.Height * _zoom));
        }
    }

    /// <summary>A trainer's sight line is drawn on the ground in the game while
    /// it is unbeaten, so the trap is visible and avoidable. Drawing it here too
    /// is the only way to see what a sight of 4 actually covers.</summary>
    private void DrawSightLines(Graphics g)
    {
        if (_zone is null) return;
        using var brush = new SolidBrush(Color.FromArgb(60, 255, 80, 80));
        foreach (var npc in _zone.Npcs)
        {
            if (npc.EffectiveKind != "trainer" || (npc.Sight ?? 0) <= 0) continue;
            var (dx, dy) = Step(npc.Facing);
            for (var i = 1; i <= npc.Sight!.Value; i++)
            {
                var x = npc.X + dx * i;
                var y = npc.Y + dy * i;
                if (!_zone.Contains(x, y)) break;
                g.FillRectangle(brush, x * _zoom, y * _zoom, _zoom, _zoom);
            }
        }
    }

    private void DrawMarker(Graphics g, object thing, int x, int y, Color colour, string label)
    {
        var cell = new Rectangle(x * _zoom + 1, y * _zoom + 1, _zoom - 2, _zoom - 2);
        if (cell.Width < 3 || cell.Height < 3) return;
        using (var fill = new SolidBrush(Color.FromArgb(220, colour))) g.FillEllipse(fill, cell);
        using (var edge = new Pen(Color.FromArgb(230, 250, 250, 250))) g.DrawEllipse(edge, cell);
        if (_zoom >= 12)
        {
            using var font = new Font(Font.FontFamily, Math.Max(6f, _zoom * 0.42f), FontStyle.Bold);
            TextRenderer.DrawText(g, label, font, cell, Color.White,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
        if (ReferenceEquals(thing, _selected))
        {
            using var ring = new Pen(Color.White, 2);
            g.DrawRectangle(ring, x * _zoom, y * _zoom, _zoom - 1, _zoom - 1);
        }
    }

    private static string Initial(string text) =>
        text.Length == 0 ? "?" : text[..1].ToUpperInvariant();

    /// <summary>Near black on a light tile, near white on a dark one. The
    /// tileset picks its own colours, so the character drawn over them cannot be
    /// one fixed shade. Solid, not translucent: TextRenderer is GDI and drops
    /// the alpha channel.</summary>
    private static Color Readable(Color colour) =>
        colour.R * 299 + colour.G * 587 + colour.B * 114 > 140 * 1000
            ? Color.FromArgb(30, 30, 30)
            : Color.FromArgb(225, 225, 225);

    private static (int, int) Step(string facing) => facing switch
    {
        "north" => (0, -1),
        "south" => (0, 1),
        "east" => (1, 0),
        "west" => (-1, 0),
        _ => (0, 0),
    };

    private static Rectangle Span(Point a, Point b) => new(
        Math.Min(a.X, b.X), Math.Min(a.Y, b.Y),
        Math.Abs(a.X - b.X) + 1, Math.Abs(a.Y - b.Y) + 1);

    // ----------------------------------------------------------------- mouse

    private Point Cell(Point p) => new(p.X / _zoom, p.Y / _zoom);

    public IPlaced? Hit(int x, int y)
    {
        if (_zone is null) return null;
        // NPCs first, then events, then warps: a warp is usually under a door
        // and rarely the thing being reached for, and an NPC is always the
        // thing being reached for.
        foreach (var npc in _zone.Npcs) if (npc.X == x && npc.Y == y) return npc;
        foreach (var ev in _zone.Events) if (ev.X == x && ev.Y == y) return ev;
        foreach (var warp in _zone.Warps) if (warp.X == x && warp.Y == y) return warp;
        return null;
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        // A plain Control is not focused by clicking it, and without focus the
        // map never sees Delete or Escape.
        Focus();
        if (_zone is null) return;
        var cell = Cell(e.Location);
        if (!_zone.Contains(cell.X, cell.Y)) return;

        if (e.Button == MouseButtons.Right)
        {
            var ch = _zone.TileAt(cell.X, cell.Y);
            if (ch != '\0') Sampled?.Invoke(ch);
            return;
        }
        if (e.Button != MouseButtons.Left) return;

        LastClick = cell;
        var hit = Hit(cell.X, cell.Y);
        if (hit is not null)
        {
            Selected = hit;
            SelectionChanged?.Invoke(hit);
            _dragging = hit;
            _dragFrom = cell;
            return;
        }

        switch (Tool)
        {
            case Tool.Paint:
                _painting = true;
                Stroke(cell);
                break;
            case Tool.Fill:
                Fill(cell);
                break;
            case Tool.Rectangle:
                _rectFrom = cell;
                break;
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (_zone is null) return;
        var cell = Cell(e.Location);
        if (cell != _mouse)
        {
            _mouse = cell;
            Report(cell);
            if (_rectFrom is not null) Invalidate();
        }
        if (!_zone.Contains(cell.X, cell.Y)) return;

        if (_dragging is not null && cell != new Point(_dragging.X, _dragging.Y))
        {
            _dragging.X = cell.X;
            _dragging.Y = cell.Y;
            _zone.Dirty = true;
            Edited?.Invoke();
            Invalidate();
        }
        else if (_painting)
        {
            Stroke(cell);
        }
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        _painting = false;
        if (_dragging is not null)
        {
            if (_dragFrom != new Point(_dragging.X, _dragging.Y)) Edited?.Invoke();
            _dragging = null;
        }
        if (_rectFrom is { } from && _zone is not null)
        {
            var box = Span(from, Cell(e.Location));
            for (var y = box.Top; y < box.Bottom; y++)
                for (var x = box.Left; x < box.Right; x++)
                    _zone.SetTile(x, y, BrushChar);
            _rectFrom = null;
            Edited?.Invoke();
            Invalidate();
        }
    }

    protected override void OnMouseLeave(EventArgs e)
    {
        base.OnMouseLeave(e);
        _mouse = new Point(-1, -1);
        Hovered?.Invoke("");
    }

    private void Report(Point cell)
    {
        if (_zone is null || _data is null || !_zone.Contains(cell.X, cell.Y))
        {
            Hovered?.Invoke("");
            return;
        }
        var ch = _zone.TileAt(cell.X, cell.Y);
        var name = _data.TileByChar.TryGetValue(ch, out var tile) ? tile.Name : "not in the tileset";
        var thing = Hit(cell.X, cell.Y);
        Hovered?.Invoke($"{ZoneFile.N(cell.X)},{ZoneFile.N(cell.Y)}  '{ch}' {name}"
                        + (thing is null ? "" : "   " + thing.Label));
    }

    private void Stroke(Point cell)
    {
        if (_zone is null || _zone.TileAt(cell.X, cell.Y) == BrushChar) return;
        _zone.SetTile(cell.X, cell.Y, BrushChar);
        Invalidate(new Rectangle(cell.X * _zoom, cell.Y * _zoom, _zoom, _zoom));
        Edited?.Invoke();
    }

    /// <summary>Four way flood fill over the character that was clicked.</summary>
    private void Fill(Point seed)
    {
        if (_zone is null) return;
        var target = _zone.TileAt(seed.X, seed.Y);
        if (target == BrushChar || target == '\0') return;

        var pending = new Stack<Point>();
        pending.Push(seed);
        while (pending.Count > 0)
        {
            var at = pending.Pop();
            if (!_zone.Contains(at.X, at.Y) || _zone.TileAt(at.X, at.Y) != target) continue;
            _zone.SetTile(at.X, at.Y, BrushChar);
            pending.Push(new Point(at.X + 1, at.Y));
            pending.Push(new Point(at.X - 1, at.Y));
            pending.Push(new Point(at.X, at.Y + 1));
            pending.Push(new Point(at.X, at.Y - 1));
        }
        Invalidate();
        Edited?.Invoke();
    }
}
