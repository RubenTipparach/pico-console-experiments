using System.Text.Json;

namespace PicoFlasher;

/// <summary>One saved entry in bundle_selection.json: which file, and how it was placed.</summary>
internal sealed class SelectionEntry
{
    public string Path { get; set; } = "";
    public int Slot { get; set; }
    public bool Forced { get; set; }
}

/// <summary>
/// The library: what you have built on the left, what is going on the console
/// on the right, and a button that writes it.
///
/// One thing this cannot do, and says so rather than pretending: read the
/// console. RPI-RP2 is a fake FAT volume with no storage behind it, so a board
/// in BOOTSEL cannot be asked what is installed. The right hand list is
/// therefore the bundle being built, and the tool remembers what it last
/// flashed so that answer is at least honest about where it came from.
/// </summary>
public sealed class LibraryTab : UserControl
{
    private readonly ListView _library = new();
    private readonly ListView _bundle = new();
    private readonly ImageList _icons = new();
    private readonly Button _add = new();
    private readonly Button _remove = new();
    private readonly Button _up = new();
    private readonly Button _down = new();
    private readonly Button _flash = new();
    private readonly Button _save = new();
    private readonly Button _import = new();
    private readonly Button _clearSlots = new();
    private readonly CheckBox _forceMode = new();
    private readonly Label _status = new();
    private readonly Label _device = new();
    private readonly TextBox _directory = new();

    private readonly List<LibraryItem> _items = new();
    private readonly List<LibraryItem> _chosen = new();
    private readonly Func<BootselDrive?> _selectedDrive;
    private string _repositoryRoot;

    public LibraryTab(string repositoryRoot, Func<BootselDrive?> selectedDrive)
    {
        _repositoryRoot = repositoryRoot;
        _selectedDrive = selectedDrive;

        Dock = DockStyle.Fill;
        Padding = new Padding(10);

        _icons.ImageSize = new Size(48, 48);
        _icons.ColorDepth = ColorDepth.Depth32Bit;

        ConfigureList(_library);
        ConfigureList(_bundle);
        _library.LargeImageList = _icons;
        _bundle.LargeImageList = _icons;
        _library.DoubleClick += (_, _) => AddSelected();
        _bundle.DoubleClick += (_, _) => RemoveSelected();

        // Drop target is the library list, not the whole tab: dropping onto
        // the bundle side would be ambiguous (import, or add-to-bundle?),
        // and the library list is where "Add file..." already puts things.
        _library.AllowDrop = true;
        _library.DragEnter += (_, e) =>
        {
            e.Effect = e.Data?.GetDataPresent(DataFormats.FileDrop) == true
                ? DragDropEffects.Copy
                : DragDropEffects.None;
        };
        _library.DragDrop += (_, e) =>
        {
            if (e.Data?.GetData(DataFormats.FileDrop) is string[] paths) ImportPaths(paths);
        };

        _directory.Dock = DockStyle.Fill;
        _directory.Text = GameLibrary.DefaultDirectory(repositoryRoot);
        // A different directory is a different library, with its own
        // remembered bundle: the in-memory selection belongs to whichever
        // directory it was loaded from, not to the tab in general.
        _directory.TextChanged += (_, _) =>
        {
            _chosen.Clear();
            Rescan();
            LoadSelection();
        };

        _import.Text = "Add file...";
        _import.AutoSize = true;
        _import.Click += (_, _) => ImportFile();

        _add.Text = "Add →";
        _remove.Text = "← Remove";
        _up.Text = "Move up";
        _down.Text = "Move down";
        foreach (var button in new[] { _add, _remove, _up, _down })
        {
            button.AutoSize = true;
            button.Width = 96;
            button.Margin = new Padding(6, 4, 6, 4);
        }
        _add.Click += (_, _) => AddSelected();
        _remove.Click += (_, _) => RemoveSelected();
        _up.Click += (_, _) => Move(-1);
        _down.Click += (_, _) => Move(1);

        // Off by default, and Add still refuses a non slot item with it off:
        // this has to be switched on on purpose, not landed on by habit.
        _forceMode.Text = "Force add (unsafe)";
        _forceMode.AutoSize = true;
        _forceMode.Margin = new Padding(6, 10, 6, 4);

        _save.Text = "Save bundle...";
        _save.AutoSize = true;
        _save.Click += (_, _) => SaveBundle();

        _flash.Text = "Flash to console";
        _flash.AutoSize = true;
        _flash.Height = 34;
        _flash.Click += (_, _) => FlashBundle();

        _clearSlots.Text = "Clear all slots...";
        _clearSlots.AutoSize = true;
        _clearSlots.Click += (_, _) => ClearAllSlots();

        _device.AutoSize = true;
        _device.Text = "No board in BOOTSEL.";
        _status.AutoSize = true;
        _status.Text = "Pick games and add them to the bundle.";

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 3,
            RowCount = 4,
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var top = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            AutoSize = true,
        };
        top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        top.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        top.Controls.Add(_directory, 0, 0);
        top.Controls.Add(_import, 1, 0);
        layout.Controls.Add(top, 0, 0);
        layout.SetColumnSpan(top, 3);

