using System.Diagnostics.CodeAnalysis;

namespace PicoFlasher;

/// <summary>An RP2040 or RP2350 sitting in its bootloader, mounted as a drive.</summary>
public sealed record BootselDrive(string Root, string Label, string Model)
{
    public override string ToString() =>
        string.IsNullOrEmpty(Model) ? $"{Root}  ({Label})" : $"{Root}  {Model}";
}

/// <summary>
/// Finds boards in BOOTSEL mode and raises an event when the set changes.
///
/// Detection is a 1 second poll, nudged by WM_DEVICECHANGE so plugging a board
/// in feels instant. FileSystemWatcher is the wrong tool for this twice over:
/// it cannot see a drive appear (there is no parent directory above E:\ to
/// watch), and rooting one on the volume holds a handle open on a device that
/// is about to yank itself off the bus mid write.
/// </summary>
public sealed class BootselWatcher : IDisposable
{
    // The volume labels the RP2040 and RP2350 bootroms present.
    private static readonly string[] KnownLabels = { "RPI-RP2", "RP2350" };

    private readonly System.Windows.Forms.Timer _timer = new() { Interval = 1000 };
    private IReadOnlyList<BootselDrive> _current = Array.Empty<BootselDrive>();

    public event Action<IReadOnlyList<BootselDrive>>? DrivesChanged;

    public IReadOnlyList<BootselDrive> Drives => _current;

    public BootselWatcher()
    {
        _timer.Tick += (_, _) => Refresh();
    }

    public void Start()
    {
        Refresh();
        _timer.Start();
    }

    /// <summary>Called from the form's WndProc on WM_DEVICECHANGE.</summary>
    public void NudgeSoon()
    {
        // Windows broadcasts the arrival slightly before the volume is
        // mountable, so re-poll shortly after rather than immediately.
        var once = new System.Windows.Forms.Timer { Interval = 400 };
        once.Tick += (_, _) =>
        {
            once.Stop();
            once.Dispose();
            Refresh();
        };
        once.Start();
    }

    public void Refresh()
    {
        var found = Scan();
        if (SameAs(found, _current)) return;
        _current = found;
        DrivesChanged?.Invoke(found);
    }

    private static List<BootselDrive> Scan()
    {
        var results = new List<BootselDrive>();

        foreach (var drive in SafeGetDrives())
        {
            // IsReady is not a guard against exceptions, it is a hint. Microsoft
            // documents the race explicitly, and this is exactly the case it
            // warns about: the drive can vanish between the check and the read.
            if (!TryDescribe(drive, out var described)) continue;
            results.Add(described);
        }

        return results;
    }

    private static DriveInfo[] SafeGetDrives()
    {
        try
        {
            return DriveInfo.GetDrives();
        }
        catch (IOException)
        {
            return Array.Empty<DriveInfo>();
        }
    }

    private static bool TryDescribe(DriveInfo drive,
                                    [NotNullWhen(true)] out BootselDrive? result)
    {
        result = null;
        try
        {
            if (!drive.IsReady) return false;
            if (drive.DriveType != DriveType.Removable) return false;

            var label = drive.VolumeLabel;
            var isKnownLabel = KnownLabels.Any(
                known => string.Equals(label, known, StringComparison.OrdinalIgnoreCase));

            var info = Path.Combine(drive.RootDirectory.FullName, "INFO_UF2.TXT");
            if (!isKnownLabel && !File.Exists(info)) return false;

            // INFO_UF2.TXT names the actual board, which is friendlier than a
            // drive letter when two are plugged in.
            var model = string.Empty;
            if (File.Exists(info))
            {
                foreach (var line in File.ReadAllLines(info))
                {
                    if (line.StartsWith("Model:", StringComparison.OrdinalIgnoreCase))
                    {
                        model = line["Model:".Length..].Trim();
                        break;
                    }
                }
            }

            result = new BootselDrive(drive.RootDirectory.FullName, label, model);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static bool SameAs(IReadOnlyList<BootselDrive> a,
                               IReadOnlyList<BootselDrive> b)
    {
        if (a.Count != b.Count) return false;
        for (var i = 0; i < a.Count; i++)
        {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    public void Dispose()
    {
        _timer.Stop();
        _timer.Dispose();
    }
}
