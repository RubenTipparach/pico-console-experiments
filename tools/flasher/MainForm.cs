namespace PicoFlasher;

/// <summary>
/// Three tabs: Flash copies a .uf2 to the board, Console chooses what goes on
/// one, and Play builds a game for this PC and runs it without a device.
///
/// The Console tab is the bundle builder, back after the console stopped
/// being a set of flash slots and became one binary. The job it does for a
/// person did not change with that ("pick some games, make a console"), so
/// losing the window when the backend changed was a regression, not a
/// consequence. It now talks to IConsoleBackend, which is the seam that
/// should keep it alive through the next change too.
///
/// Quick and dirty on purpose (rule 13). No installer, no settings, no
/// framework. This is a tool, not a product, and every feature added here is a
/// feature that has to keep working.
/// </summary>
public sealed class MainForm : Form
{
    private const int WmDeviceChange = 0x0219;

    private readonly BootselWatcher _watcher = new();
    private readonly ComboBox _devices = new();
    private readonly ListBox _files = new();
    private readonly Button _flash = new();
    private readonly Button _browse = new();
    private readonly Button _rescan = new();
    private readonly ProgressBar _progress = new();
    private readonly Label _status = new();
    private readonly TextBox _root = new();

    private readonly TabControl _tabs = new();
    private readonly ConsoleTab _console;
    private readonly PlayTab _play;

    public MainForm()
    {
        Text = "PicoFlasher";
        Width = 760;
        Height = 620;
        MinimumSize = new Size(640, 520);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9F);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(12),
            ColumnCount = 3,
            RowCount = 6,
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var deviceLabel = new Label { Text = "Device", AutoSize = true, Margin = new Padding(0, 6, 0, 2) };
        _devices.Dock = DockStyle.Fill;
        _devices.DropDownStyle = ComboBoxStyle.DropDownList;
        _devices.SelectedIndexChanged += (_, _) => UpdateFlashEnabled();

        _rescan.Text = "Rescan";
        _rescan.AutoSize = true;
        _rescan.Click += (_, _) => _watcher.Refresh();

        _root.Dock = DockStyle.Fill;
        _root.Text = GuessRepositoryRoot();
        _root.TextChanged += (_, _) =>
        {
            ReloadFiles();
            // A different folder is a different checkout, so the console tab
            // is looking at a different set of games and a different menu, and
            // so is the play tab.
            var moved = new ConsoleYamlBackend(_root.Text);
            _console?.SetBackend(moved);
            _play?.SetGames(moved.DiscoverGames(), _root.Text);
        };

        _browse.Text = "Folder...";
        _browse.AutoSize = true;
        _browse.Click += (_, _) => BrowseForFolder();

        _files.Dock = DockStyle.Fill;
        _files.IntegralHeight = false;
        _files.SelectedIndexChanged += (_, _) => UpdateFlashEnabled();
        _files.DoubleClick += (_, _) => DoFlash();

        _progress.Dock = DockStyle.Fill;
        _progress.Height = 18;

        _flash.Text = "Flash";
        _flash.AutoSize = true;
        _flash.Height = 34;
        _flash.Enabled = false;
        _flash.Click += (_, _) => DoFlash();

        _status.Dock = DockStyle.Fill;
        _status.AutoSize = true;
        _status.Text = "Looking for a board...";

        layout.Controls.Add(deviceLabel, 0, 0);
        layout.SetColumnSpan(deviceLabel, 3);
        layout.Controls.Add(_devices, 0, 1);
        layout.Controls.Add(_rescan, 1, 1);
        layout.SetColumnSpan(_devices, 1);

        var filesLabel = new Label { Text = "Firmware", AutoSize = true, Margin = new Padding(0, 10, 0, 2) };
        layout.Controls.Add(filesLabel, 0, 2);
        layout.Controls.Add(_root, 0, 2);
        layout.Controls.Add(_browse, 1, 2);
        layout.Controls.Add(_files, 0, 3);
        layout.SetColumnSpan(_files, 3);
        layout.Controls.Add(_progress, 0, 4);
        layout.SetColumnSpan(_progress, 2);
        layout.Controls.Add(_flash, 2, 4);
        layout.Controls.Add(_status, 0, 5);
        layout.SetColumnSpan(_status, 3);

        var flashPage = new TabPage("Flash") { Padding = new Padding(4) };
        flashPage.Controls.Add(layout);

        var backend = new ConsoleYamlBackend(_root.Text);
        _console = new ConsoleTab(backend);
        _console.FlashRequested += OnFlashRequested;
        var consolePage = new TabPage("Console") { Padding = new Padding(4) };
        consolePage.Controls.Add(_console);

        // The play tab takes a list rather than the backend. It has no console
        // in it and no business holding one; the backend is simply the thing
        // that already knows how to read games/*/game.yml, and a second reader
        // would be a second place to keep that in step.
        _play = new PlayTab(_root.Text);
        _play.SetGames(backend.DiscoverGames(), _root.Text);
        var playPage = new TabPage("Play") { Padding = new Padding(4) };
        playPage.Controls.Add(_play);

        _tabs.Dock = DockStyle.Fill;
        _tabs.TabPages.Add(flashPage);
        _tabs.TabPages.Add(consolePage);
        _tabs.TabPages.Add(playPage);
        Controls.Add(_tabs);

