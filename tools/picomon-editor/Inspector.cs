namespace PicomonEditor;

/// <summary>
/// The fields for whatever is selected, built fresh for its kind: an NPC has a
/// party and four kinds of text, a warp has a destination, an event has the
/// arguments its own kind takes and no others.
///
/// The dropdowns are filled from the data directory rather than from a list in
/// here, so a species added to species.txt or a sheet used by one NPC turns up
/// without this file changing. Where the compiler accepts free text (a sheet
/// name, a flag) the box is editable and the list is only a shortcut; where it
/// accepts one of a fixed set (a facing, an event kind) the box is closed,
/// because the point of this app is that a typo cannot leave it.
/// </summary>
public sealed class Inspector : Panel
{
    private readonly TableLayoutPanel _rows = new();
    private Dataset? _data;
    private ZoneFile? _zone;
    private bool _loading;

    /// <summary>Raised when a field is edited, so the map repaints and the
    /// checks run again.</summary>
    public event Action? Edited;

    /// <summary>Asks the form to open another zone at a tile, which is how the
    /// destination of a warp gets looked at.</summary>
    public event Action<string, int, int>? Goto;

    public Inspector()
    {
        AutoScroll = true;
        Padding = new Padding(8);
        _rows.Dock = DockStyle.Top;
        _rows.AutoSize = true;
        _rows.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        _rows.ColumnCount = 2;
        _rows.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _rows.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        Controls.Add(_rows);
    }

    public void Bind(Dataset? data, ZoneFile? zone)
    {
        _data = data;
        _zone = zone;
        Display(null);
    }

    /// <summary>Rebuilds the panel after the event that asked for it has
    /// finished. Changing the kind of a thing changes which fields it has, and
    /// the control that reported the change is one of the controls being thrown
    /// away: disposing it inside its own handler is how a tool crashes on the
    /// one action that was working.</summary>
    public void Rebuild(object? thing) => BeginInvoke(() => Display(thing));

    public void Display(object? thing)
    {
        _loading = true;
        _rows.SuspendLayout();
        foreach (var control in _rows.Controls.Cast<Control>().ToArray()) control.Dispose();
        _rows.Controls.Clear();
        _rows.RowStyles.Clear();
        _rows.RowCount = 0;

        switch (thing)
        {
            case NpcDef npc: BuildNpc(npc); break;
            case WarpDef warp: BuildWarp(warp); break;
            case EventDef ev: BuildEvent(ev); break;
            default: BuildZone(); break;
        }

        _rows.ResumeLayout();
        _loading = false;
    }

    private void Changed()
    {
        if (_loading) return;
        if (_zone is not null) _zone.Dirty = true;
        Edited?.Invoke();
    }

    // ---------------------------------------------------------------- layouts

    private void BuildZone()
    {
        if (_zone is null)
        {
            Row("", Note("Open a data directory to start."));
            return;
        }
        Row("zone", Note(_zone.Id));
        Row("name", TextField(_zone.Name, v => { _zone.Name = v; Changed(); }));
        Row("size", Note($"{ZoneFile.N(_zone.Width)} by {ZoneFile.N(_zone.Height)}"));
        Row("file", Note(Path.GetFileName(_zone.Path)));
        Row("", Note("Click a marker to edit it, or paint with the palette."));
    }

