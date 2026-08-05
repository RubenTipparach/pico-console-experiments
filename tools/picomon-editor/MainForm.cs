namespace PicomonEditor;

/// <summary>
/// One window: the palette on the left, the map in the middle, whatever is
/// selected on the right, and everything wrong with the data along the bottom.
///
/// Quick and dirty on purpose, the same way the flasher is. No installer, no
/// document model, no undo. The one thing it takes seriously is that a file it
/// writes is a file the build accepts: the checks are the compiler's checks,
/// they run on every edit, and Save refuses while the open zone has an error in
/// it. An editor that lets a broken map through has cost more time than it
/// saved.
/// </summary>
public sealed class MainForm : Form
{
    private readonly Settings _settings = Settings.Load();

    private readonly TextBox _dataPath = new();
    private readonly Button _browse = new();
    private readonly ComboBox _zones = new();
    private readonly Button _save = new();
    private readonly Button _reload = new();
    private readonly NumericUpDown _zoom = new();
    private readonly RadioButton _paint = new();
    private readonly RadioButton _fill = new();
    private readonly RadioButton _rectangle = new();

    private readonly ListBox _palette = new();
    private readonly Panel _viewport = new();
    private readonly MapView _map = new();

    private readonly TabControl _tabs = new();
    private readonly TabPage _thingsPage = new("Placed");
    private readonly TabPage _encountersPage = new("Encounters");
    private readonly ListBox _things = new();
    private readonly List<object> _thingList = new();
    private readonly Inspector _inspector = new();
    private readonly EncounterPanel _encounters = new();
    private readonly Button _addNpc = new();
    private readonly Button _addWarp = new();
    private readonly Button _addEvent = new();
    private readonly Button _delete = new();

    private readonly ListView _problems = new();
    private readonly Label _status = new();

    private Dataset? _data;
    private ZoneFile? _zone;
    private List<Problem> _found = new();
    private bool _syncing;

    public MainForm()
    {
        Text = "PicomonEditor";
        Width = 1280;
        Height = 860;
        MinimumSize = new Size(1000, 700);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9F);
        KeyPreview = true;

