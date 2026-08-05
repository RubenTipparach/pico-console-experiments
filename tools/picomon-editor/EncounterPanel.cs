namespace PicomonEditor;

/// <summary>
/// The encounter tables: one per encountering tile character, each a rate and a
/// list of species with a level range and a weight.
///
/// Weights are relative and need not sum to anything, so each line shows what
/// its weight actually works out as. A table where one line reads 40 and
/// another reads 400 is perfectly legal, and the percentage is the only way to
/// see that the first one may as well not be there.
///
/// Both lists hold text and are read back by index, not by the object they
/// display. It is a line of code either way, and this way the list and the
/// model cannot drift apart over what an item is called.
/// </summary>
public sealed class EncounterPanel : UserControl
{
    private readonly ListBox _tables = new();
    private readonly ListBox _slots = new();
    private readonly ComboBox _tile = new();
    private readonly NumericUpDown _rate = new();
    private readonly ComboBox _species = new();
    private readonly NumericUpDown _min = new();
    private readonly NumericUpDown _max = new();
    private readonly NumericUpDown _weight = new();
    private readonly Button _addTable = new();
    private readonly Button _removeTable = new();
    private readonly Button _addSlot = new();
    private readonly Button _removeSlot = new();

    private Dataset? _data;
    private ZoneFile? _zone;
    private bool _loading;

    public event Action? Edited;

