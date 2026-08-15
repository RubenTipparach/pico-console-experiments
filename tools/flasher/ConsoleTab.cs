namespace PicoFlasher;

/// <summary>
/// Choosing what is on the console: the games available on the left, the
/// menu being built on the right, and a button that makes the .uf2.
///
/// This talks to IConsoleBackend and nothing else. It has no idea the recipe
/// is a yaml file or that building runs CMake, which is the point: the last
/// version of this window knew it was assembling flash slots, so when the
/// console stopped being slots the window had to go. Whatever replaces the
/// backend next, this file should not need to change.
/// </summary>
public sealed class ConsoleTab : UserControl
{
    private readonly ListView _available = new();
    private readonly ListView _menu = new();
    private readonly ImageList _icons = new();
    private readonly TextBox _title = new();
    private readonly TextBox _log = new();
    private readonly Label _status = new();

    private readonly Button _add = new();
    private readonly Button _remove = new();
    private readonly Button _heading = new();
    private readonly Button _rename = new();
    private readonly Button _up = new();
    private readonly Button _down = new();
    private readonly Button _build = new();
    private readonly Button _buildAndFlash = new();

    private readonly ComboBox _board = new();
    private readonly ComboBox _bundle = new();
    private readonly Button _saveBundle = new();
    private readonly MemoryBar _memory = new();

    // Set while a dropdown is being repopulated, so the handler that reacts to
    // a person choosing something does not also fire for the code that put the
    // items there. Without it, refreshing the bundle list reloads the menu and
    // throws away the edit that prompted the refresh.
    private bool _populating;

    // The last measurement, kept because taking one runs arm-none-eabi-size
    // and Revalidate fires on every keystroke in the title box. Measuring
    // there spawned a process per character typed. Re-measured only when the
    // thing it describes can actually have changed: a build finished, or the
    // target board moved to one with a different build directory.
    private ConsoleSize? _size;
    private bool _sizeMeasured;

    private readonly List<RecipeEntry> _rows = new();
    private IReadOnlyList<AvailableGame> _games = Array.Empty<AvailableGame>();
    private IConsoleBackend _backend;
    private bool _building;

    // The last validation, so UpdateButtons does not run a second full pass
    // over the filesystem for an answer Revalidate just worked out.
    private IReadOnlyList<Problem> _problems = Array.Empty<Problem>();

    /// <summary>Raised with a freshly built .uf2 the user asked to flash.</summary>
    public event Action<string>? FlashRequested;