    private void BuildNpc(NpcDef npc)
    {
        Row("id", TextField(npc.Id, v => { npc.Id = v.Trim(); Changed(); }));
        Row("at", Place(npc));
        Row("facing", Choice(Validator.Facings, npc.Facing, v => { npc.Facing = v; Changed(); }));
        Row("sheet", Free(_data?.Sheets ?? Enumerable.Empty<string>(), npc.Sheet,
                          v => { npc.Sheet = v.Trim(); Changed(); }));
        Row("kind", Choice(Validator.NpcKinds, npc.EffectiveKind, v =>
        {
            npc.SetKind(v);
            Changed();
            // Kind decides which of the fields below mean anything, so the panel
            // is rebuilt rather than left showing a party box on a shopkeeper.
            Rebuild(npc);
        }));

        var sight = new NumericUpDown { Minimum = 0, Maximum = Validator.MaxSight, Width = 60 };
        // Clamped rather than trusted, the same way every other number in this
        // panel is. The compiler puts no bounds on a sight key at all, and a
        // NumericUpDown throws when it is handed a value outside its own range,
        // so a hand written 'sight 25' would take the editor down on the click
        // that selected the NPC. The model keeps what the file said until the
        // box is touched, and the problem list is what reports it.
        sight.Value = Math.Clamp(npc.Sight ?? 0, 0, Validator.MaxSight);
        sight.Enabled = npc.Sight.HasValue;
        var hasSight = new CheckBox { Text = "line of sight", AutoSize = true, Checked = npc.Sight.HasValue };
        hasSight.CheckedChanged += (_, _) =>
        {
            sight.Enabled = hasSight.Checked;
            // A sight key is what makes an NPC a trainer, whatever its value, so
            // ticking the box is a real change even at zero.
            npc.Sight = hasSight.Checked ? (int)sight.Value : null;
            Changed();
        };
        sight.ValueChanged += (_, _) =>
        {
            if (hasSight.Checked) npc.Sight = (int)sight.Value;
            Changed();
        };
        Row("sight", Beside(hasSight, sight));

        Row("party", Party(npc));
        Row("say", Pages(npc.Say));
        Row("win", Pages(npc.Win));
        Row("lose", Pages(npc.Lose));
        Row("reward", Number(npc.Reward, 0, Validator.MaxReward, v => { npc.Reward = v; Changed(); }));
        Row("flag", Free(_data?.SetFlags ?? Enumerable.Empty<string>(), npc.Flag,
                         v => { npc.Flag = v.Trim(); Changed(); }));
        Row("only when", Condition(npc));
    }

    private void BuildWarp(WarpDef warp)
    {
        Row("at", Place(warp));
        Row("to zone", Choice(_data?.ZoneById.Keys.OrderBy(k => k, StringComparer.Ordinal)
                              ?? Enumerable.Empty<string>(),
                              warp.Dest, v => { warp.Dest = v; Changed(); }));

        var x = new NumericUpDown { Minimum = 0, Maximum = 254, Width = 60, Value = Clamp(warp.DestX) };
        var y = new NumericUpDown { Minimum = 0, Maximum = 254, Width = 60, Value = Clamp(warp.DestY) };
        x.ValueChanged += (_, _) => { warp.DestX = (int)x.Value; Changed(); };
        y.ValueChanged += (_, _) => { warp.DestY = (int)y.Value; Changed(); };
        Row("lands at", Beside(x, y));

        Row("facing", Choice(Validator.Facings, warp.EffectiveFacing, v => { warp.SetFacing(v); Changed(); }));

        var jump = new Button { Text = "Open the other side", AutoSize = true };
        jump.Click += (_, _) => Goto?.Invoke(warp.Dest, warp.DestX, warp.DestY);
        Row("", jump);
    }

    private void BuildEvent(EventDef ev)
    {
        Row("kind", Choice(Validator.EventKinds, ev.Kind, v =>
        {
            ev.Kind = v;
            // The extra header arguments belong to the kind, so they go with it:
            // a sign carrying an item and a count is a file the compiler
            // rejects, and the editor is not allowed to write one.
            if (v == "item")
            {
                while (ev.Args.Count < 2) ev.Args.Add(ev.Args.Count == 0 ? FirstItem() : "1");
                if (ev.Args.Count > 2) ev.Args = ev.Args.Take(2).ToList();
            }
            else
            {
                ev.Args = new List<string>();
            }
            Changed();
            Rebuild(ev);
        }));
        Row("at", Place(ev));

        if (ev.Kind == "item")
        {
            Row("item", Free(_data?.Items.Select(i => i.Id) ?? Enumerable.Empty<string>(), ev.Item,
                             v => { ev.Item = v.Trim(); Changed(); }));
            Row("count", TextField(ev.Count, v => { ev.Count = v.Trim(); Changed(); }));
        }

        Row("flag", Free(_data?.SetFlags ?? Enumerable.Empty<string>(), ev.Flag,
                         v => { ev.Flag = v.Trim(); Changed(); }));
        Row("say", Pages(ev.Say));
    }

