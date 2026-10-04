using System;
using System.IO;

namespace AcademyLauncher
{
    // Where things are. An installed launcher keeps its read-only files next to the exe:
    //   runtime\cw_runtime.dll, runtime\xbe2exe.exe   (built from native_port)
    //   mods\<name>\{data, edits.json, files, plugin\<name>.dll}, mods\presets.json
    //   VERSION, defaults.ini
    // Run from the repository (launcher\out), it uses native_port\build-x86\bin and mods\ instead.
    // Everything the player's machine produces goes under %LOCALAPPDATA%\AcademyLauncher (ACADEMY_DATA overrides):
    //   game\         the extracted game disc (or settings game_dir points at the player's own extracted copy)
    //   play\         clone_wars.exe (converted from the player's default.xbe), cw_runtime.dll, hdd\ (saves)
    //   modroots\<preset>\   built mod overlays
//   addons\<name>\      extra mods the player adds (layered on every modded preset)
    //   settings.ini, launcher.log
    public static class AppPaths
    {
        public static readonly string InstallDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
        public static readonly string RuntimeDir;
        public static readonly string ModsDir;
        public static readonly string PluginFallbackDir;  // repository build: plugin DLLs are in native_port\build-x86\bin\mods
        public static readonly string VersionFile;
        public static readonly string DefaultsFile;
        public static readonly bool FromRepository;

        public static readonly string DataDir;
        public static string GameDir { get { return Path.Combine(DataDir, "game"); } }
        public static string PlayDir { get { return Path.Combine(DataDir, "play"); } }
        public static string ModRootsDir { get { return Path.Combine(DataDir, "modroots"); } }
        public static string AddonsDir { get { return Path.Combine(DataDir, "addons"); } }
        public static string SettingsFile { get { return Path.Combine(DataDir, "settings.ini"); } }
        public static string LogFile { get { return Path.Combine(DataDir, "launcher.log"); } }

        static AppPaths()
        {
            if (Directory.Exists(Path.Combine(InstallDir, "runtime")))
            {
                RuntimeDir = Path.Combine(InstallDir, "runtime");
                ModsDir = Path.Combine(InstallDir, "mods");
                VersionFile = Path.Combine(InstallDir, "VERSION");
                DefaultsFile = Path.Combine(InstallDir, "defaults.ini");
            }
            else
            {
                string repo = FindRepository(InstallDir);
                if (repo == null)
                {
                    throw new InvalidOperationException("Installation is incomplete: " + Path.Combine(InstallDir, "runtime") + " is missing. Reinstall Academy Launcher.");
                }
                FromRepository = true;
                RuntimeDir = Path.Combine(repo, "native_port", "build-x86", "bin");
                PluginFallbackDir = Path.Combine(RuntimeDir, "mods");
                ModsDir = Path.Combine(repo, "mods");
                VersionFile = Path.Combine(repo, "VERSION");
                DefaultsFile = Path.Combine(repo, "launcher", "defaults.ini");
            }
            string data = Environment.GetEnvironmentVariable("ACADEMY_DATA");
            DataDir = !string.IsNullOrEmpty(data) ? Path.GetFullPath(data)
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AcademyLauncher");
            Directory.CreateDirectory(DataDir);
        }

        static string FindRepository(string start)
        {
            for (DirectoryInfo directory = new DirectoryInfo(start); directory != null; directory = directory.Parent)
            {
                if (File.Exists(Path.Combine(directory.FullName, "VERSION")) && Directory.Exists(Path.Combine(directory.FullName, "native_port")))
                {
                    return directory.FullName;
                }
            }
            return null;
        }

        public static string Version
        {
            get
            {
                try { return File.ReadAllText(VersionFile).Trim(); }
                catch (IOException) { return "0.0.0"; }
            }
        }

        public static void Log(string message)
        {
            try { File.AppendAllText(LogFile, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " " + message + Environment.NewLine); }
            catch (IOException) { }
        }
    }
}
