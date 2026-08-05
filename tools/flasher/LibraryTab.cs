namespace PicoFlasher;

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

        _directory.Dock = DockStyle.Fill;
        _directory.Text = GameLibrary.DefaultDirectory(repositoryRoot);
        _directory.TextChanged += (_, _) => Rescan();

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

        _save.Text = "Save bundle...";
        _save.AutoSize = true;
        _save.Click += (_, _) => SaveBundle();

        _flash.Text = "Flash to console";
        _flash.AutoSize = true;
        _flash.Height = 34;
        _flash.Click += (_, _) => FlashBundle();

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
        buttons.Controls.Add(_device);
        layout.Controls.Add(buttons, 0, 2);
        layout.SetColumnSpan(buttons, 3);

        layout.Controls.Add(_status, 0, 3);
        layout.SetColumnSpan(_status, 3);

        Controls.Add(layout);
        Rescan();
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

        foreach (var item in GameLibrary.Scan(_directory.Text, _repositoryRoot))
        {
            _items.Add(item);
        }

        // The launcher is not a game and cannot go in a slot, and a standalone
        // build cannot go in a bundle at all. Both stay visible so it is clear
        // what the library holds, but only slot builds are addable.
        foreach (var item in _items)
        {
            var view = new ListViewItem(item.Title)
            {
                Tag = item,
                ToolTipText = $"{item.Detail}\n{item.Path}",
            };
            view.ImageIndex = IconFor(item);
            _library.Items.Add(view);
        }

        _status.Text = _items.Count == 0
            ? $"Nothing in {_directory.Text}. Put built .uf2 files there, or use Add file."
            : $"{_items.Count} file(s) in the library.";
        UpdateButtons();
    }

    private int IconFor(LibraryItem item)
    {
        var icon = item.Meta?.Icon;
        if (icon is null) return -1;
        _icons.Images.Add(icon);
        return _icons.Images.Count - 1;
    }

    private void ImportFile()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "UF2 images (*.uf2)|*.uf2|All files (*.*)|*.*",
            Title = "Add a .uf2 to the library",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        var copied = GameLibrary.Import(dialog.FileName, _directory.Text);
        _status.Text = copied is null
            ? "Could not copy that file into the library."
            : $"Added {Path.GetFileName(copied)}.";
        Rescan();
    }

    private void AddSelected()
    {
        foreach (ListViewItem view in _library.SelectedItems)
        {
            if (view.Tag is not LibraryItem item) continue;
            if (!item.IsSlotBuild)
            {
                _status.Text = item.IsLauncher
                    ? "The launcher is added automatically; it owns slot 0."
                    : $"{item.Title} is a standalone build, linked at the base of " +
                      "flash. A bundle needs one built with -DPICO_SLOT=n.";
                continue;
            }
            if (_chosen.Count >= Bundle.SlotCount)
            {
                _status.Text = $"A bundle holds {Bundle.SlotCount} games.";
                break;
            }
            if (_chosen.Any(chosen => chosen.Path == item.Path)) continue;
            _chosen.Add(item);
        }
        RefreshBundle();
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
            var view = new ListViewItem($"{i + 1}. {item.Title}")
            {
                Tag = item,
                ToolTipText = item.Path,
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
    }

    private void UpdateButtons()
    {
        var haveGames = _chosen.Count > 0;
        _save.Enabled = haveGames;
        _flash.Enabled = haveGames && _selectedDrive() is not null;
        _remove.Enabled = _bundle.SelectedItems.Count > 0 || haveGames;
        _up.Enabled = haveGames;
        _down.Enabled = haveGames;
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
            placements.Add(new Bundle.Placement(item.Title, item.Slot, item.Path));
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
        _status.Text = $"Flashing {_chosen.Count} game(s)...";

        Task.Run(() => Uf2Flasher.Flash(temporary, drive.Root))
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