    private string FirstItem() => _data?.Items.FirstOrDefault()?.Id ?? "potion";

    // ---------------------------------------------------------------- pieces

    private Control Place(IPlaced thing)
    {
        var x = new NumericUpDown { Minimum = 0, Maximum = 254, Width = 60, Value = Clamp(thing.X) };
        var y = new NumericUpDown { Minimum = 0, Maximum = 254, Width = 60, Value = Clamp(thing.Y) };
        x.ValueChanged += (_, _) => { thing.X = (int)x.Value; Changed(); };
        y.ValueChanged += (_, _) => { thing.Y = (int)y.Value; Changed(); };
        return Beside(x, y);
    }

    /// <summary>The party in the syntax the file uses, with the species list
    /// beside it so a name never has to be remembered. Typing straight into the
    /// line is quicker than a grid for six entries, and a name that is not a
    /// species turns up in the problem list either way.</summary>
    private Control Party(NpcDef npc)
    {
        var line = new TextBox { Text = PartyText(npc), Dock = DockStyle.Fill };
        line.TextChanged += (_, _) =>
        {
            npc.Party.Clear();
            foreach (var entry in line.Text.Split(','))
            {
                var words = ZoneFile.Words(entry);
                if (words.Length == 0) continue;
                npc.Party.Add(new PartyEntry
                {
                    Species = words[0],
                    Level = words.Length > 1 && int.TryParse(words[1], out var level) ? level : 1,
                });
            }
            Changed();
        };

        var species = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 130 };
        foreach (var one in _data?.Species ?? new List<NamedDef>()) species.Items.Add(one.Id);
        if (species.Items.Count > 0) species.SelectedIndex = 0;

        var add = new Button { Text = "Add", AutoSize = true };
        add.Click += (_, _) =>
        {
            if (species.SelectedItem is not string id) return;
            line.Text = line.Text.Trim().Length == 0 ? $"{id} 5" : line.Text.TrimEnd() + $", {id} 5";
        };