        var middle = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.TopDown,
            Dock = DockStyle.Fill,
            AutoSize = true,
            Padding = new Padding(0, 40, 0, 0),
        };
        middle.Controls.Add(_add);
        middle.Controls.Add(_remove);
        middle.Controls.Add(_up);
        middle.Controls.Add(_down);
        middle.Controls.Add(_forceMode);

        layout.Controls.Add(_library, 0, 1);
        layout.Controls.Add(middle, 1, 1);
        layout.Controls.Add(_bundle, 2, 1);

        var buttons = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            Dock = DockStyle.Fill,
            AutoSize = true,
        };
        buttons.Controls.Add(_flash);
        buttons.Controls.Add(_save);
        buttons.Controls.Add(_clearSlots);
        buttons.Controls.Add(_device);
        layout.Controls.Add(buttons, 0, 2);
        layout.SetColumnSpan(buttons, 3);

        layout.Controls.Add(_status, 0, 3);
        layout.SetColumnSpan(_status, 3);

        Controls.Add(layout);
        Rescan();
        LoadSelection();
    }

    private string SelectionPath => Path.Combine(_directory.Text, "bundle_selection.json");

    /// <summary>
    /// What is going on the console is the whole point of this tab, so
    /// losing it every time the app closes meant re-picking the same games
    /// on every launch. Saved next to the library itself, not somewhere
    /// under AppData: the selection belongs to this library, not to the
    /// install of the tool, so pointing the tab at a different directory
    /// gets a different remembered bundle rather than the wrong one.
    /// </summary>
    private void SaveSelection()
    {
        try
        {
            var entries = _chosen.Select(item => new SelectionEntry
            {
                Path = item.Path,
                Slot = item.Slot,
                Forced = item.Forced,
            }).ToList();
            Directory.CreateDirectory(_directory.Text);
            File.WriteAllText(SelectionPath,
                JsonSerializer.Serialize(entries, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    /// <summary>
    /// Restores the bundle from disk, matched back up against what Rescan
    /// just found by path. A game the file remembers but the library no
    /// longer has (moved, deleted, a different directory entirely) is
    /// silently dropped rather than kept as a phantom entry that can never
    /// really be there.
    /// </summary>
    private void LoadSelection()
    {
        List<SelectionEntry>? entries;
        try
        {
            if (!File.Exists(SelectionPath)) return;
            entries = JsonSerializer.Deserialize<List<SelectionEntry>>(File.ReadAllText(SelectionPath));
        }
        catch (IOException) { return; }
        catch (JsonException) { return; }
        if (entries is null) return;

        foreach (var entry in entries)
        {
            var item = _items.FirstOrDefault(i => i.Path == entry.Path);
            if (item is null) continue;
            _chosen.Add(item with { Slot = entry.Slot, Forced = entry.Forced });
        }
        RefreshBundle();
    }

    private static void ConfigureList(ListView list)
    {
        list.Dock = DockStyle.Fill;
        list.View = View.LargeIcon;
        list.MultiSelect = true;
        list.HideSelection = false;
    }

    /// <summary>Called by the form when the BOOTSEL drive list changes.</summary>
    public void DeviceChanged(int driveCount)
    {
        _device.Text = driveCount > 0
            ? "Board in BOOTSEL, ready to flash."
            : "No board in BOOTSEL. Hold X and press power.";
        UpdateButtons();
    }

    private void Rescan()
    {
        _items.Clear();
        _icons.Images.Clear();
        _library.Items.Clear();

        foreach (var item in GameLibrary.Scan(_directory.Text))
        {
            _items.Add(item);
        }

        // A standalone build of one of our own games is not shown: it is an
        // incidental side effect of build_uf2s.bat, run for an unrelated
        // reason (a single-game flash, a browser test), never something
        // actually added here, and it is never addable either. A foreign
        // .uf2 is different: dropping one in was a deliberate action, so it
        // stays visible, labeled "unrecognized" rather than silently gone,
        // even though it cannot be added any more than a standalone build
        // can. Only the launcher and slot builds respond to Add at all (see
        // AddSelected).
        var shown = 0;
        foreach (var item in _items)
        {
            if (item.Slot == -1) continue;
            shown++;

            var label = item.IsLauncher ? item.Title : $"{item.Title} ({item.ShortLabel})";
            var view = new ListViewItem(label)
            {
                Tag = item,
                ToolTipText = $"{item.Detail}\n{item.Path}",
            };
            view.ImageIndex = IconFor(item);
            _library.Items.Add(view);
        }

        _status.Text = shown == 0
            ? $"Nothing bundlable in {_directory.Text}. Drop .uf2 files here, or use Add file."
            : $"{_items.Count} file(s) in the library.";
        UpdateButtons();
    }

    /// <summary>
    /// A game's own icon when it has one; otherwise a generated placeholder,
    /// same idea as the on-device launcher's for a game with no metadata
    /// block (CLAUDE.md rule 8): a flat tile with a slash through it, so a
    /// homebrew .uf2 with nothing to show reads as "no picture", never as a
    /// blank tile that looks like a rendering bug.
    /// </summary>
    private int IconFor(LibraryItem item)
    {
        _icons.Images.Add(item.Meta?.Icon ?? MakePlaceholderIcon());
        return _icons.Images.Count - 1;
    }

    private static Bitmap MakePlaceholderIcon()
    {
        var bitmap = new Bitmap(48, 48);
        using var g = Graphics.FromImage(bitmap);
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        g.Clear(Color.FromArgb(58, 58, 68));
        using var pen = new Pen(Color.FromArgb(150, 150, 160), 3);
        g.DrawLine(pen, 8, 8, 40, 40);
        return bitmap;
    }

    private void ImportFile()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "UF2 images (*.uf2)|*.uf2|All files (*.*)|*.*",
            Title = "Add a .uf2 to the library",
            Multiselect = true,
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        ImportPaths(dialog.FileNames);
    }

    /// <summary>Shared by "Add file..." and dropping files onto the library list.</summary>
    private void ImportPaths(IEnumerable<string> paths)
    {
        var added = 0;
        var skipped = 0;
        foreach (var path in paths)
        {
            // A folder or a non-.uf2 file dropped by mistake is silently
            // uninteresting here, not an error dialog to dismiss.
            if (!string.Equals(Path.GetExtension(path), ".uf2",
                               StringComparison.OrdinalIgnoreCase))
            {
                skipped++;
                continue;
            }
            if (GameLibrary.Import(path, _directory.Text) is not null) added++;
        }

        _status.Text = added == 0
            ? "Nothing added. Drop or pick a .uf2 file."
            : skipped == 0
                ? $"Added {added} file(s)."
                : $"Added {added} file(s), skipped {skipped} that were not .uf2.";
        Rescan();
    }

    private void AddSelected()
    {
        foreach (ListViewItem view in _library.SelectedItems)
        {
            if (view.Tag is not LibraryItem item) continue;
            if (_chosen.Any(chosen => chosen.Path == item.Path)) continue;

            if (!item.IsSlotBuild)
            {
                if (item.IsLauncher)
                {
                    _status.Text = "The launcher is added automatically; it owns slot 0.";
                    continue;
                }
                if (_forceMode.Checked) { TryForceAdd(item); continue; }

                _status.Text = item.IsForeign
                    ? $"{item.Title} was not built by this project (no metadata " +
                      "block), so there is no way to tell where it is safe to " +
                      "put it in flash. Rebuilding it from source with " +
                      "-DPICO_SLOT=n is the real fix, or tick \"Force add\" to try " +
                      "it anyway at your own risk."
                    : $"{item.Title} is a standalone build, linked at the base " +
                      "of flash. A bundle needs one built with -DPICO_SLOT=n, or " +
                      "tick \"Force add\" to try it anyway at your own risk.";
                continue;
            }
            if (_chosen.Count >= Bundle.SlotCount)
            {
                _status.Text = $"A bundle holds {Bundle.SlotCount} games.";
                break;
            }
            _chosen.Add(item);
        }
        RefreshBundle();
    }

    /// <summary>
    /// Adds a non slot item anyway, at the next open slot, after a plain
    /// statement of what that actually means: the bytes get relocated so
    /// flashing will not clobber the launcher or another game, and it gets a
    /// name through the launcher's override table, but nothing here can fix
    /// an absolute address the game's own code still carries for wherever it
    /// actually thinks it is. See Bundle.Compose.
    /// </summary>
    private void TryForceAdd(LibraryItem item)
    {
        if (_chosen.Count >= Bundle.SlotCount)
        {
            _status.Text = $"A bundle holds {Bundle.SlotCount} games.";
            return;
        }
        var slot = NextFreeSlot();
        if (slot is null)
        {
            _status.Text = "Every slot is already taken.";
            return;
        }

        var answer = MessageBox.Show(this,
            $"{item.Title} was not built by this project, so its code is linked " +
            $"for a different address than slot {slot}.\n\n" +
            "Forcing it in will relocate its data so flashing will not overwrite " +
            "the launcher or another game, and will give it a name in the menu " +
            "through the launcher's own override table. It will NOT fix any " +
            "address the game's own code still expects internally, which this " +
            "tool has no way to know about.\n\n" +
            "Selecting it on the console may work, may do nothing, or may hang " +
            "requiring a BOOTSEL reflash to recover, not a restart.\n\n" +
            $"Force add {item.Title} into slot {slot} anyway?",
            "Force add: not built for a slot",
            MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
        if (answer != DialogResult.Yes) return;

        _chosen.Add(item with { Slot = slot.Value, Forced = true });
    }

    private int? NextFreeSlot()
    {
        var used = new HashSet<int>(_chosen.Select(item => item.Slot));
        for (var slot = 1; slot <= Bundle.SlotCount; slot++)
        {
            if (!used.Contains(slot)) return slot;
        }
        return null;
    }

    private void RemoveSelected()
    {
        var doomed = new List<LibraryItem>();
        foreach (ListViewItem view in _bundle.SelectedItems)
        {
            if (view.Tag is LibraryItem item) doomed.Add(item);
        }
        foreach (var item in doomed) _chosen.Remove(item);
        RefreshBundle();
    }

    private void Move(int direction)
    {
        if (_bundle.SelectedItems.Count != 1) return;
        var index = _bundle.SelectedIndices[0];
        var target = index + direction;
        if (target < 0 || target >= _chosen.Count) return;
        (_chosen[index], _chosen[target]) = (_chosen[target], _chosen[index]);
        RefreshBundle();
        _bundle.Items[target].Selected = true;
    }

    private void RefreshBundle()
    {
        _bundle.Items.Clear();
        for (var i = 0; i < _chosen.Count; i++)
        {
            var item = _chosen[i];
            var label = item.Forced ? $"{i + 1}. {item.Title} (forced)" : $"{i + 1}. {item.Title}";
            var view = new ListViewItem(label)
            {
                Tag = item,
                ToolTipText = item.Forced
                    ? $"{item.Path}\nnot built for this project, may not boot"
                    : item.Path,
            };
            // The image list is shared, so the index found for the library
            // entry is the same picture here.
            for (var l = 0; l < _library.Items.Count; l++)
            {
                if (_library.Items[l].Tag is LibraryItem other
                    && other.Path == item.Path)
                {
                    view.ImageIndex = _library.Items[l].ImageIndex;
                    break;
                }
            }
            _bundle.Items.Add(view);
        }
        UpdateButtons();
        SaveSelection();
    }

    private void UpdateButtons()
    {
        var haveGames = _chosen.Count > 0;
        _save.Enabled = haveGames;
        _flash.Enabled = haveGames && _selectedDrive() is not null;
        _remove.Enabled = _bundle.SelectedItems.Count > 0 || haveGames;
        _up.Enabled = haveGames;
        _down.Enabled = haveGames;
        _clearSlots.Enabled = _selectedDrive() is not null;
    }

    /// <summary>
    /// The games are placed in the order shown, but each one can only go in the
    /// slot it was linked for, so the order is presentation and the slot is the
    /// truth. Composing tells the user when those disagree.
    /// </summary>
    private Bundle.Result Build()
    {
        var launcher = _items.FirstOrDefault(item => item.IsLauncher);
        if (launcher is null)
        {
            return new Bundle.Result(false,
                "No launcher in the library. It is the launcher-bundle artifact " +
                "from CI, or a local build with -DPICO_LAUNCHER_ONLY=ON.");
        }

        var placements = new List<Bundle.Placement>();
        foreach (var item in _chosen)
        {
            placements.Add(new Bundle.Placement(item.Title, item.Slot, item.Path, item.Forced));
        }
        return Bundle.Compose(launcher.Path, placements);
    }

    private void SaveBundle()
    {
        var result = Build();
        if (!result.Success || result.Uf2 is null)
        {
            _status.Text = result.Message;
            return;
        }

        using var dialog = new SaveFileDialog
        {
            Filter = "UF2 images (*.uf2)|*.uf2",
            FileName = "bundle.uf2",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        try
        {
            File.WriteAllBytes(dialog.FileName, result.Uf2);
            _status.Text = $"Wrote {Path.GetFileName(dialog.FileName)}. {result.Message}";
        }
        catch (Exception error)
        {
            _status.Text = $"Could not write it: {error.Message}";
        }
    }

    private void FlashBundle()
    {
        if (_selectedDrive() is not BootselDrive drive)
        {
            _status.Text = "No board in BOOTSEL. Hold X and press power.";
            return;
        }

        var result = Build();
        if (!result.Success || result.Uf2 is null)
        {
            _status.Text = result.Message;
            return;
        }

        // Uf2Flasher works on a file, and a bundle lives in memory until now,
        // so it goes through a temp file rather than growing a second code path
        // for the one operation that must not have two.
        string temporary;
        try
        {
            temporary = Path.Combine(Path.GetTempPath(),
                                     $"pse-bundle-{Guid.NewGuid():N}.uf2");
            File.WriteAllBytes(temporary, result.Uf2);
        }
        catch (Exception error)
        {
            _status.Text = $"Could not stage the bundle: {error.Message}";
            return;
        }

        _flash.Enabled = false;
        _status.Text = $"Flashing {_chosen.Count} game(s)... 0%";
        var progress = new Progress<int>(
            percent => _status.Text = $"Flashing {_chosen.Count} game(s)... {percent}%");

        Task.Run(() => Uf2Flasher.Flash(temporary, drive.Root, progress))
            .ContinueWith(task =>
            {
                _status.Text = task.Result.Message;
                try { File.Delete(temporary); } catch (IOException) { }
                UpdateButtons();
            }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    /// <summary>
    /// Wipes every game slot back to empty, deliberately, on request. This is
    /// what a stale game from an earlier test flash actually needs: BOOTSEL
    /// flashing only ever writes the addresses a UF2's blocks cover, so a
    /// slot never chosen for the current bundle keeps whatever was there
    /// before forever, and normal flashing correctly leaves that alone
    /// (see Bundle.Compose). The launcher itself (slot 0) is never touched.
    /// </summary>
    private void ClearAllSlots()
    {
        if (_selectedDrive() is not BootselDrive drive)
        {
            _status.Text = "No board in BOOTSEL. Hold X and press power.";
            return;
        }

        var answer = MessageBox.Show(this,
            "This erases every game slot on the console, including anything " +
            "not in the bundle on the right. The launcher itself is not " +
            "touched. This cannot be undone from here — reflash whatever " +
            "you want back afterward.\n\nClear all slots?",
            "Clear all slots",
            MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
        if (answer != DialogResult.Yes) return;

        string temporary;
        try
        {
            temporary = Path.Combine(Path.GetTempPath(),
                                     $"pse-clear-{Guid.NewGuid():N}.uf2");
            File.WriteAllBytes(temporary, Bundle.ComposeClearAllSlots());
        }
        catch (Exception error)
        {
            _status.Text = $"Could not stage the clear: {error.Message}";
            return;
        }

        _clearSlots.Enabled = false;
        _status.Text = "Clearing all slots... 0%";
        var progress = new Progress<int>(percent => _status.Text = $"Clearing all slots... {percent}%");

        Task.Run(() => Uf2Flasher.Flash(temporary, drive.Root, progress))
            .ContinueWith(task =>
            {
                _status.Text = task.Result.Message;
                try { File.Delete(temporary); } catch (IOException) { }
                UpdateButtons();
            }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    public void SetRepositoryRoot(string root)
    {
        _repositoryRoot = root;
        _directory.Text = GameLibrary.DefaultDirectory(root);
    }
}
