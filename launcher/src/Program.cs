using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace AcademyLauncher
{
    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
#if TOOL
            return Tool(args);
#else
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.ThreadException += (sender, e) => Fail(e.Exception);
            AppDomain.CurrentDomain.UnhandledException += (sender, e) => Fail(e.ExceptionObject as Exception);
            using (var mutex = new Mutex(true, "AcademyLauncher.Single", out bool first))
            {
                if (!first)
                {
                    MessageBox.Show("Academy Launcher is already running.", "Academy Launcher", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return 0;
                }
                try
                {
                    Application.Run(new MainForm());
                }
                catch (Exception error)
                {
                    Fail(error);
                    return 1;
                }
            }
            return 0;
#endif
        }

        static void Fail(Exception error)
        {
            AppPaths.Log("error: " + error);
            MessageBox.Show(error != null ? error.Message : "Unknown error", "Academy Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }

#if TOOL
        // academy-tool.exe: the launcher's operations from a command line (tests, CI, troubleshooting).
        static int Tool(string[] args)
        {
            const string usage = "usage: academy-tool <command>\n"
                + "  list-iso <iso>                       list the files in a game disc image\n"
                + "  extract <iso> <dir>                  extract a game disc image\n"
                + "  verify <game-dir>                    check that a folder holds the supported game\n"
                + "  presets                              list the presets with their fingerprints\n"
                + "  build <preset> <game-dir> <out-dir>  build a preset's mod root\n"
                + "  query <host[:port]> [code]           ask a game or relay (room) who is there\n"
                + "  version                              print the version";
            try
            {
                string command = args.Length > 0 ? args[0] : "";
                switch (command)
                {
                    case "list-iso":
                        using (var image = new Xiso(args[1]))
                        {
                            foreach (Xiso.Entry entry in image.ListFiles())
                            {
                                Console.WriteLine((entry.IsDirectory ? "<dir>" : entry.Size.ToString()).PadLeft(12) + " " + entry.Path);
                            }
                        }
                        return 0;
                    case "extract":
                    {
                        long lastReport = 0;
                        using (var image = new Xiso(args[1]))
                        {
                            image.ExtractAll(args[2], (done, total) =>
                            {
                                if (done - lastReport > (100 << 20) || done == total)
                                {
                                    lastReport = done;
                                    Console.WriteLine("{0} / {1} MB", done >> 20, total >> 20);
                                }
                            }, CancellationToken.None);
                        }
                        return 0;
                    }
                    case "verify":
                        Game.Verify(args[1]);
                        Console.WriteLine("ok: supported game");
                        return 0;
                    case "presets":
                        foreach (Preset preset in Preset.LoadAll())
                        {
                            Console.WriteLine(preset.Id + " " + Online.BuildId(preset) + " " + string.Join("+", preset.Mods));
                        }
                        return 0;
                    case "build":
                    {
                        Preset preset = Preset.LoadAll().Find(p => p.Id == args[1]);
                        if (preset == null) throw new ArgumentException("no preset " + args[1]);
                        ModBuilder.Build(preset, args[2], Path.GetFullPath(args[3]), Console.WriteLine);
                        Console.WriteLine("built " + args[3]);
                        return 0;
                    }
                    case "query":
                    {
                        Online.QueryResult result = Online.Query(args[1], args.Length > 2 ? Online.NormalizeCode(args[2]) : null);
                        Console.WriteLine(result.Answered ? "players " + result.Players + " build " + result.Build.ToString("x8") : "no answer");
                        return result.Answered ? 0 : 2;
                    }
                    case "version":
                        Console.WriteLine(AppPaths.Version);
                        return 0;
                    default:
                        Console.Error.WriteLine(usage);
                        return 1;
                }
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("error: " + error.Message);
                return 1;
            }
        }
#endif
    }
}