        var stack = new TableLayoutPanel
        {
            ColumnCount = 2, RowCount = 2, AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Fill, Margin = new Padding(0),
        };
        stack.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        stack.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        stack.Controls.Add(line, 0, 0);
        stack.SetColumnSpan(line, 2);
        stack.Controls.Add(species, 0, 1);
        stack.Controls.Add(add, 1, 1);
        return stack;
    }

    private static string PartyText(NpcDef npc) =>
        string.Join(", ", npc.Party.Select(p => $"{p.Species} {ZoneFile.N(p.Level)}"));

    /// <summary>One page of dialogue per line. The device shows them one after
    /// another, and each one has to fit the panel on its own.</summary>
    private Control Pages(List<string> pages)
    {
        var box = new TextBox
        {
            Multiline = true,
            ScrollBars = ScrollBars.Vertical,
            Height = 62,
            Dock = DockStyle.Fill,
            Text = string.Join(Environment.NewLine, pages),
        };
        box.TextChanged += (_, _) =>
        {
            pages.Clear();
            foreach (var line in box.Lines)
            {
                var page = line.Trim();
                if (page.Length > 0) pages.Add(page);
            }
            Changed();
        };
        return box;
    }

    private Control Condition(NpcDef npc)
    {
        var mode = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 90 };
        mode.Items.AddRange(new object[] { "always", "onlyif", "hideif" });
        mode.SelectedItem = npc.Cond.Length == 0 ? "always" : (npc.CondHide ? "hideif" : "onlyif");

        var flag = new ComboBox { DropDownStyle = ComboBoxStyle.DropDown, Width = 170, Text = npc.Cond };
        foreach (var name in _data?.SetFlags ?? Enumerable.Empty<string>()) flag.Items.Add(name);
        flag.Enabled = npc.Cond.Length > 0;

        mode.SelectedIndexChanged += (_, _) =>
        {
            var choice = mode.SelectedItem as string ?? "always";
            flag.Enabled = choice != "always";
            npc.CondHide = choice == "hideif";
            npc.Cond = choice == "always" ? "" : flag.Text.Trim();
            Changed();
        };
        flag.TextChanged += (_, _) =>
        {
            if (flag.Enabled) npc.Cond = flag.Text.Trim();
            Changed();
        };
        return Beside(mode, flag);
    }

    private void Row(string label, Control control)
    {
        _rows.RowCount++;
        _rows.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        var caption = new Label
        {
            Text = label,
            AutoSize = true,
            Margin = new Padding(0, 6, 8, 2),
            ForeColor = SystemColors.GrayText,
        };
        control.Margin = new Padding(0, 2, 0, 2);
        // Anchoring both sides is what stretches a field across its cell.
        // Setting Anchor clears Dock, so only the fields that asked to fill get
        // it: an auto sized row of two boxes must stay its own width.
        if (control.Dock == DockStyle.Fill) control.Anchor = AnchorStyles.Left | AnchorStyles.Right;
        _rows.Controls.Add(caption, 0, _rows.RowCount - 1);
        _rows.Controls.Add(control, 1, _rows.RowCount - 1);
    }

    private static Control Note(string text) =>
        new Label { Text = text, AutoSize = true, Margin = new Padding(0, 6, 0, 2) };

    private static Control Beside(Control left, Control right)
    {
        var flow = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Margin = new Padding(0),
            WrapContents = false,
        };
        left.Margin = new Padding(0, 0, 6, 0);
        right.Margin = new Padding(0);
        flow.Controls.Add(left);
        flow.Controls.Add(right);
        return flow;
    }

    private Control TextField(string value, Action<string> set)
    {
        var box = new TextBox { Text = value, Dock = DockStyle.Fill };
        box.TextChanged += (_, _) => set(box.Text);
        return box;
    }

    private Control Number(int value, int low, int high, Action<int> set)
    {
        var box = new NumericUpDown
        {
            Minimum = low, Maximum = high, Width = 90,
            Value = Math.Clamp(value, low, high),
        };
        box.ValueChanged += (_, _) => set((int)box.Value);
        return box;
    }

    /// <summary>A closed list: the compiler accepts these and nothing else.</summary>
    private Control Choice(IEnumerable<string> options, string value, Action<string> set)
    {
        var box = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
        foreach (var option in options) box.Items.Add(option);
        // A value the list does not have is still shown rather than silently
        // becoming the first option, which would edit the file by opening it.
        if (value.Length > 0 && !box.Items.Contains(value)) box.Items.Add(value);
        box.SelectedItem = value;
        box.SelectedIndexChanged += (_, _) =>
        {
            if (box.SelectedItem is string chosen) set(chosen);
        };
        return box;
    }

    /// <summary>An open list: the value is free text and these are only the
    /// ones already in use.</summary>
    private Control Free(IEnumerable<string> options, string value, Action<string> set)
    {
        var box = new ComboBox { DropDownStyle = ComboBoxStyle.DropDown, Dock = DockStyle.Fill, Text = value };
        foreach (var option in options) box.Items.Add(option);
        box.TextChanged += (_, _) => set(box.Text);
        return box;
    }

    private static decimal Clamp(int value) => Math.Clamp(value, 0, 254);
}
