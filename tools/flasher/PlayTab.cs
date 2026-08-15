using System.Diagnostics;

namespace PicoFlasher;

/// <summary>
/// Build a game for this PC and run it, without a device.
///
/// The desktop build is the fastest way to look at a change: the 32blit SDK
/// targets SDL as well as the boards, so the same sources that make a .uf2
/// make an .exe, and there is no flashing step between an edit and seeing it.
/// The repo has had this all along as run_&lt;slug&gt;.bat, one wrapper per game,
/// which means it is discoverable only if you already know it exists.
///
/// This shells out to tools\run_game.bat rather than driving CMake itself, for
/// the same reason the Console tab shells out to build_console.bat: the script
/// already carries the compiler checks, the CMake policy quoting that
/// PowerShell mangles, the SDL2 DLL copy, and the --size 240,240 the SDL
/// backend needs to match the board. Two ways to build the same thing is two
/// ways for them to drift (rule 2).
///
/// Note the script LAUNCHES the game and waits for it, so this stays "running"
/// until the game window is closed. That is the honest thing to show: there is
/// one process and it is the game.
/// </summary>
public sealed class PlayTab : UserControl
{
    private readonly ListView _games = new();
    private readonly ImageList _icons = new();
    private readonly Button _play = new();
    private readonly TextBox _log = new();
    private readonly Label _status = new();

    private string _repositoryRoot;
    private bool _busy;
    private Process? _running;

    public PlayTab(string repositoryRoot)
    {
        _repositoryRoot = repositoryRoot;

        Dock = DockStyle.Fill;
        Padding = new Padding(10);

        _icons.ImageSize = new Size(GameIcon.Size, GameIcon.Size);
        _icons.ColorDepth = ColorDepth.Depth32Bit;

        _games.Dock = DockStyle.Fill;
        _games.View = View.LargeIcon;
        _games.MultiSelect = false;
        _games.HideSelection = false;
        _games.LargeImageList = _icons;
        _games.DoubleClick += (_, _) => Play();
        _games.SelectedIndexChanged += (_, _) => UpdateButtons();

        _play.Text = "Build and play";
        _play.AutoSize = true;
        _play.Height = 32;
        _play.Click += (_, _) => Play();

        _log.Dock = DockStyle.Fill;
        _log.Multiline = true;
        _log.ReadOnly = true;
        _log.ScrollBars = ScrollBars.Vertical;
        _log.Font = new Font("Consolas", 8F);
        _log.Height = 120;

        _status.Dock = DockStyle.Fill;
        _status.AutoSize = true;

        Controls.Add(BuildLayout());
        UpdateButtons();
    }

    private Control BuildLayout()
    {
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 4,
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        layout.Controls.Add(_games, 0, 0);

        var buttons = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            Dock = DockStyle.Fill,
            AutoSize = true,
        };
        buttons.Controls.Add(_play);
        layout.Controls.Add(buttons, 0, 1);

        layout.Controls.Add(_status, 0, 2);
        layout.Controls.Add(_log, 0, 3);
        return layout;
    }

    /// <summary>The games to offer, discovered by whoever already reads them.</summary>
    public void SetGames(IReadOnlyList<AvailableGame> games, string repositoryRoot)
    {
        _repositoryRoot = repositoryRoot;

        _icons.Images.Clear();
        _games.Items.Clear();
        foreach (var game in games)
        {
            _icons.Images.Add(GameIcon.Load(game.ThumbnailPath));
            _games.Items.Add(new ListViewItem(game.Title)
            {
                Tag = game,
                ImageIndex = _icons.Images.Count - 1,
                ToolTipText = string.IsNullOrWhiteSpace(game.Blurb)
                    ? game.Slug
                    : $"{game.Slug}\n{game.Blurb}",
            });
        }
        UpdateButtons();
    }

    private string RunScript =>
        Path.Combine(_repositoryRoot, "tools", "run_game.bat");

    private void UpdateButtons()
    {
        _play.Enabled = !_busy
                        && _games.SelectedItems.Count == 1
                        && File.Exists(RunScript);

        if (_busy) return;

        if (!File.Exists(RunScript))
        {
            _status.ForeColor = Color.FromArgb(168, 32, 32);
            _status.Text = "This is not a checkout of the repository, so there "
                           + "is nothing to build from. Point the folder box on "
                           + "the Flash tab at one.";
        }
        else if (_games.Items.Count == 0)
        {
            _status.ForeColor = SystemColors.ControlText;
            _status.Text = "No games found under games/.";
        }
        else
        {
            _status.ForeColor = SystemColors.ControlText;
            _status.Text = _games.SelectedItems.Count == 1
                ? "Builds for this PC and runs it. No device needed."
                : "Pick a game.";
        }
    }

    private async void Play()
    {
        if (_busy) return;
        if (_games.SelectedItems.Count != 1) return;
        if (_games.SelectedItems[0].Tag is not AvailableGame game) return;
        if (!File.Exists(RunScript)) return;

        _busy = true;
        _log.Clear();
        _status.ForeColor = SystemColors.ControlText;
        _status.Text = $"Building {game.Title}...";
        UpdateButtons();

        var log = new Progress<string>(line =>
        {
            _log.AppendText(line + Environment.NewLine);
            // tools\run_game.bat prints "Running <exe>..." immediately before
            // it starts the game, and that is the moment this stops being a
            // build and starts being a game. Matched against what the script
            // actually says, not what it seemed likely to say.
            if (line.StartsWith("Running ", StringComparison.OrdinalIgnoreCase))
            {
                _status.Text = $"{game.Title} is running. Close its window to finish.";
            }
        });

        var message = await RunAsync(game, log).ConfigureAwait(true);

        _busy = false;
        _running = null;
        _status.ForeColor = message.StartsWith("Could not", StringComparison.Ordinal)
                            || message.Contains("failed", StringComparison.OrdinalIgnoreCase)
            ? Color.FromArgb(168, 32, 32)
            : SystemColors.ControlText;
        _status.Text = message;
        UpdateButtons();
    }

    private async Task<string> RunAsync(AvailableGame game, IProgress<string> log)
    {
        var start = new ProcessStartInfo
        {
            FileName = "cmd.exe",
            // /c with the script quoted: the repository path can contain spaces.
            Arguments = $"/c \"\"{RunScript}\" {game.Slug}\"",
            WorkingDirectory = _repositoryRoot,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        try
        {
            using var process = new Process { StartInfo = start };
            process.OutputDataReceived += (_, e) =>
            {
                if (e.Data is not null) log.Report(e.Data);
            };
            process.ErrorDataReceived += (_, e) =>
            {
                if (e.Data is not null) log.Report(e.Data);
            };

            process.Start();
            _running = process;
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync().ConfigureAwait(true);

            return process.ExitCode == 0
                ? $"{game.Title} closed."
                : $"The build failed (exit code {process.ExitCode}). The log "
                  + "above says why.";
        }
        catch (Exception error)
        {
            return $"Could not run the build: {error.Message}";
        }
    }

    /// <summary>
    /// Closing the window while a game is running would leave it with no
    /// parent and no way to be found again, so take it with us.
    /// </summary>
    public void StopIfRunning()
    {
        try
        {
            if (_running is { HasExited: false }) _running.Kill(entireProcessTree: true);
        }
        catch (InvalidOperationException) { }
        catch (System.ComponentModel.Win32Exception) { }
        catch (NotSupportedException) { }
    }
}
