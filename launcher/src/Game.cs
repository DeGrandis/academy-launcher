using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace AcademyLauncher
{
    // The player's game files and everything derived from them.
    public static class Game
    {
        // default.xbe of the supported release (NTSC-U). The runtime patches code at fixed addresses, so other
        // releases cannot work.
        static readonly string[] SupportedXbe = { "b795d697836c061307df4d220ed2effc8ebc088e585c78278ca7c04bd5be0229" };

        public static string GameDir(Settings settings)
        {
            return settings.Get("game_dir", AppPaths.GameDir);
        }

        public static bool IsReady(Settings settings)
        {
            string dir = GameDir(settings);
            return File.Exists(Path.Combine(dir, "default.xbe")) && File.Exists(Path.Combine(dir, "data.zwp"));
        }

        public static string Sha256(string path)
        {
            using (var sha = SHA256.Create())
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            {
                return ModBuilder.ToHex(sha.ComputeHash(stream));
            }
        }

        // Throws with a player-facing message when the folder is not the supported game.
        public static void Verify(string dir)
        {
            string xbe = Path.Combine(dir, "default.xbe");
            if (!File.Exists(xbe) || !File.Exists(Path.Combine(dir, "data.zwp")))
            {
                throw new InvalidDataException("That folder does not contain the game (default.xbe and data.zwp are missing).");
            }
            if (Array.IndexOf(SupportedXbe, Sha256(xbe)) < 0)
            {
                throw new InvalidDataException("This copy of the game is a different release or region (default.xbe does not match). "
                    + "Academy Launcher supports the North American (NTSC-U) release of Star Wars: The Clone Wars for Xbox.");
            }
            using (var stream = File.OpenRead(Path.Combine(dir, "data.zwp")))
            {
                Zwp.ReadDirectory(stream);
            }
        }

        // Extracts an ISO into the launcher's game folder (via a temporary folder, so a cancelled run leaves nothing).
        public static void ImportIso(string iso, Settings settings, Action<long, long> progress, CancellationToken cancel)
        {
            string target = AppPaths.GameDir;
            string temporary = target + ".partial";
            if (Directory.Exists(temporary)) Directory.Delete(temporary, true);
            using (var image = new Xiso(iso))
            {
                image.ExtractAll(temporary, progress, cancel);
            }
            Verify(temporary);
            if (Directory.Exists(target)) Directory.Delete(target, true);
            Directory.Move(temporary, target);
            settings.Set("game_dir", "");
            settings.Save();
            AppPaths.Log("imported " + iso);
        }

        public static void UseFolder(string dir, Settings settings)
        {
            Verify(dir);
            settings.Set("game_dir", Path.GetFullPath(dir));
            settings.Save();
            AppPaths.Log("using game folder " + dir);
        }

        // play\: cw_runtime.dll (refreshed when the launcher updates), clone_wars.exe converted from the player's
        // default.xbe (redone when the runtime or the game changes), game_root.txt.
        public static void PreparePlay(Settings settings, Action<string> log)
        {
            string play = AppPaths.PlayDir;
            Directory.CreateDirectory(play);
            string runtime = Path.Combine(AppPaths.RuntimeDir, "cw_runtime.dll");
            string converter = Path.Combine(AppPaths.RuntimeDir, "xbe2exe.exe");
            string xbe = Path.Combine(GameDir(settings), "default.xbe");
            string stamp = Sha256(runtime) + " " + Sha256(converter) + " " + Sha256(xbe);
            string stampFile = Path.Combine(play, "prepared.txt");
            string exe = Path.Combine(play, "clone_wars.exe");
            if (File.Exists(exe) && File.Exists(stampFile) && File.ReadAllText(stampFile) == stamp)
            {
                return;
            }
            log("Preparing the game...");
            File.Copy(runtime, Path.Combine(play, "cw_runtime.dll"), true);
            var start = new ProcessStartInfo(converter, Quote(xbe) + " " + Quote(exe))
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
            };
            using (Process process = Process.Start(start))
            {
                string output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
                process.WaitForExit();
                if (process.ExitCode != 0)
                {
                    throw new InvalidOperationException("Converting the game failed: " + output.Trim());
                }
            }
            File.WriteAllText(Path.Combine(play, "game_root.txt"), GameDir(settings) + "\n", new UTF8Encoding(false));
            File.WriteAllText(stampFile, stamp);
            AppPaths.Log("prepared play folder");
        }

        static string Quote(string path)
        {
            return "\"" + path + "\"";
        }

        // Returns the preset's mod root (built when missing or out of date), or null for the unmodified game.
        public static string EnsureModRoot(Preset preset, Settings settings, Action<string> log)
        {
            if (preset.Mods.Count == 0)
            {
                return null;
            }
            string root = Path.Combine(AppPaths.ModRootsDir, preset.Id);
            string game = GameDir(settings);
            var zwp = new FileInfo(Path.Combine(game, "data.zwp"));
            string stamp = ModBuilder.Fingerprint(preset) + " " + zwp.Length + " " + zwp.LastWriteTimeUtc.Ticks;
            string stampFile = Path.Combine(root, "built.txt");
            if (File.Exists(stampFile) && File.ReadAllText(stampFile) == stamp)
            {
                return root;
            }
            log("Building mods for " + preset.Name + " (first time only)...");
            ModBuilder.Build(preset, game, root, message => AppPaths.Log(preset.Id + ": " + message));
            File.WriteAllText(stampFile, stamp);
            AppPaths.Log("built mod root " + root);
            return root;
        }

        // The player's display and audio settings as runtime variables.
        public static Dictionary<string, string> RuntimeOptions(Settings settings)
        {
            var options = new Dictionary<string, string>();
            string resolution = settings.Get("resolution", "window");
            if (resolution.StartsWith("x", StringComparison.Ordinal)) options["CW_RENDER_SCALE"] = resolution.Substring(1);
            else options["CW_RESOLUTION"] = resolution;
            options["CW_FULLSCREEN"] = settings.GetBool("fullscreen", false) ? "1" : "0";
            options["CW_WIDESCREEN"] = settings.GetBool("widescreen", true) ? "1" : "0";
            options["CW_VIEW_DISTANCE"] = settings.Get("view_distance", "4");
            options["CW_CAMERA_DISTANCE"] = settings.Get("camera_distance", "1.2");
            options["CW_AUDIO_VOLUME"] = settings.Get("volume", "1");
            return options;
        }

        public static Process Launch(Settings settings, string modRoot, IDictionary<string, string> extra)
        {
            string play = AppPaths.PlayDir;
            var start = new ProcessStartInfo(Path.Combine(play, "clone_wars.exe"))
            {
                UseShellExecute = false,
                WorkingDirectory = play,
            };
            // Start clean: variables from a developer's shell must not leak into a player's game.
            foreach (string key in new List<string>(EnvironmentKeys(start)))
            {
                if (key.StartsWith("CW_", StringComparison.OrdinalIgnoreCase)) start.EnvironmentVariables.Remove(key);
            }
            foreach (var pair in RuntimeOptions(settings)) start.EnvironmentVariables[pair.Key] = pair.Value;
            if (modRoot != null) start.EnvironmentVariables["CW_MOD_ROOT"] = modRoot;
            start.EnvironmentVariables["CW_LOG_PATH"] = Path.Combine(play, "cw_runtime.log");
            foreach (var pair in extra) start.EnvironmentVariables[pair.Key] = pair.Value;
            AppPaths.Log("launch: mods " + (modRoot ?? "none") + (extra.ContainsKey("CW_NET") ? ", network" : ""));
            return Process.Start(start);
        }

        static IEnumerable<string> EnvironmentKeys(ProcessStartInfo start)
        {
            foreach (string key in start.EnvironmentVariables.Keys) yield return key;
        }
    }
}
