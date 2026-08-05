using System.Text.Json;

namespace PicomonEditor;

/// <summary>
/// The little the app remembers between runs: where the data is, which zone was
/// open, how far in the map was zoomed.
///
/// Under AppData rather than next to the data, which is the opposite of where
/// the flasher keeps its bundle: that file describes a particular library and
/// belongs to it, this one describes an install of the tool. A settings file
/// written into games/picomon/data would be a file in the repo that no build
/// reads and every checkout would carry.
/// </summary>
public sealed class Settings
{
    public string DataDirectory { get; set; } = "";
    public string LastZone { get; set; } = "";
    public int Zoom { get; set; } = 16;

    private static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "PicomonEditor", "settings.json");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new Settings();
        }
        catch (IOException) { }
        catch (JsonException) { }
        catch (UnauthorizedAccessException) { }
        return new Settings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath,
                JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    /// <summary>Walks up from the executable looking for the data directory, so
    /// the editor opens on the maps when it is run from inside the repo. The
    /// remembered path wins when it is still there, because it was chosen on
    /// purpose and this is a guess.</summary>
    public static string GuessDataDirectory()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var candidate = Path.Combine(directory.FullName, "games", "picomon", "data");
            if (Dataset.LooksLikeDataDirectory(candidate)) return candidate;
            directory = directory.Parent;
        }
        return "";
    }
}