        _watcher.DrivesChanged += OnDrivesChanged;
        _watcher.Start();
        ReloadFiles();
    }

    /// <summary>
    /// The console tab finished a build and wants it on the device. The file
    /// is new, so the list has to be rebuilt before it can be selected in it.
    /// </summary>
    private void OnFlashRequested(string path)
    {
        ReloadFiles();
        _tabs.SelectedIndex = 0;

        for (var i = 0; i < _files.Items.Count; i++)
        {
            if (_files.Items[i] is Uf2File file
                && string.Equals(file.Path, path, StringComparison.OrdinalIgnoreCase))
            {
                _files.SelectedIndex = i;
                break;
            }
        }

        if (_devices.SelectedItem is not BootselDrive)
        {
            _status.Text = "Built. Put the board in BOOTSEL (hold X, press power) " +
                           "and press Flash.";
            return;
        }
        DoFlash();
    }

    /// <summary>
    /// Windows broadcasts volume arrival and removal here. Cheaper and far more
    /// responsive than shortening the poll interval, and the poll stays as the
    /// backstop for anything the broadcast misses.
    /// </summary>
    protected override void WndProc(ref Message m)
    {
        if (m.Msg == WmDeviceChange) _watcher.NudgeSoon();
        base.WndProc(ref m);
    }

    private void OnDrivesChanged(IReadOnlyList<BootselDrive> drives)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => OnDrivesChanged(drives));
            return;
        }

        var previous = _devices.SelectedItem as BootselDrive;
        _devices.Items.Clear();
        foreach (var drive in drives) _devices.Items.Add(drive);

        if (drives.Count == 0)
        {
            _status.Text = "No board in BOOTSEL mode. Hold X and press power.";
        }
        else
        {
            var index = previous is null ? 0 : Math.Max(0, _devices.Items.IndexOf(previous));
            _devices.SelectedIndex = index;
            _status.Text = drives.Count == 1
                ? "Board ready."
                : $"{drives.Count} boards ready.";
        }

        UpdateFlashEnabled();
    }

    private void ReloadFiles()
    {
        _files.Items.Clear();
        foreach (var file in Uf2Locator.Find(_root.Text)) _files.Items.Add(file);
        if (_files.Items.Count > 0) _files.SelectedIndex = 0;
        UpdateFlashEnabled();
    }

    private void BrowseForFolder()
    {
        using var dialog = new FolderBrowserDialog { SelectedPath = _root.Text };
        if (dialog.ShowDialog(this) == DialogResult.OK) _root.Text = dialog.SelectedPath;
    }

    private void UpdateFlashEnabled()
    {
        _flash.Enabled = _devices.SelectedItem is BootselDrive
                         && _files.SelectedItem is Uf2File;
    }

    private void DoFlash()
    {
        if (_devices.SelectedItem is not BootselDrive drive) return;
        if (_files.SelectedItem is not Uf2File file) return;

        if (!BoardsAgree(file, drive)) return;

        _flash.Enabled = false;
        _progress.Value = 0;
        _status.Text = $"Flashing {Path.GetFileName(file.Path)}...";

        var progress = new Progress<int>(percent =>
            _progress.Value = Math.Clamp(percent, 0, 100));

        // Off the UI thread so the window keeps painting during the copy.
        Task.Run(() => Uf2Flasher.Flash(file.Path, drive.Root, progress))
            .ContinueWith(task =>
            {
                var result = task.Result;
                _progress.Value = result.Success ? 100 : 0;
                _status.Text = result.Message;
                // The board has rebooted out of BOOTSEL, so the drive is gone.
                _watcher.Refresh();
                UpdateFlashEnabled();
            }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    /// <summary>
    /// Refuses a .uf2 built for the other board, before it is copied.
    ///
    /// The hardware will not tell you: the bootrom checks each block's family
    /// id and silently ignores the ones that do not match, so flashing a
    /// PicoSystem build at a Tufty copies the file, reports success, reboots,
    /// and runs whatever was there before. Both boards now produce a
    /// console.uf2 and a catcoin.uf2, so the two are one mis-click apart, and
    /// "nothing happened" is the least debuggable failure there is.
    ///
    /// Only refuses when both sides are known. An unrecognised drive or a file
    /// whose family belongs to neither board goes through as it always did:
    /// this is here to catch a specific confusion, not to become a gatekeeper
    /// for every .uf2 the world contains.
    /// </summary>
    private bool BoardsAgree(Uf2File file, BootselDrive drive)
    {
        var forBoard = Uf2Family.Identify(file.Path);
        var isBoard = BoardSpec.ForDriveLabel(drive.Label);
        if (forBoard is null || isBoard is null) return true;
        if (forBoard.Board == isBoard.Board) return true;

        MessageBox.Show(this,
            $"{Path.GetFileName(file.Path)} is a {forBoard.Name} build "
            + $"({forBoard.Chip}), and the board plugged in is a {isBoard.Name} "
            + $"({isBoard.Chip}).\n\n"
            + "Flashing it would appear to work and do nothing: the bootloader "
            + "ignores blocks that are not for its own chip.\n\n"
            + $"Build it with {isBoard.ConsoleScript} for the console, or "
            + $"build_uf2s{(isBoard.Board == TargetBoard.Tufty2350 ? "_tufty" : "")}.bat "
            + "for a single game.",
            "Wrong board", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        _status.Text = $"Not flashed: that file is for the {forBoard.Name}.";
        return false;
    }

    /// <summary>Walks up from the executable looking for the repo, so the file
    /// list is already populated when the tool opens from a build tree.</summary>
    private static string GuessRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (Directory.Exists(Path.Combine(directory.FullName, "games"))
                && File.Exists(Path.Combine(directory.FullName, "CMakeLists.txt")))
            {
                return directory.FullName;
            }
            directory = directory.Parent;
        }
        return Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
    }

    /// <summary>An edited menu should survive closing the window without a build.</summary>
    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        _console?.SaveQuietly();
        // A game launched from the Play tab is a child of this process. Left
        // behind it would be a window with no way back to the tool that
        // started it.
        _play?.StopIfRunning();
        base.OnFormClosing(e);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _watcher.Dispose();
        base.Dispose(disposing);
    }
}
