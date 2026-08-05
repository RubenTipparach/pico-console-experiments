namespace PicoFlasher;

/// <summary>
/// One window, two tabs: flash a single .uf2, or build a library onto the
/// console.
///
/// Quick and dirty on purpose. No installer, no settings, no framework. This is
/// a tool, not a product, and every feature added here is a feature that has to
/// keep working.
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
    private readonly LibraryTab _library;

    public MainForm()
    {
        Text = "PicoFlasher";
        Width = 820;
        Height = 560;
        MinimumSize = new Size(680, 460);
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
        _root.TextChanged += (_, _) => ReloadFiles();

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

        var flashPage = new TabPage("Flash one game");
        flashPage.Controls.Add(layout);

        _library = new LibraryTab(GuessRepositoryRoot(),
                                  () => _devices.SelectedItem as BootselDrive);
        var libraryPage = new TabPage("Library");
        libraryPage.Controls.Add(_library);

        _tabs.Dock = DockStyle.Fill;
        _tabs.TabPages.Add(flashPage);
        _tabs.TabPages.Add(libraryPage);
        Controls.Add(_tabs);

        _watcher.DrivesChanged += OnDrivesChanged;
        _watcher.Start();
        ReloadFiles();
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
        _library.DeviceChanged(drives.Count);
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

    protected override void Dispose(bool disposing)
    {
        if (disposing) _watcher.Dispose();
        base.Dispose(disposing);
    }
}
