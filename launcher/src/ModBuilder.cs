using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace AcademyLauncher
{
    public sealed class Preset
    {
        public string Id;
        public string Name;
        public string Description;
        public bool Online;
        public List<string> Mods = new List<string>();

        public static List<Preset> LoadAll()
        {
            string file = Path.Combine(AppPaths.ModsDir, "presets.json");
            var root = (Dictionary<string, object>)new JavaScriptSerializer().DeserializeObject(File.ReadAllText(file, Encoding.UTF8));
            var presets = new List<Preset>();
            foreach (Dictionary<string, object> item in (IEnumerable)root["presets"])
            {
                var preset = new Preset
                {
                    Id = (string)item["id"],
                    Name = (string)item["name"],
                    Description = item.ContainsKey("description") ? (string)item["description"] : "",
                    Online = item.ContainsKey("online") && (bool)item["online"],
                };
                foreach (object mod in (IEnumerable)item["mods"]) preset.Mods.Add((string)mod);
                presets.Add(preset);
            }
            return presets;
        }
    }

    // Builds a mod overlay folder (CW_MOD_ROOT) from mods\<name>\ folders, like tools/build_mod.py:
    //   data\       files that replace or add data.zwp entries by file name
    //   edits.json  regex edits of data.zwp entries ({"file", "find", "replace", "count", "from"}; Python re syntax)
    //   files\      loose files overlaid on the game disc
    //   plugin\     <name>.dll, copied to plugins\
    // Mods apply in order; later ones win.
    public static class ModBuilder
    {
        static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

        public static string ModDir(string name)
        {
            return Path.Combine(AppPaths.ModsDir, name);
        }

        static string PluginDll(string name)
        {
            string shipped = Path.Combine(ModDir(name), "plugin", name + ".dll");
            if (File.Exists(shipped)) return shipped;
            if (AppPaths.PluginFallbackDir != null)
            {
                string built = Path.Combine(AppPaths.PluginFallbackDir, name + ".dll");
                if (File.Exists(built)) return built;
            }
            throw new FileNotFoundException("Mod '" + name + "' has a plugin, but " + name + ".dll is missing.");
        }

        // SHA-256 over the launcher version and every input of the mods (in order): the network build fingerprint and
        // the "is this mod root up to date" stamp. Plugin sources are skipped; the DLL that would be used counts.
        public static string Fingerprint(Preset preset)
        {
            using (var sha = SHA256.Create())
            {
                var all = new MemoryStream();
                Action<string> text = value => { byte[] bytes = Encoding.UTF8.GetBytes(value + "\n"); all.Write(bytes, 0, bytes.Length); };
                Action<string> file = path => { byte[] bytes = sha.ComputeHash(File.ReadAllBytes(path)); all.Write(bytes, 0, bytes.Length); };
                text("academy " + AppPaths.Version);
                foreach (string name in preset.Mods)
                {
                    string dir = ModDir(name);
                    if (!Directory.Exists(dir)) throw new DirectoryNotFoundException("Mod '" + name + "' is not installed.");
                    text("mod " + name);
                    var files = new List<string>();
                    foreach (string sub in new[] { "data", "files" })
                    {
                        if (Directory.Exists(Path.Combine(dir, sub))) files.AddRange(Directory.GetFiles(Path.Combine(dir, sub), "*", SearchOption.AllDirectories));
                    }
                    if (File.Exists(Path.Combine(dir, "edits.json"))) files.Add(Path.Combine(dir, "edits.json"));
                    files.Sort(StringComparer.OrdinalIgnoreCase);
                    foreach (string path in files)
                    {
                        text(path.Substring(dir.Length + 1).Replace('\\', '/').ToLowerInvariant());
                        file(path);
                    }
                    if (Directory.Exists(Path.Combine(dir, "plugin")))
                    {
                        text("plugin");
                        file(PluginDll(name));
                    }
                }
                return ToHex(sha.ComputeHash(all.ToArray()));
            }
        }

        public static string ToHex(byte[] bytes)
        {
            var builder = new StringBuilder(bytes.Length * 2);
            foreach (byte value in bytes) builder.Append(value.ToString("x2"));
            return builder.ToString();
        }

        // Builds into `output` (replaced). `gameDir` holds the original data.zwp.
        public static void Build(Preset preset, string gameDir, string output, Action<string> log)
        {
            if (Directory.Exists(output)) Directory.Delete(output, true);
            Directory.CreateDirectory(output);
            // Staged data.zwp entries: lower-case name -> (name as first written, content).
            var staging = new Dictionary<string, KeyValuePair<string, byte[]>>(StringComparer.OrdinalIgnoreCase);
            Archive archive = null;
            try
            {
                foreach (string name in preset.Mods)
                {
                    string dir = ModDir(name);
                    log("mod " + name);
                    string data = Path.Combine(dir, "data");
                    if (Directory.Exists(data))
                    {
                        foreach (string file in Directory.GetFiles(data, "*", SearchOption.AllDirectories))
                        {
                            Stage(staging, Path.GetFileName(file), File.ReadAllBytes(file));
                        }
                    }
                    string edits = Path.Combine(dir, "edits.json");
                    if (File.Exists(edits))
                    {
                        if (archive == null) archive = new Archive(Path.Combine(gameDir, "data.zwp"));
                        ApplyEdits(name, File.ReadAllText(edits, Encoding.UTF8), staging, archive);
                    }
                    if (Directory.Exists(Path.Combine(dir, "plugin")))
                    {
                        Directory.CreateDirectory(Path.Combine(output, "plugins"));
                        File.Copy(PluginDll(name), Path.Combine(output, "plugins", name + ".dll"), true);
                    }
                    string files = Path.Combine(dir, "files");
                    if (Directory.Exists(files))
                    {
                        foreach (string file in Directory.GetFiles(files, "*", SearchOption.AllDirectories))
                        {
                            string target = Path.Combine(output, file.Substring(files.Length + 1));
                            Directory.CreateDirectory(Path.GetDirectoryName(target));
                            File.Copy(file, target, true);
                        }
                    }
                }
            }
            finally
            {
                if (archive != null) archive.Dispose();
            }
            if (staging.Count > 0)
            {
                var overrides = new Dictionary<string, KeyValuePair<string, byte[]>>();
                foreach (var pair in staging) overrides[pair.Key.ToLowerInvariant()] = pair.Value;
                Zwp.Build(Path.Combine(gameDir, "data.zwp"), overrides, Path.Combine(output, "data.zwp"), log);
            }
            Directory.CreateDirectory(Path.Combine(output, "Bins"));
        }

        static void Stage(Dictionary<string, KeyValuePair<string, byte[]>> staging, string name, byte[] content)
        {
            KeyValuePair<string, byte[]> existing;
            // Like files in a folder on Windows: rewriting keeps the name's original case.
            staging[name] = new KeyValuePair<string, byte[]>(staging.TryGetValue(name, out existing) ? existing.Key : name, content);
        }

        sealed class Archive : IDisposable
        {
            readonly FileStream stream;
            readonly Dictionary<string, Zwp.Entry> entries = new Dictionary<string, Zwp.Entry>(StringComparer.OrdinalIgnoreCase);

            public Archive(string path)
            {
                stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
                foreach (Zwp.Entry entry in Zwp.ReadDirectory(stream))
                {
                    if (!entries.ContainsKey(entry.Name)) entries[entry.Name] = entry;
                }
            }

            public Zwp.Entry Find(string name)
            {
                Zwp.Entry entry;
                return entries.TryGetValue(name, out entry) ? entry : null;
            }

            public byte[] Read(Zwp.Entry entry)
            {
                return Zwp.ReadEntry(stream, entry);
            }

            public void Dispose()
            {
                stream.Dispose();
            }
        }

        static void ApplyEdits(string mod, string json, Dictionary<string, KeyValuePair<string, byte[]>> staging, Archive archive)
        {
            Func<string, KeyValuePair<string, byte[]>> original = name =>
            {
                KeyValuePair<string, byte[]> staged;
                if (staging.TryGetValue(name, out staged)) return staged;
                Zwp.Entry entry = archive.Find(name);
                if (entry == null) throw new InvalidDataException(mod + ": edits.json names " + name + ", which is not in data.zwp");
                return new KeyValuePair<string, byte[]>(entry.Name, archive.Read(entry));
            };
            foreach (Dictionary<string, object> edit in (IEnumerable)new JavaScriptSerializer().DeserializeObject(json))
            {
                string file = (string)edit["file"];
                byte[] content;
                string target;
                if (edit.ContainsKey("from"))
                {
                    content = original((string)edit["from"]).Value;
                    target = file;
                }
                else
                {
                    KeyValuePair<string, byte[]> source = original(file);
                    content = source.Value;
                    target = source.Key;
                }
                if (!edit.ContainsKey("find"))
                {
                    Stage(staging, target, content);
                    continue;
                }
                string find = (string)edit["find"];
                string text = Latin1.GetString(content);
                int matches;
                string updated = PythonRegex.Sub(find, (string)edit["replace"], text, out matches);
                int? expected = edit.ContainsKey("count") ? Convert.ToInt32(edit["count"]) : (int?)null;
                if (matches == 0 || (expected.HasValue && matches != expected.Value))
                {
                    throw new InvalidDataException(mod + ": edit of " + file + " matched " + matches + " times (expected "
                        + (expected.HasValue ? expected.Value.ToString() : "at least 1") + "): " + find);
                }
                Stage(staging, target, Latin1.GetBytes(updated));
            }
        }
    }

    // re.subn(pattern, template, text, flags=re.MULTILINE) with .NET regular expressions: translates the Python-only
    // syntax mods use (\Z, (?P<name>...), (?P=name)) and expands Python replacement templates (\1, \g<name>, \n, ...).
    public static class PythonRegex
    {
        public static string Sub(string pattern, string template, string text, out int matches)
        {
            var regex = new Regex(TranslatePattern(pattern), RegexOptions.Multiline | RegexOptions.CultureInvariant);
            int count = 0;
            string result = regex.Replace(text, match => { count++; return Expand(template, match); });
            matches = count;
            return result;
        }

        static string TranslatePattern(string pattern)
        {
            var builder = new StringBuilder(pattern.Length);
            for (int i = 0; i < pattern.Length; i++)
            {
                char c = pattern[i];
                if (c == '\\' && i + 1 < pattern.Length)
                {
                    char next = pattern[i + 1];
                    builder.Append(next == 'Z' ? "\\z" : "\\" + next);
                    i++;
                }
                else if (string.CompareOrdinal(pattern, i, "(?P<", 0, 4) == 0)
                {
                    builder.Append("(?<");
                    i += 3;
                }
                else if (string.CompareOrdinal(pattern, i, "(?P=", 0, 4) == 0)
                {
                    int close = pattern.IndexOf(')', i);
                    builder.Append("\\k<").Append(pattern, i + 4, close - i - 4).Append('>');
                    i = close;
                }
                else
                {
                    builder.Append(c);
                }
            }
            return builder.ToString();
        }

        static string Expand(string template, Match match)
        {
            var builder = new StringBuilder();
            for (int i = 0; i < template.Length; i++)
            {
                char c = template[i];
                if (c != '\\' || i + 1 >= template.Length)
                {
                    builder.Append(c);
                    continue;
                }
                char next = template[++i];
                if (char.IsDigit(next))
                {
                    int group = next - '0';
                    if (i + 1 < template.Length && char.IsDigit(template[i + 1]))
                    {
                        group = group * 10 + (template[++i] - '0');
                    }
                    builder.Append(match.Groups[group].Value);
                }
                else if (next == 'g' && i + 1 < template.Length && template[i + 1] == '<')
                {
                    int close = template.IndexOf('>', i);
                    string name = template.Substring(i + 2, close - i - 2);
                    int number;
                    builder.Append(int.TryParse(name, out number) ? match.Groups[number].Value : match.Groups[name].Value);
                    i = close;
                }
                else
                {
                    switch (next)
                    {
                        case 'n': builder.Append('\n'); break;
                        case 't': builder.Append('\t'); break;
                        case 'r': builder.Append('\r'); break;
                        case 'f': builder.Append('\f'); break;
                        case 'v': builder.Append('\v'); break;
                        case 'a': builder.Append('\a'); break;
                        case '\\': builder.Append('\\'); break;
                        default: builder.Append('\\').Append(next); break;
                    }
                }
            }
            return builder.ToString();
        }
    }
}