    public EncounterPanel()
    {
        Dock = DockStyle.Fill;
        Padding = new Padding(8);

        _tables.Dock = DockStyle.Fill;
        _tables.IntegralHeight = false;
        _tables.SelectedIndexChanged += (_, _) => ShowTable();

        _slots.Dock = DockStyle.Fill;
        _slots.IntegralHeight = false;
        _slots.SelectedIndexChanged += (_, _) => ShowSlot();

        _tile.DropDownStyle = ComboBoxStyle.DropDownList;
        _tile.Width = 150;
        _tile.SelectedIndexChanged += (_, _) =>
        {
            if (_loading || Table is not { } table || _tile.SelectedItem is not TileDef tile) return;
            table.Tile = tile.Ch;
            Touch();
            RefreshTables();
        };

        _rate.Minimum = 1;
        _rate.Maximum = 100;
        _rate.Width = 60;
        _rate.ValueChanged += (_, _) =>
        {
            if (_loading || Table is not { } table) return;
            table.Rate = (int)_rate.Value;
            Touch();
            RefreshTables();
        };

        _species.DropDownStyle = ComboBoxStyle.DropDownList;
        _species.Width = 150;
        _species.SelectedIndexChanged += (_, _) => WriteSlot();

        foreach (var box in new[] { _min, _max, _weight })
        {
            box.Minimum = 1;
            box.Maximum = 100;
            box.Width = 62;
            box.ValueChanged += (_, _) => WriteSlot();
        }
        // A weight is neither a level nor a percentage: it only means anything
        // next to the other weights in its own table.
        _weight.Maximum = 1000;

        _addTable.Text = "Add table";
        _addTable.Click += (_, _) => AddTable();
        _removeTable.Text = "Remove";
        _removeTable.Click += (_, _) => RemoveTable();
        _addSlot.Text = "Add species";
        _addSlot.Click += (_, _) => AddSlot();
        _removeSlot.Text = "Remove";
        _removeSlot.Click += (_, _) => RemoveSlot();
        foreach (var button in new[] { _addTable, _removeTable, _addSlot, _removeSlot })
        {
            button.AutoSize = true;
            button.Margin = new Padding(0, 0, 6, 0);
        }

        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 7 };
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 35));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 65));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        layout.Controls.Add(Strip(Caption("Tables", 0), _addTable, _removeTable), 0, 0);
        layout.Controls.Add(_tables, 0, 1);
        layout.Controls.Add(Strip(Caption("tile", 0), _tile, Caption("rate %", 12), _rate), 0, 2);
        layout.Controls.Add(Strip(Caption("Species in this table", 0)), 0, 3);
        layout.Controls.Add(_slots, 0, 4);
        layout.Controls.Add(Strip(_species, Caption("levels", 10), _min, _max, Caption("weight", 10), _weight), 0, 5);
        layout.Controls.Add(Strip(_addSlot, _removeSlot), 0, 6);
        Controls.Add(layout);
    }

    private static Label Caption(string text, int left) =>
        new() { Text = text, AutoSize = true, Margin = new Padding(left, 7, 6, 0) };

    private static Control Strip(params Control[] controls)
    {
        var flow = new FlowLayoutPanel
        {
            AutoSize = true, WrapContents = false, Dock = DockStyle.Fill, Margin = new Padding(0, 2, 0, 2),
        };
        foreach (var control in controls) flow.Controls.Add(control);
        return flow;
    }

    private EncounterTable? Table
    {
        get
        {
            var tables = _zone?.Encounters;
            var index = _tables.SelectedIndex;
            return tables is not null && index >= 0 && index < tables.Count ? tables[index] : null;
        }
    }

    private EncounterRow? Slot
    {
        get
        {
            var slots = Table?.Slots;
            var index = _slots.SelectedIndex;
            return slots is not null && index >= 0 && index < slots.Count ? slots[index] : null;
        }
    }

    public void Bind(Dataset? data, ZoneFile? zone)
    {
        _data = data;
        _zone = zone;

        _loading = true;
        _tile.Items.Clear();
        // Only tiles that roll an encounter: a table on any other character is
        // one the game would never look at.
        foreach (var tile in data?.Tiles.Where(t => t.Encounter) ?? Enumerable.Empty<TileDef>())
            _tile.Items.Add(tile);

        _species.Items.Clear();
        foreach (var one in data?.Species ?? new List<NamedDef>()) _species.Items.Add(one.Id);
        _loading = false;

        RefreshTables();
    }

    /// <summary>Used by the problem list to open the table it is complaining
    /// about.</summary>
    public void Select(EncounterTable table)
    {
        var index = _zone?.Encounters.IndexOf(table) ?? -1;
        if (index >= 0) _tables.SelectedIndex = index;
    }

    private void RefreshTables()
    {
        var chosen = _tables.SelectedIndex;
        _loading = true;
        _tables.Items.Clear();
        foreach (var table in _zone?.Encounters ?? new List<EncounterTable>())
        {
            _tables.Items.Add($"'{table.Tile}'   rate {ZoneFile.N(table.Rate)}%   "
                              + $"{ZoneFile.N(table.Slots.Count)} species");
        }
        _loading = false;
        if (_tables.Items.Count == 0) ShowTable();
        else _tables.SelectedIndex = Math.Clamp(chosen, 0, _tables.Items.Count - 1);
    }

    private void ShowTable()
    {
        if (_loading) return;
        _loading = true;
        var table = Table;
        var live = table is not null;
        _tile.Enabled = _rate.Enabled = _addSlot.Enabled = _removeTable.Enabled = live;
        if (table is not null)
        {
            _rate.Value = Math.Clamp(table.Rate, 1, 100);
            var tile = _data?.Tiles.FirstOrDefault(t => t.Ch == table.Tile);
            // A table on a character the tileset does not roll on is still
            // shown, because hiding it is how it stays broken.
            if (tile is not null && !_tile.Items.Contains(tile)) _tile.Items.Add(tile);
            _tile.SelectedItem = tile;
        }
        FillSlots();
        _loading = false;
        if (_slots.Items.Count > 0) _slots.SelectedIndex = 0; else ShowSlot();
    }

    private void FillSlots()
    {
        var table = Table;
        var total = table?.Slots.Sum(s => Math.Max(0, s.Weight)) ?? 0;
        _slots.Items.Clear();
        foreach (var slot in table?.Slots ?? new List<EncounterRow>())
        {
            var share = total > 0 ? slot.Weight * 100 / total : 0;
            _slots.Items.Add($"{slot.Species}   lv {ZoneFile.N(slot.MinLevel)}-{ZoneFile.N(slot.MaxLevel)}   "
                             + $"weight {ZoneFile.N(slot.Weight)}   ({ZoneFile.N(share)}%)");
        }
    }

    private void ShowSlot()
    {
        if (_loading) return;
        _loading = true;
        var slot = Slot;
        var live = slot is not null;
        _species.Enabled = _min.Enabled = _max.Enabled = _weight.Enabled = _removeSlot.Enabled = live;
        if (slot is not null)
        {
            if (!_species.Items.Contains(slot.Species)) _species.Items.Add(slot.Species);
            _species.SelectedItem = slot.Species;
            _min.Value = Math.Clamp(slot.MinLevel, 1, 100);
            _max.Value = Math.Clamp(slot.MaxLevel, 1, 100);
            _weight.Value = Math.Clamp(slot.Weight, 1, 1000);
        }
        _loading = false;
    }

    private void WriteSlot()
    {
        if (_loading || Slot is not { } slot) return;
        slot.Species = _species.SelectedItem as string ?? slot.Species;
        slot.MinLevel = (int)_min.Value;
        slot.MaxLevel = (int)_max.Value;
        slot.Weight = (int)_weight.Value;
        Touch();

        var chosen = _slots.SelectedIndex;
        _loading = true;
        FillSlots();
        if (chosen >= 0 && chosen < _slots.Items.Count) _slots.SelectedIndex = chosen;
        _loading = false;
    }

    private void AddTable()
    {
        if (_zone is null) return;
        // The first encountering tile that has no table yet, because the game
        // only ever rolls the first table for a character: starting on one that
        // is already spoken for would add a block that can never fire. Falls
        // back to the first when they all have one, and the problem list says
        // what that means.
        var taken = _zone.Encounters.Select(t => t.Tile).ToHashSet();
        var encountering = _data?.Tiles.Where(t => t.Encounter).ToList() ?? new List<TileDef>();
        var tile = encountering.FirstOrDefault(t => !taken.Contains(t.Ch)) ?? encountering.FirstOrDefault();
        if (tile is null)
        {
            MessageBox.Show(this, "No tile in tileset.txt has the encounter flag, so there is nothing to roll on.",
                            "Nothing to roll on", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        var table = new EncounterTable { Tile = tile.Ch, Rate = 10 };
        table.Slots.Add(NewSlot());
        _zone.Encounters.Add(table);
        Touch();
        RefreshTables();
        Select(table);
    }

    private void RemoveTable()
    {
        if (_zone is null || Table is not { } table) return;
        _zone.Encounters.Remove(table);
        Touch();
        RefreshTables();
    }

    private void AddSlot()
    {
        if (Table is not { } table) return;
        table.Slots.Add(NewSlot());
        Touch();
        RefreshTables();
        _loading = true;
        FillSlots();
        _loading = false;
        _slots.SelectedIndex = _slots.Items.Count - 1;
    }

    private void RemoveSlot()
    {
        if (Table is not { } table || Slot is not { } slot) return;
        table.Slots.Remove(slot);
        Touch();
        RefreshTables();
        ShowTable();
    }

    private EncounterRow NewSlot() => new()
    {
        Species = _data?.Species.FirstOrDefault()?.Id ?? "",
        MinLevel = 2,
        MaxLevel = 4,
        Weight = 10,
    };

    private void Touch()
    {
        if (_zone is not null) _zone.Dirty = true;
        Edited?.Invoke();
    }
}