    public ConsoleTab(IConsoleBackend backend)
    {
        _backend = backend;

        Dock = DockStyle.Fill;
        Padding = new Padding(10);

        _icons.ImageSize = new Size(48, 48);
        _icons.ColorDepth = ColorDepth.Depth32Bit;

        _available.Dock = DockStyle.Fill;
        _available.View = View.LargeIcon;
        _available.MultiSelect = true;
        _available.HideSelection = false;
        _available.LargeImageList = _icons;
        _available.DoubleClick += (_, _) => AddSelected();

        _menu.Dock = DockStyle.Fill;
        _menu.View = View.Details;
        _menu.MultiSelect = true;
        _menu.HideSelection = false;
        _menu.FullRowSelect = true;
        _menu.HeaderStyle = ColumnHeaderStyle.None;
        _menu.SmallImageList = _icons;
        _menu.Columns.Add("Row", 320);
        _menu.DoubleClick += (_, _) => RenameSelected();
        _menu.SelectedIndexChanged += (_, _) => UpdateButtons();

        _title.Dock = DockStyle.Fill;
        _title.TextChanged += (_, _) => Revalidate();

        _log.Dock = DockStyle.Fill;
        _log.Multiline = true;
        _log.ReadOnly = true;
        _log.ScrollBars = ScrollBars.Vertical;
        _log.Font = new Font("Consolas", 8F);
        _log.Height = 90;

        _status.Dock = DockStyle.Fill;
        _status.AutoSize = true;

        foreach (var (button, text, handler) in new (Button, string, Action)[]
                 {
                     (_add, "Add →", AddSelected),
                     (_remove, "← Remove", RemoveSelected),
                     (_heading, "Heading...", AddHeading),
                     (_rename, "Rename...", RenameSelected),
                     (_up, "Move up", () => MoveSelected(-1)),
                     (_down, "Move down", () => MoveSelected(1)),
                 })
        {
            button.Text = text;
            button.AutoSize = true;
            button.Width = 104;
            button.Margin = new Padding(6, 4, 6, 4);
            button.Click += (_, _) => handler();
        }

        _build.Text = "Build console";
        _build.AutoSize = true;
        _build.Height = 32;
        _build.Click += (_, _) => Build(thenFlash: false);

        _buildAndFlash.Text = "Build and flash";
        _buildAndFlash.AutoSize = true;
        _buildAndFlash.Height = 32;
        _buildAndFlash.Click += (_, _) => Build(thenFlash: true);

        // Target hardware. There is no autodetecting this at build time: there
        // may be no board plugged in, and the two do not share a toolchain, a
        // build directory or an SRAM ceiling. Flashing is the other way round
        // and does detect, off the .uf2's own family id.
        _board.DropDownStyle = ComboBoxStyle.DropDownList;
        _board.Width = 132;
        foreach (var spec in BoardSpec.All) _board.Items.Add(spec);
        _board.SelectedIndex = 0;
        _board.SelectedIndexChanged += (_, _) =>
        {
            if (_populating) return;
            if (_board.SelectedItem is not BoardSpec spec) return;
            _backend.Board = spec.Board;
            RefreshMemory(remeasure: true);
            Revalidate();
        };

        _bundle.DropDownStyle = ComboBoxStyle.DropDownList;
        _bundle.Width = 176;
        _bundle.SelectedIndexChanged += (_, _) =>
        {
            if (_populating) return;
            LoadSelectedBundle();
        };

        _saveBundle.Text = "Save bundle...";
        _saveBundle.AutoSize = true;
        _saveBundle.Width = 118;
        _saveBundle.Click += (_, _) => SaveBundleAs();

        Controls.Add(BuildLayout());

        Reload();
    }

    private Control BuildLayout()
    {
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 3,
            RowCount = 6,
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        // The strip: target, bundles, memory bar.
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var titleRow = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            AutoSize = true,
        };
        titleRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        titleRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        titleRow.Controls.Add(new Label
        {
            Text = "Console title",
            AutoSize = true,
            Margin = new Padding(0, 6, 8, 0),
        }, 0, 0);
        titleRow.Controls.Add(_title, 1, 0);
        layout.Controls.Add(titleRow, 0, 0);
        layout.SetColumnSpan(titleRow, 3);