        BuildToolbar();
        BuildPalette();
        BuildMap();
        BuildSidePanel();
        BuildProblems();

        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 4 };
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 150));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var middle = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 1 };
        middle.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 200));
        middle.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        middle.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 360));
        middle.Controls.Add(_palette, 0, 0);
        middle.Controls.Add(_viewport, 1, 0);
        middle.Controls.Add(_tabs, 2, 0);

        layout.Controls.Add(Toolbar(), 0, 0);
        layout.Controls.Add(middle, 0, 1);
        layout.Controls.Add(_problems, 0, 2);
        layout.Controls.Add(_status, 0, 3);
        Controls.Add(layout);

        KeyDown += OnKeyDown;
        FormClosing += OnFormClosing;

        var start = Dataset.LooksLikeDataDirectory(_settings.DataDirectory)
            ? _settings.DataDirectory
            : Settings.GuessDataDirectory();
        if (start.Length > 0) OpenData(start, _settings.LastZone);
        else _status.Text = "Point the editor at games/picomon/data with Folder...";
    }

    // -------------------------------------------------------------- building

    private readonly TableLayoutPanel _toolbar = new();

    private void BuildToolbar()
    {
        _dataPath.ReadOnly = true;
        _dataPath.Dock = DockStyle.Fill;
        _dataPath.Margin = new Padding(0, 4, 6, 4);

        _browse.Text = "Folder...";
        _browse.AutoSize = true;
        _browse.Click += (_, _) => BrowseForData();

        _zones.DropDownStyle = ComboBoxStyle.DropDownList;
        _zones.Width = 160;
        _zones.Margin = new Padding(12, 4, 6, 4);
        _zones.SelectedIndexChanged += (_, _) =>
        {
            if (_syncing || _zones.SelectedItem is not string id) return;
            OpenZone(id);
        };

        _save.Text = "Save";
        _save.AutoSize = true;
        _save.Click += (_, _) => SaveCurrent();

        _reload.Text = "Reload";
        _reload.AutoSize = true;
        _reload.Click += (_, _) => ReloadData();

        _paint.Text = "Paint";
        _fill.Text = "Fill";
        _rectangle.Text = "Rect";
        _paint.Checked = true;
        foreach (var (button, tool) in new[]
                 {
                     (_paint, Tool.Paint), (_fill, Tool.Fill), (_rectangle, Tool.Rectangle),
                 })
        {
            button.Appearance = Appearance.Button;
            button.AutoSize = true;
            button.Margin = new Padding(0, 4, 4, 4);
            var chosen = tool;
            button.CheckedChanged += (_, _) => { if (button.Checked) _map.Tool = chosen; };
        }

        _zoom.Minimum = MapView.MinZoom;
        _zoom.Maximum = MapView.MaxZoom;
        _zoom.Increment = 2;
        _zoom.Width = 56;
        _zoom.Value = Math.Clamp(_settings.Zoom, MapView.MinZoom, MapView.MaxZoom);
        _zoom.Margin = new Padding(12, 4, 4, 4);
        _zoom.ValueChanged += (_, _) =>
        {
            _map.Zoom = (int)_zoom.Value;
            _settings.Zoom = _map.Zoom;
        };
    }

    private Control Toolbar()
    {
        _toolbar.Dock = DockStyle.Fill;
        _toolbar.AutoSize = true;
        _toolbar.ColumnCount = 5;
        _toolbar.RowCount = 1;
        _toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (var i = 0; i < 4; i++) _toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        var right = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        right.Controls.Add(_zones);
        right.Controls.Add(_save);
        right.Controls.Add(_reload);
        right.Controls.Add(new Label { Text = "tool", AutoSize = true, Margin = new Padding(12, 8, 4, 0) });
        right.Controls.Add(_paint);
        right.Controls.Add(_fill);
        right.Controls.Add(_rectangle);
        right.Controls.Add(new Label { Text = "zoom", AutoSize = true, Margin = new Padding(12, 8, 4, 0) });
        right.Controls.Add(_zoom);

        _toolbar.Controls.Add(_dataPath, 0, 0);
        _toolbar.Controls.Add(_browse, 1, 0);
        _toolbar.Controls.Add(right, 2, 0);
        return _toolbar;
    }

    /// <summary>
    /// The palette is the tileset, drawn from the file. Nothing in this app
    /// knows what a tile is called or what colour it is, so a character added
    /// to tileset.txt appears here on the next Reload with no code change,
    /// which is the same rule the game's own build follows.
    /// </summary>
    private void BuildPalette()
    {
        _palette.Dock = DockStyle.Fill;
        _palette.IntegralHeight = false;
        _palette.DrawMode = DrawMode.OwnerDrawFixed;
        _palette.ItemHeight = 24;
        _palette.DrawItem += (_, e) =>
        {
            if (e.Index < 0 || _palette.Items[e.Index] is not TileDef tile) return;
            e.DrawBackground();
            using (var swatch = new SolidBrush(tile.Colour))
                e.Graphics.FillRectangle(swatch, e.Bounds.Left + 5, e.Bounds.Top + 4, 16, 16);
            e.Graphics.DrawRectangle(Pens.Black, e.Bounds.Left + 5, e.Bounds.Top + 4, 16, 16);
            var font = e.Font ?? Font;
            TextRenderer.DrawText(e.Graphics, $"{tile.Ch}  {tile.Name}", font,
                new Point(e.Bounds.Left + 28, e.Bounds.Top + 5), e.ForeColor);
            using var small = new Font(font.FontFamily, 7F);
            TextRenderer.DrawText(e.Graphics, tile.FlagText, small,
                new Rectangle(e.Bounds.Left, e.Bounds.Top + 6, e.Bounds.Width - 6, e.Bounds.Height),
                SystemColors.GrayText, TextFormatFlags.Right);
            e.DrawFocusRectangle();
        };
        _palette.SelectedIndexChanged += (_, _) =>
        {
            if (_palette.SelectedItem is TileDef tile) _map.BrushChar = tile.Ch;
        };
    }

    private void BuildMap()
    {
        _viewport.Dock = DockStyle.Fill;
        _viewport.AutoScroll = true;
        _viewport.BackColor = Color.FromArgb(24, 24, 28);
        _viewport.Controls.Add(_map);

        _map.Edited += OnEdited;
        _map.SelectionChanged += thing =>
        {
            SelectThing(thing);
            _tabs.SelectedTab = _thingsPage;
        };
        _map.Sampled += ch =>
        {
            for (var i = 0; i < _palette.Items.Count; i++)
            {
                if (_palette.Items[i] is TileDef tile && tile.Ch == ch) { _palette.SelectedIndex = i; return; }
            }
        };
        _map.Hovered += text => _status.Text = text.Length > 0 ? text : Summary();
    }

    private void BuildSidePanel()
    {
        _things.Dock = DockStyle.Fill;
        _things.IntegralHeight = false;
        _things.SelectedIndexChanged += (_, _) =>
        {
            if (_syncing) return;
            var index = _things.SelectedIndex;
            SelectThing(index >= 0 && index < _thingList.Count ? _thingList[index] : null);
        };

        _addNpc.Text = "NPC";
        _addWarp.Text = "Warp";
        _addEvent.Text = "Event";
        _delete.Text = "Delete";
        _addNpc.Click += (_, _) => AddNpc();
        _addWarp.Click += (_, _) => AddWarp();
        _addEvent.Click += (_, _) => AddEvent();
        _delete.Click += (_, _) => DeleteSelected();
        foreach (var button in new[] { _addNpc, _addWarp, _addEvent, _delete })
        {
            button.AutoSize = true;
            button.Margin = new Padding(0, 0, 6, 0);
        }

        var buttons = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill, AutoSize = true, WrapContents = false, Margin = new Padding(0, 4, 0, 4),
        };
        buttons.Controls.Add(new Label { Text = "add", AutoSize = true, Margin = new Padding(0, 7, 6, 0) });
        buttons.Controls.Add(_addNpc);
        buttons.Controls.Add(_addWarp);
        buttons.Controls.Add(_addEvent);
        buttons.Controls.Add(_delete);

        var page = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, Padding = new Padding(6) };
        page.RowStyles.Add(new RowStyle(SizeType.Absolute, 170));
        page.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        page.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        page.Controls.Add(_things, 0, 0);
        page.Controls.Add(buttons, 0, 1);
        page.Controls.Add(_inspector, 0, 2);
        _inspector.Dock = DockStyle.Fill;
        _inspector.Edited += OnEdited;
        _inspector.Goto += (zoneId, x, y) =>
        {
            if (_data is null || !_data.ZoneById.ContainsKey(zoneId)) return;
            OpenZone(zoneId);
            _map.EnsureVisible(x, y);
        };
        _thingsPage.Controls.Add(page);

        _encounters.Edited += OnEdited;
        _encountersPage.Controls.Add(_encounters);

        _tabs.Dock = DockStyle.Fill;
        _tabs.TabPages.Add(_thingsPage);
        _tabs.TabPages.Add(_encountersPage);
    }

    private void BuildProblems()
    {
        _problems.Dock = DockStyle.Fill;
        _problems.View = View.Details;
        _problems.FullRowSelect = true;
        _problems.MultiSelect = false;
        _problems.HideSelection = false;
        _problems.Columns.Add("", 60);
        _problems.Columns.Add("where", 150);
        _problems.Columns.Add("problem", 900);
        _problems.ItemActivate += (_, _) => JumpToProblem();
        _problems.Click += (_, _) => JumpToProblem();

        _status.Dock = DockStyle.Fill;
        _status.AutoSize = true;
        _status.Padding = new Padding(4, 4, 4, 4);
    }

    // --------------------------------------------------------------- opening

    private void BrowseForData()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Pick games/picomon/data",
            SelectedPath = _dataPath.Text,
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        if (!Dataset.LooksLikeDataDirectory(dialog.SelectedPath))
        {
            MessageBox.Show(this, "That folder has no tileset.txt and no zones directory.",
                            "Not a data directory", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        OpenData(dialog.SelectedPath, "");
    }

    private void ReloadData()
    {
        if (_data is null) return;
        if (!ConfirmDiscard("Reload")) return;
        OpenData(_data.Root, _zone?.Id ?? "");
    }

    private void OpenData(string root, string zoneId)
    {
        // A file that will not parse is collected as a problem rather than
        // thrown, so this only fires for a directory that cannot be read at
        // all: a dead network path, a folder someone has open elsewhere.
        try
        {
            _data = Dataset.Load(root);
        }
        catch (IOException e)
        {
            MessageBox.Show(this, e.Message, "Could not read the data directory",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        catch (UnauthorizedAccessException e)
        {
            MessageBox.Show(this, e.Message, "Could not read the data directory",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        _dataPath.Text = root;
        _settings.DataDirectory = root;
        _settings.Save();

        _palette.Items.Clear();
        foreach (var tile in _data.Tiles) _palette.Items.Add(tile);
        if (_palette.Items.Count > 0) _palette.SelectedIndex = 0;

        _syncing = true;
        _zones.Items.Clear();
        foreach (var zone in _data.Zones) _zones.Items.Add(zone.Id);
        _syncing = false;

        _map.Data = _data;
        _map.Zoom = (int)_zoom.Value;

        var wanted = _data.Zones.FirstOrDefault(z => z.Id == zoneId) ?? _data.Zones.FirstOrDefault();
        if (wanted is null)
        {
            _zone = null;
            _map.Zone = null;
            _inspector.Bind(_data, null);
            _encounters.Bind(_data, null);
        }
        else
        {
            OpenZone(wanted.Id);
        }
        Recheck();
    }

    private void OpenZone(string id)
    {
        if (_data is null || !_data.ZoneById.TryGetValue(id, out var zone)) return;
        _zone = zone;
        _map.Zone = zone;
        _inspector.Bind(_data, zone);
        _encounters.Bind(_data, zone);
        RefreshThings();
        SelectThing(null);

        _syncing = true;
        _zones.SelectedItem = id;
        _syncing = false;

        _settings.LastZone = id;
        _settings.Save();
        UpdateTitle();
        Recheck();
    }

    // --------------------------------------------------------------- editing

    private void OnEdited()
    {
        RefreshThings();
        _map.Invalidate();
        UpdateTitle();
        Recheck();
    }

    private void RefreshThings()
    {
        var chosen = _map.Selected;
        _syncing = true;
        _things.Items.Clear();
        _thingList.Clear();
        foreach (var thing in _zone?.Placed ?? Enumerable.Empty<IPlaced>())
        {
            _thingList.Add(thing);
            _things.Items.Add(thing.Label);
        }
        var index = chosen is null ? -1 : _thingList.IndexOf(chosen);
        if (index >= 0) _things.SelectedIndex = index;
        _syncing = false;
    }

    /// <summary>Selects a placed thing everywhere at once: on the map, in the
    /// list, and in the panel of fields.</summary>
    private void SelectThing(object? thing)
    {
        _map.Selected = thing;
        _inspector.Display(thing);
        _syncing = true;
        var index = thing is null ? -1 : _thingList.IndexOf(thing);
        _things.SelectedIndex = index;
        _syncing = false;
        _status.Text = Summary();
    }

    private Point NewPlace()
    {
        var at = _map.LastClick;
        return _zone is not null && _zone.Contains(at.X, at.Y) ? at : new Point(0, 0);
    }

    private void AddNpc()
    {
        if (_zone is null) return;
        var at = NewPlace();
        var npc = new NpcDef
        {
            Id = UniqueId("npc"),
            X = at.X,
            Y = at.Y,
            Facing = "south",
            Sheet = _data?.Sheets.FirstOrDefault() ?? "villager",
        };
        // An NPC with nothing to say fails the build, so a new one starts with a
        // page rather than with an error.
        npc.Say.Add("...");
        _zone.Npcs.Add(npc);
        Touch(npc);
    }

    private void AddWarp()
    {
        if (_zone is null || _data is null) return;
        var at = NewPlace();
        var dest = _data.Zones.FirstOrDefault(z => z.Id != _zone.Id) ?? _zone;
        var landing = FirstWalkable(dest);
        var warp = new WarpDef
        {
            X = at.X, Y = at.Y, Dest = dest.Id, DestX = landing.X, DestY = landing.Y,
        };
        _zone.Warps.Add(warp);
        Touch(warp);
    }

    private void AddEvent()
    {
        if (_zone is null) return;
        var at = NewPlace();
        var ev = new EventDef { Kind = "sign", X = at.X, Y = at.Y };
        ev.Say.Add("...");
        _zone.Events.Add(ev);
        Touch(ev);
    }

    /// <summary>Where a new warp lands: the first tile of the destination the
    /// player could actually stand on. Landing on a tree is a failed build, and
    /// 0,0 is a tree in every zone in the game.</summary>
    private Point FirstWalkable(ZoneFile zone)
    {
        if (_data is not null)
        {
            for (var y = 0; y < zone.Height; y++)
            {
                for (var x = 0; x < zone.Rows[y].Length; x++)
                {
                    if (_data.TileByChar.TryGetValue(zone.Rows[y][x], out var tile) && tile.Walk)
                        return new Point(x, y);
                }
            }
        }
        return new Point(0, 0);
    }

    private string UniqueId(string stem)
    {
        var taken = _zone?.Npcs.Select(n => n.Id).ToHashSet() ?? new HashSet<string>();
        var n = 1;
        while (taken.Contains(stem + ZoneFile.N(n))) n++;
        return stem + ZoneFile.N(n);
    }

    private void Touch(object thing)
    {
        if (_zone is not null) _zone.Dirty = true;
        RefreshThings();
        SelectThing(thing);
        _map.Invalidate();
        UpdateTitle();
        Recheck();
    }

    private void DeleteSelected()
    {
        if (_zone is null || _map.Selected is null) return;
        switch (_map.Selected)
        {
            case NpcDef npc: _zone.Npcs.Remove(npc); break;
            case WarpDef warp: _zone.Warps.Remove(warp); break;
            case EventDef ev: _zone.Events.Remove(ev); break;
            default: return;
        }
        _zone.Dirty = true;
        RefreshThings();
        SelectThing(null);
        _map.Invalidate();
        UpdateTitle();
        Recheck();
    }

    private void OnKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Control && e.KeyCode == Keys.S)
        {
            SaveCurrent();
            e.Handled = true;
            return;
        }
        // Delete belongs to the map and the list of things. Anywhere else the
        // caret is in a field and Delete means delete a character.
        if (!_map.Focused && !_things.Focused) return;
        if (e.KeyCode == Keys.Delete) DeleteSelected();
        else if (e.KeyCode == Keys.Escape) SelectThing(null);
    }

    // ------------------------------------------------------------- reporting

    /// <summary>Runs the checks over the whole directory and rebuilds the
    /// problem list. Not called Validate: a Form already has one, inherited from
    /// ContainerControl, that runs the focused control's validation events, and
    /// a method of that name here would quietly hide it.</summary>
    private void Recheck()
    {
        if (_data is null) return;
        _found = Validator.Check(_data);

        _problems.BeginUpdate();
        _problems.Items.Clear();
        // The open zone first: everything else is someone else's problem right
        // now, and it is still listed because a warp into this zone can be
        // broken from the other side.
        foreach (var problem in _found.OrderBy(p => p.ZoneId == _zone?.Id ? 0 : 1)
                                      .ThenBy(p => p.Severity == Severity.Error ? 0 : 1))
        {
            var row = new ListViewItem(problem.Severity == Severity.Error ? "error" : "warning")
            {
                Tag = problem,
                ForeColor = problem.Severity == Severity.Error
                    ? Color.FromArgb(160, 20, 20)
                    : Color.FromArgb(150, 100, 0),
            };
            row.SubItems.Add(problem.Where);
            row.SubItems.Add(problem.Text);
            _problems.Items.Add(row);
        }
        _problems.EndUpdate();
        _status.Text = Summary();
    }

    private string Summary()
    {
        if (_data is null) return "No data directory open.";
        var errors = _found.Count(p => p.Severity == Severity.Error);
        var warnings = _found.Count - errors;
        var mine = _found.Count(p => p.Severity == Severity.Error && p.ZoneId == _zone?.Id);
        var text = errors == 0 && warnings == 0
            ? "Everything checks out."
            : $"{ZoneFile.N(errors)} error(s), {ZoneFile.N(warnings)} warning(s)"
              + (mine > 0 ? $", {ZoneFile.N(mine)} of them in this zone" : "");
        return text + (_zone is not null && _zone.Dirty ? "   unsaved changes" : "");
    }

    private void JumpToProblem()
    {
        if (_problems.SelectedItems.Count == 0) return;
        if (_problems.SelectedItems[0].Tag is not Problem problem) return;
        if (_data is not null && problem.ZoneId != _zone?.Id && _data.ZoneById.ContainsKey(problem.ZoneId))
            OpenZone(problem.ZoneId);

        switch (problem.Thing)
        {
            case EncounterTable table:
                _tabs.SelectedTab = _encountersPage;
                _encounters.Select(table);
                break;
            case not null:
                _tabs.SelectedTab = _thingsPage;
                SelectThing(problem.Thing);
                break;
        }
        if (problem.HasPlace) _map.EnsureVisible(problem.X, problem.Y);
    }

    private void UpdateTitle()
    {
        var name = _zone is null ? "" : " - " + Path.GetFileName(_zone.Path);
        Text = "PicomonEditor" + name + (_zone?.Dirty == true ? "*" : "");
    }

    // ---------------------------------------------------------------- saving

    private void SaveCurrent()
    {
        if (_zone is null) return;
        Recheck();
        var blocking = _found
            .Where(p => p.Severity == Severity.Error && p.ZoneId == _zone.Id)
            .ToList();
        if (blocking.Count > 0)
        {
            // Only this zone's errors stop this zone being saved. A broken warp
            // in another file is real and listed, but refusing to save the map
            // in front of you because of it would make the tool unusable in
            // exactly the situation it exists for.
            MessageBox.Show(this,
                "The build would reject this zone:\n\n"
                + string.Join("\n", blocking.Take(6).Select(p => "  " + p.Where + "  " + p.Text))
                + (blocking.Count > 6 ? $"\n  ...and {ZoneFile.N(blocking.Count - 6)} more" : ""),
                "Not saved", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            if (_problems.Items.Count > 0)
            {
                _problems.Items[0].Selected = true;
                _problems.Select();
            }
            return;
        }

        try
        {
            _zone.Save();
        }
        catch (IOException e)
        {
            MessageBox.Show(this, e.Message, "Could not write the file",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        catch (UnauthorizedAccessException e)
        {
            MessageBox.Show(this, e.Message, "Could not write the file",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        UpdateTitle();
        _status.Text = $"Saved {Path.GetFileName(_zone.Path)}.   " + Summary();
    }

    private bool ConfirmDiscard(string action)
    {
        var dirty = _data?.Zones.Count(z => z.Dirty) ?? 0;
        if (dirty == 0) return true;
        return MessageBox.Show(this,
            $"{ZoneFile.N(dirty)} zone(s) have unsaved changes. {action} anyway?",
            "Unsaved changes", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) == DialogResult.OK;
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        _settings.Save();
        var dirty = _data?.Zones.Where(z => z.Dirty).ToList() ?? new List<ZoneFile>();
        if (dirty.Count == 0) return;

        var answer = MessageBox.Show(this,
            $"Save changes to {ZoneFile.N(dirty.Count)} zone(s)?",
            "Unsaved changes", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
        if (answer == DialogResult.Cancel) { e.Cancel = true; return; }
        if (answer == DialogResult.No) return;

        // Checked again here rather than trusted: what is on screen was checked
        // when it was last edited, and this is the last chance to not write a
        // file the build would reject.
        Recheck();
        var errors = _found.Where(p => p.Severity == Severity.Error).Select(p => p.ZoneId).ToHashSet();
        var refused = dirty.Where(z => errors.Contains(z.Id)).ToList();
        foreach (var zone in dirty.Except(refused))
        {
            try { zone.Save(); }
            catch (IOException) { refused.Add(zone); }
            catch (UnauthorizedAccessException) { refused.Add(zone); }
        }
        if (refused.Count > 0)
        {
            MessageBox.Show(this,
                "These zones were not saved, because the build would reject them:\n\n  "
                + string.Join("\n  ", refused.Select(z => Path.GetFileName(z.Path))),
                "Still open", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            e.Cancel = true;
        }
    }
}
