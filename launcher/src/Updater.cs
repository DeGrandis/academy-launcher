using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Text;
using System.Web.Script.Serialization;

namespace AcademyLauncher
{
    // Updates come from GitHub Releases of the repository named by update_repo (defaults.ini). A release carries
    // AcademyLauncherSetup-<version>.exe and SHA256SUMS.txt; updating downloads the installer, checks its hash, runs
    // it silently (it closes the launcher, replaces the files and starts the new launcher) and leaves the player's
    // game files, saves and settings alone. The stable channel takes the newest full release, beta also takes
    // pre-releases.
    public static class Updater
    {
        public sealed class Release
        {
            public string Version;
            public string Notes;
            public string InstallerUrl;
            public string InstallerName;
            public string ChecksumsUrl;
            public string PageUrl;
        }

        static Updater()
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
        }

        static WebClient Client()
        {
            var client = new WebClient();
            client.Headers[HttpRequestHeader.UserAgent] = "AcademyLauncher/" + AppPaths.Version;
            return client;
        }

        // Returns a newer release, or null. Throws on network errors.
        public static Release Check(Settings settings)
        {
            string repo = settings.Get("update_repo");
            if (repo.Length == 0 || !settings.GetBool("check_updates", true))
            {
                return null;
            }
            // A pre-release build follows the beta channel, so testers get the next beta (and then the release).
            bool beta = settings.Get("update_channel", "stable") == "beta" || AppPaths.Version.Contains("-");
            string url = "https://api.github.com/repos/" + repo + "/releases" + (beta ? "?per_page=10" : "/latest");
            string json;
            using (WebClient client = Client())
            {
                client.Headers[HttpRequestHeader.Accept] = "application/vnd.github+json";
                json = client.DownloadString(url);
            }
            var serializer = new JavaScriptSerializer();
            var releases = new List<Dictionary<string, object>>();
            object parsed = serializer.DeserializeObject(json);
            if (parsed is Dictionary<string, object>) releases.Add((Dictionary<string, object>)parsed);
            else foreach (Dictionary<string, object> item in (IEnumerable)parsed) releases.Add(item);
            foreach (Dictionary<string, object> release in releases)
            {
                if ((bool)release["draft"] || ((bool)release["prerelease"] && !beta)) continue;
                string version = ((string)release["tag_name"]).TrimStart('v', 'V');
                if (!IsNewer(version, AppPaths.Version)) return null;
                var result = new Release
                {
                    Version = version,
                    Notes = release["body"] as string ?? "",
                    PageUrl = release["html_url"] as string ?? "",
                };
                foreach (Dictionary<string, object> asset in (IEnumerable)release["assets"])
                {
                    string name = (string)asset["name"];
                    string download = (string)asset["browser_download_url"];
                    if (name.StartsWith("AcademyLauncherSetup", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                    {
                        result.InstallerUrl = download;
                        result.InstallerName = name;
                    }
                    else if (name.Equals("SHA256SUMS.txt", StringComparison.OrdinalIgnoreCase))
                    {
                        result.ChecksumsUrl = download;
                    }
                }
                return result.InstallerUrl != null && result.ChecksumsUrl != null ? result : null;
            }
            return null;
        }

        public static bool IsNewer(string candidate, string current)
        {
            Version a, b;
            string coreA = candidate.Split('-')[0], coreB = current.Split('-')[0];
            if (!System.Version.TryParse(coreA, out a) || !System.Version.TryParse(coreB, out b)) return false;
            if (a != b) return a > b;
            // 1.2.0 is newer than 1.2.0-beta.1
            return !candidate.Contains("-") && current.Contains("-");
        }

        // Downloads and verifies the installer, then starts it. The caller exits right after.
        public static void Install(Release release, Action<int> progress)
        {
            string folder = Path.Combine(Path.GetTempPath(), "AcademyLauncherUpdate");
            Directory.CreateDirectory(folder);
            string installer = Path.Combine(folder, release.InstallerName);
            string sums;
            using (WebClient client = Client())
            {
                sums = client.DownloadString(release.ChecksumsUrl);
                client.DownloadProgressChanged += (sender, e) => progress(e.ProgressPercentage);
                var task = client.DownloadFileTaskAsync(new Uri(release.InstallerUrl), installer);
                task.Wait();
            }
            string expected = null;
            foreach (string line in sums.Split('\n'))
            {
                string[] parts = line.Trim().Split(new[] { ' ', '*' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length == 2 && parts[1].Equals(release.InstallerName, StringComparison.OrdinalIgnoreCase)) expected = parts[0].ToLowerInvariant();
            }
            if (expected == null || Game.Sha256(installer) != expected)
            {
                File.Delete(installer);
                throw new InvalidDataException("The downloaded update is damaged (checksum mismatch). Try again later.");
            }
            AppPaths.Log("installing update " + release.Version);
            Process.Start(new ProcessStartInfo(installer, "/SILENT /SP- /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /UPDATE=1")
            {
                UseShellExecute = true,
            });
        }
    }
}