        // Target, bundles and the memory bar on one strip above the lists.
        // Everything here is about the console as a whole rather than about a
        // row, which is why it sits above the two lists and not beside them.
        var strip = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            Dock = DockStyle.Fill,
            AutoSize = true,
            WrapContents = false,
            Margin = new Padding(0, 4, 0, 4),
        };
        strip.Controls.Add(new Label
        {
            Text = "Target",
            AutoSize = true,
            Margin = new Padding(0, 7, 6, 0),
        });
        strip.Controls.Add(_board);
        strip.Controls.Add(new Label
        {
            Text = "Bundle",
            AutoSize = true,
            Margin = new Padding(18, 7, 6, 0),
        });
        strip.Controls.Add(_bundle);
        strip.Controls.Add(_saveBundle);
        strip.Controls.Add(_memory);
        layout.Controls.Add(strip, 0, 1);
        layout.SetColumnSpan(strip, 3);

        var middle = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.TopDown,
            Dock = DockStyle.Fill,
            AutoSize = true,
            Padding = new Padding(0, 40, 0, 0),
        };
        foreach (var button in new[] { _add, _remove, _heading, _rename, _up, _down })
        {
            middle.Controls.Add(button);
        }

        layout.Controls.Add(_available, 0, 2);
        layout.Controls.Add(middle, 1, 2);
        layout.Controls.Add(_menu, 2, 2);

        var buttons = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            Dock = DockStyle.Fill,
            AutoSize = true,
        };
        buttons.Controls.Add(_build);
        buttons.Controls.Add(_buildAndFlash);
        layout.Controls.Add(buttons, 0, 3);
        layout.SetColumnSpan(buttons, 3);

        layout.Controls.Add(_status, 0, 4);
        layout.SetColumnSpan(_status, 3);
        layout.Controls.Add(_log, 0, 5);
        layout.SetColumnSpan(_log, 3);

        return layout;
    }

    /// <summary>Points the tab at a different checkout.</summary>
    public void SetBackend(IConsoleBackend backend)
    {
        _backend = backend;
        Reload();
    }

    // ---- bundles ----

    /// <summary>
    /// The saved lists, with an unnamed first entry for "whatever console.yaml
    /// currently holds". That entry is what the window opens on and what a
    /// build uses, so there is always something selected and never a state
    /// where the dropdown implies an edit that has not happened.
    /// </summary>
    private void RefreshBundles(string? select = null)
    {
        _populating = true;
        try
        {
            _bundle.Items.Clear();
            _bundle.Items.Add(CurrentBundleLabel);
            foreach (var name in _backend.ListBundles()) _bundle.Items.Add(name);

            var at = select is null ? 0 : _bundle.Items.IndexOf(select);
            _bundle.SelectedIndex = at < 0 ? 0 : at;
        }
        finally
        {
            _populating = false;
        }
    }

    private const string CurrentBundleLabel = "(current)";

    private void LoadSelectedBundle()
    {
        if (_bundle.SelectedItem is not string name) return;
        var recipe = name == CurrentBundleLabel
            ? _backend.Load()
            : _backend.LoadBundle(name);

        _title.Text = recipe.Title;
        _rows.Clear();
        _rows.AddRange(recipe.Entries);
        RefreshMenu();
    }

    private void SaveBundleAs()
    {
        var suggested = _bundle.SelectedItem as string;
        if (suggested is null or CurrentBundleLabel) suggested = _title.Text;

        var name = Prompt.Ask(this, "Save bundle",
            "A name for this list of games. Saving one keeps the menu you have "
            + "arranged so you can come back to it; it does not change how the "
            + "console is built.",
            suggested ?? "");
        if (string.IsNullOrWhiteSpace(name)) return;

        try
        {
            _backend.SaveBundle(name.Trim(), CurrentRecipe());
        }
        catch (IOException error)
        {
            _status.ForeColor = Color.FromArgb(168, 32, 32);
            _status.Text = $"Could not save the bundle: {error.Message}";
            return;
        }
        catch (UnauthorizedAccessException error)
        {
            _status.ForeColor = Color.FromArgb(168, 32, 32);
            _status.Text = $"Could not save the bundle: {error.Message}";
            return;
        }

        RefreshBundles(name.Trim());
        _status.ForeColor = SystemColors.ControlText;
        _status.Text = $"Saved bundle \"{name.Trim()}\".";
    }

    // ---- the memory bar ----

    /// <summary>
    /// Shows what the last build for the selected board cost, and says so when
    /// the menu has moved on since. Comparing game counts rather than the whole
    /// recipe on purpose: reordering a menu or renaming a row does not change
    /// what is linked in, so calling those stale would cry wolf.
    /// </summary>
    private void RefreshMemory(bool remeasure = false)
    {
        var spec = _board.SelectedItem as BoardSpec ?? BoardSpec.PicoSystem;

        if (remeasure || !_sizeMeasured)
        {
            try { _size = _backend.MeasureLastBuild(); }
            catch (IOException) { _size = null; }
            _sizeMeasured = true;
        }

        var games = _rows.OfType<GameEntry>().Count();
        var stale = _size is not null && _size.Games != games;
        _memory.Show(_size, stale, $"no {spec.Name} build yet");
    }

    private void Reload()
    {
        _games = _backend.DiscoverGames();

        _icons.Images.Clear();
        _available.Items.Clear();
        foreach (var game in _games)
        {
            _icons.Images.Add(LoadIcon(game.ThumbnailPath));
            _available.Items.Add(new ListViewItem(game.Title)
            {
                Tag = game,
                ImageIndex = _icons.Images.Count - 1,
                ToolTipText = string.IsNullOrWhiteSpace(game.Blurb)
                    ? game.Slug
                    : $"{game.Slug}\n{game.Blurb}",
            });
        }

        _sizeMeasured = false;
        RefreshBundles();
        var recipe = _backend.Load();
        _title.Text = recipe.Title;
        _rows.Clear();
        _rows.AddRange(recipe.Entries);
        RefreshMenu();
    }

    private static Image LoadIcon(string? path)
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
                return new Bitmap(original, new Size(48, 48));
            }
            catch (IOException) { }
            catch (ArgumentException) { }
        }

        var placeholder = new Bitmap(48, 48);
        using var graphics = Graphics.FromImage(placeholder);
        graphics.Clear(Color.FromArgb(58, 58, 68));
        using var pen = new Pen(Color.FromArgb(150, 150, 160), 3);
        graphics.DrawLine(pen, 8, 8, 40, 40);
        return placeholder;
    }

    private ConsoleRecipe CurrentRecipe() => new(_title.Text, _rows.ToList());

    private void RefreshMenu()
    {
        var selected = _menu.SelectedIndices.Cast<int>().ToHashSet();
        _menu.BeginUpdate();
        _menu.Items.Clear();

        for (var i = 0; i < _rows.Count; i++)
        {
            var item = new ListViewItem(RowLabel(_rows[i])) { Tag = _rows[i] };
            if (_rows[i] is GameEntry game)
            {
                var at = _games.ToList().FindIndex(g => g.Slug == game.Slug);
                if (at >= 0) item.ImageIndex = at;
            }
            else
            {
                item.ForeColor = Color.FromArgb(90, 100, 120);
            }
            _menu.Items.Add(item);
        }

        foreach (var index in selected)
        {
            if (index < _menu.Items.Count) _menu.Items[index].Selected = true;
        }
        _menu.EndUpdate();

        Revalidate();
    }

    // The rules either side of a heading are plain ASCII hyphens. They used to
    // be U+2500 box drawing characters, which join up into one long dash and
    // read on screen as an em dash, and that character does not go in front of
    // anyone in this project.
    private string RowLabel(RecipeEntry entry) => entry switch
    {
        HeadingEntry heading => $"-- {ConsoleYamlBackend.DisplayName(heading.Text)} --",
        GameEntry game => ConsoleYamlBackend.DisplayName(NameOf(game)),
        _ => "?",
    };

    private string NameOf(GameEntry entry)
    {
        if (!string.IsNullOrWhiteSpace(entry.Name)) return entry.Name!;
        return _games.FirstOrDefault(g => g.Slug == entry.Slug)?.Title ?? entry.Slug;
    }

    /// <summary>
    /// Re-checks the whole recipe and marks the rows worth looking at. Runs on
    /// every edit rather than at build time: a title one letter too wide for
    /// the header should be red here, not a failure after a compile.
    ///
    /// Two kinds of mark, because they mean different things. Red is a fault
    /// and the build is off until it is fixed. Amber is a note: the row is
    /// fine and something about it is worth knowing, which today means a name
    /// long enough that the menu will scroll it. A note that turned the build
    /// off would be this tool refusing a thing the console does on purpose.
    /// </summary>
    private void Revalidate()
    {
        var problems = _backend.Validate(CurrentRecipe());
        _problems = problems;

        foreach (ListViewItem item in _menu.Items)
        {
            item.BackColor = Color.White;
            item.ToolTipText = "";
        }

        foreach (var problem in problems.Where(p => p.EntryIndex >= 0))
        {
            if (problem.EntryIndex >= _menu.Items.Count) continue;
            var item = _menu.Items[problem.EntryIndex];
            // A row with a fault stays red even if it also has a note.
            if (problem.Blocking || item.BackColor == Color.White)
            {
                item.BackColor = problem.Blocking
                    ? Color.FromArgb(255, 226, 226)
                    : Color.FromArgb(255, 243, 214);
            }
            item.ToolTipText = string.IsNullOrEmpty(item.ToolTipText)
                ? problem.Message
                : item.ToolTipText + "\n" + problem.Message;
        }

        var games = _rows.OfType<GameEntry>().Count();
        var fault = problems.FirstOrDefault(p => p.Blocking);
        var note = problems.FirstOrDefault(p => !p.Blocking);
        if (fault is not null)
        {
            _status.ForeColor = Color.FromArgb(168, 32, 32);
            _status.Text = fault.Message;
        }
        else if (note is not null)
        {
            _status.ForeColor = Color.FromArgb(150, 96, 0);
            _status.Text = note.Message;
        }
        else
        {
            _status.ForeColor = SystemColors.ControlText;
            _status.Text = $"{games} game(s), {_rows.Count} row(s). Ready to build.";
        }

        RefreshMemory();
        UpdateButtons();
    }

    private void UpdateButtons()
    {
        var hasSelection = _menu.SelectedIndices.Count > 0;
        var ready = !_building && !_problems.Any(p => p.Blocking);

        _add.Enabled = !_building;
        _heading.Enabled = !_building;
        _remove.Enabled = hasSelection && !_building;
        _rename.Enabled = _menu.SelectedIndices.Count == 1 && !_building;
        _up.Enabled = hasSelection && !_building;
        _down.Enabled = hasSelection && !_building;
        _build.Enabled = ready;
        _buildAndFlash.Enabled = ready;
        // The target decides which script runs and which directory it writes
        // into, so changing it mid build would describe the wrong one.
        _board.Enabled = !_building;
        _bundle.Enabled = !_building;
        _saveBundle.Enabled = !_building;
    }

    // ---- editing ----

    /// <summary>
    /// Adds the games picked on the left, directly under the row selected on
    /// the right (at the end when nothing is selected). The menu is an order
    /// being arranged, and a game that lands at the bottom of a long list has
    /// to be walked back up to where it was wanted, one Move up at a time.
    /// Same rule as Heading..., which already inserted where you were looking.
    /// </summary>
    private void AddSelected()
    {
        var at = _menu.SelectedIndices.Count > 0
            ? _menu.SelectedIndices[^1] + 1
            : _rows.Count;
        at = Math.Clamp(at, 0, _rows.Count);

        var added = new List<int>();
        foreach (ListViewItem item in _available.SelectedItems)
        {
            if (item.Tag is not AvailableGame game) continue;
            _rows.Insert(at, new GameEntry(game.Slug, null));
            added.Add(at);
            at++;
        }
        if (added.Count == 0) return;

        RefreshMenu();

        // The new rows end up selected, so a second add goes under them: a
        // group can be built downward without reaching for the mouse between
        // one game and the next.
        _menu.SelectedIndices.Clear();
        foreach (var index in added)
        {
            if (index < _menu.Items.Count) _menu.Items[index].Selected = true;
        }
        _menu.Items[added[^1]].EnsureVisible();
    }

    private void RemoveSelected()
    {
        foreach (var index in _menu.SelectedIndices.Cast<int>().OrderByDescending(i => i))
        {
            if (index < _rows.Count) _rows.RemoveAt(index);
        }
        RefreshMenu();
    }

    private void AddHeading()
    {
        var text = Prompt.Ask(this, "Heading", "Text for the heading row:", "");
        if (string.IsNullOrWhiteSpace(text)) return;

        var at = _menu.SelectedIndices.Count > 0
            ? _menu.SelectedIndices[^1] + 1
            : _rows.Count;
        _rows.Insert(Math.Clamp(at, 0, _rows.Count), new HeadingEntry(text));
        RefreshMenu();
    }

    private void RenameSelected()
    {
        if (_menu.SelectedIndices.Count != 1) return;
        var index = _menu.SelectedIndices[0];
        if (index >= _rows.Count) return;

        switch (_rows[index])
        {
            case HeadingEntry heading:
            {
                var text = Prompt.Ask(this, "Heading", "Text for the heading row:",
                                      heading.Text);
                if (text is null) return;
                _rows[index] = new HeadingEntry(text);
                break;
            }
            case GameEntry game:
            {
                var text = Prompt.Ask(this, "Name on the menu",
                    $"What {game.Slug} is called on the console.\n" +
                    "Leave it empty to follow the game's own title.",
                    game.Name ?? "");
                if (text is null) return;
                _rows[index] = game with
                {
                    Name = string.IsNullOrWhiteSpace(text) ? null : text,
                };
                break;
            }
        }
        RefreshMenu();
    }

    private void MoveSelected(int direction)
    {
        var indices = _menu.SelectedIndices.Cast<int>().ToList();
        if (indices.Count == 0) return;

        // Walked from the end the row is moving toward, so a block of rows
        // moving together does not have them swap past each other.
        var order = direction < 0 ? indices.OrderBy(i => i) : indices.OrderByDescending(i => i);
        var moved = new List<int>();
        foreach (var index in order)
        {
            var target = index + direction;
            if (target < 0 || target >= _rows.Count || moved.Contains(target))
            {
                moved.Add(index);
                continue;
            }
            (_rows[index], _rows[target]) = (_rows[target], _rows[index]);
            moved.Add(target);
        }

        RefreshMenu();
        _menu.SelectedIndices.Clear();
        foreach (var index in moved)
        {
            if (index >= 0 && index < _menu.Items.Count) _menu.Items[index].Selected = true;
        }
    }

    // ---- building ----

    private async void Build(bool thenFlash)
    {
        if (_building) return;

        if (!_backend.CanBuild(out var why))
        {
            MessageBox.Show(this, why, "Cannot build here",
                            MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        _building = true;
        UpdateButtons();
        _log.Clear();
        _status.ForeColor = SystemColors.ControlText;
        _status.Text = $"Building with {_backend.Description}. This takes a while.";

        var log = new Progress<string>(line =>
        {
            _log.AppendText(line + Environment.NewLine);
        });

        BuildOutcome outcome;
        try
        {
            outcome = await _backend.BuildAsync(CurrentRecipe(), log, CancellationToken.None);
        }
        catch (Exception error)
        {
            outcome = new BuildOutcome(false, $"The build threw: {error.Message}", null);
        }

        _building = false;
        _status.ForeColor = outcome.Success
            ? SystemColors.ControlText
            : Color.FromArgb(168, 32, 32);
        _status.Text = outcome.Message;
        RefreshMemory(remeasure: true);
        UpdateButtons();

        if (outcome.Success && thenFlash && outcome.Uf2Path is not null)
        {
            FlashRequested?.Invoke(outcome.Uf2Path);
        }
    }

    /// <summary>Persists the current list, so closing the window keeps the edit.</summary>
    public void SaveQuietly()
    {
        try { _backend.Save(CurrentRecipe()); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}

/// <summary>
/// A one line text prompt, because WinForms has no InputBox and the two
/// things this tab asks for are both one line of text.
/// </summary>
internal static class Prompt
{
    public static string? Ask(IWin32Window owner, string title, string message,
                              string initial)
    {
        using var form = new Form
        {
            Text = title,
            FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition = FormStartPosition.CenterParent,
            MinimizeBox = false,
            MaximizeBox = false,
            ClientSize = new Size(420, 150),
        };

        var label = new Label
        {
            Text = message,
            Left = 12,
            Top = 12,
            Width = 396,
            Height = 48,
        };
        var box = new TextBox { Text = initial, Left = 12, Top = 66, Width = 396 };
        var ok = new Button
        {
            Text = "OK",
            DialogResult = DialogResult.OK,
            Left = 232,
            Top = 100,
            Width = 84,
        };
        var cancel = new Button
        {
            Text = "Cancel",
            DialogResult = DialogResult.Cancel,
            Left = 324,
            Top = 100,
            Width = 84,
        };

        form.Controls.AddRange(new Control[] { label, box, ok, cancel });
        form.AcceptButton = ok;
        form.CancelButton = cancel;

        return form.ShowDialog(owner) == DialogResult.OK ? box.Text : null;
    }
}
