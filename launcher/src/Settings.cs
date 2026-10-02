using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace AcademyLauncher
{
    // KEY=VALUE files: defaults.ini ships with the launcher (relay address, update repository), settings.ini holds the
    // player's choices and wins over the defaults.
    public sealed class Settings
    {
        readonly Dictionary<string, string> defaults;
        readonly Dictionary<string, string> values;
        readonly string path;

        Settings(Dictionary<string, string> defaults, Dictionary<string, string> values, string path)
        {
            this.defaults = defaults;
            this.values = values;
            this.path = path;
        }

        public static Settings Load()
        {
            return new Settings(ReadFile(AppPaths.DefaultsFile), ReadFile(AppPaths.SettingsFile), AppPaths.SettingsFile);
        }

        public static Dictionary<string, string> ReadFile(string file)
        {
            var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (!File.Exists(file))
            {
                return result;
            }
            foreach (string raw in File.ReadAllLines(file, Encoding.UTF8))
            {
                string line = raw.Trim();
                int equals = line.IndexOf('=');
                if (line.Length == 0 || line[0] == '#' || line[0] == ';' || equals <= 0)
                {
                    continue;
                }
                result[line.Substring(0, equals).Trim()] = line.Substring(equals + 1).Trim();
            }
            return result;
        }

        public string Get(string key, string fallback = "")
        {
            string value;
            if (values.TryGetValue(key, out value) && value.Length > 0)
            {
                return value;
            }
            return defaults.TryGetValue(key, out value) && value.Length > 0 ? value : fallback;
        }

        public bool GetBool(string key, bool fallback)
        {
            string value = Get(key, fallback ? "1" : "0");
            return value == "1" || value.Equals("true", StringComparison.OrdinalIgnoreCase);
        }

        public double GetDouble(string key, double fallback)
        {
            double value;
            return double.TryParse(Get(key), NumberStyles.Float, CultureInfo.InvariantCulture, out value) ? value : fallback;
        }

        public void Set(string key, string value)
        {
            values[key] = value ?? "";
        }

        public void Set(string key, bool value)
        {
            values[key] = value ? "1" : "0";
        }

        public void Set(string key, double value)
        {
            values[key] = value.ToString(CultureInfo.InvariantCulture);
        }

        public void Save()
        {
            var builder = new StringBuilder("# Academy Launcher settings (written by the launcher)\r\n");
            var keys = new List<string>(values.Keys);
            keys.Sort(StringComparer.OrdinalIgnoreCase);
            foreach (string key in keys)
            {
                builder.Append(key).Append('=').Append(values[key]).Append("\r\n");
            }
            File.WriteAllText(path, builder.ToString(), new UTF8Encoding(false));
        }
    }
}
