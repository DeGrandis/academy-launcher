using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AcademyLauncher
{
    public sealed class MainForm : Form
    {
        readonly Settings settings = Settings.Load();
        readonly List<Preset> presets = Preset.LoadAll();
        Process game;
        Updater.Release update;

        // header
        readonly Panel updateBar;
        readonly Label updateText;
        readonly Button updateButton;
        // play
        readonly ComboBox presetBox;
        readonly Label presetDescription;
        readonly Button playButton;
        readonly Label playStatus;
        // online
        readonly RadioButton viaRelay, viaDirect, viaLan;
        readonly Button hostButton, joinButton, copyButton;
        readonly TextBox joinBox;
        readonly Label hostCode, hostHelp, joinHelp, onlineStatus;
        // settings
        readonly Label gameFolder;

        public MainForm()
        {
            SuspendLayout();
            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;
            Text = "Academy Launcher";
            Icon = Ui.AppIcon;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(720, 590);
            BackColor = Ui.Background;
            Font = Ui.BodyFont;

            // Header
            var header = new Panel { Location = new Point(0, 0), Size = new Size(720, 76), BackColor = Ui.Header };
            header.Controls.Add(new Label { Text = "Academy Launcher", Font = Ui.TitleFont, ForeColor = Color.White, AutoSize = true, Location = new Point(22, 10) });
            header.Controls.Add(new Label
            {
                Text = "Star Wars: The Clone Wars on PC  ·  version " + AppPaths.Version + (AppPaths.FromRepository ? " (development)" : ""),
                ForeColor = Color.FromArgb(170, 190, 220),
                AutoSize = true,
                Location = new Point(25, 47),
            });
            Controls.Add(header);

            updateBar = new Panel { Location = new Point(0, 76), Size = new Size(720, 40), BackColor = Ui.Notice, Visible = false };
            updateText = new Label { AutoSize = true, Location = new Point(22, 11), ForeColor = Ui.Text };
            updateButton = Ui.PrimaryButton("Update now", new Point(590, 5), new Size(110, 30));
            updateButton.Click += (sender, e) => InstallUpdate();
            updateBar.Controls.Add(updateText);
            updateBar.Controls.Add(updateButton);
            Controls.Add(updateBar);

            var tabs = new TabControl { Location = new Point(12, 88), Size = new Size(696, 490), Padding = new Point(14, 5) };
            Controls.Add(tabs);
            var playTab = Tab(tabs, "Play");
            var onlineTab = Tab(tabs, "Online");
            var settingsTab = Tab(tabs, "Settings");
            var controlsTab = Tab(tabs, "Controls");

            // Play
            playTab.Controls.Add(Ui.Heading("Play", new Point(20, 16)));
            playTab.Controls.Add(Ui.Caption("Mode", new Point(22, 62)));
            presetBox = Ui.Choice(new Point(22, 82), 300);
            foreach (Preset preset in presets) presetBox.Items.Add(new Ui.Option(preset.Name, preset.Id));
            Ui.Select(presetBox, settings.Get("preset", presets[0].Id));
            presetBox.SelectedIndexChanged += (sender, e) => { settings.Set("preset", SelectedPreset().Id); settings.Save(); ShowPreset(); };
            playTab.Controls.Add(presetBox);
            presetDescription = Ui.Paragraph("", new Point(22, 118), 630);
            playTab.Controls.Add(presetDescription);
            playButton = Ui.PrimaryButton("PLAY", new Point(22, 220), new Size(220, 60));
            playButton.Font = Ui.BigButtonFont;
            playButton.Click += (sender, e) => Play();
            playTab.Controls.Add(playButton);
            playStatus = Ui.Paragraph("", new Point(22, 292), 630);
            playTab.Controls.Add(playStatus);
            playTab.Controls.Add(Ui.Paragraph(
                "In the game: F11 toggles fullscreen, F9 changes the resolution, F7 / F8 change the view distance. "
                + "Your saves are kept between updates.", new Point(22, 400), 630));
            ShowPreset();

            // Online
            onlineTab.Controls.Add(Ui.Heading("Play online", new Point(20, 16)));
            onlineTab.Controls.Add(Ui.Paragraph(
                "Online games use the " + OnlinePreset().Name + " mode, and everyone needs the same launcher version. "
                + "The host starts System Link in the game; everyone else joins it.", new Point(22, 52), 640));
            var connection = new GroupBox { Text = "Connection", Location = new Point(22, 96), Size = new Size(640, 52) };
            viaRelay = new RadioButton { Text = "Join code (easiest)", Location = new Point(14, 20), AutoSize = true };
            viaDirect = new RadioButton { Text = "Direct / IP address", Location = new Point(220, 20), AutoSize = true };
            viaLan = new RadioButton { Text = "Same network (LAN)", Location = new Point(420, 20), AutoSize = true };
            connection.Controls.AddRange(new Control[] { viaRelay, viaDirect, viaLan });
            onlineTab.Controls.Add(connection);
            string mode = settings.Get("online_mode", "relay");
            (mode == "direct" ? viaDirect : mode == "lan" ? viaLan : viaRelay).Checked = true;
            foreach (RadioButton radio in new[] { viaRelay, viaDirect, viaLan })
            {
                radio.CheckedChanged += (sender, e) =>
                {
                    if (!((RadioButton)sender).Checked) return;
                    settings.Set("online_mode", OnlineMode());
                    settings.Save();
                    ShowOnlineMode();
                };
            }

            var host = new GroupBox { Text = "Host a game", Location = new Point(22, 158), Size = new Size(312, 200) };
            hostButton = Ui.PrimaryButton("Host", new Point(14, 26), new Size(120, 36));
            hostButton.Click += (sender, e) => Host();
            hostCode = new Label { Location = new Point(146, 26), Size = new Size(110, 36), Font = Ui.CodeFont, ForeColor = Ui.Accent, TextAlign = ContentAlignment.MiddleLeft };
            copyButton = Ui.SecondaryButton("Copy", new Point(254, 30), new Size(50, 28));
            copyButton.Visible = false;
            copyButton.Click += (sender, e) => { if (hostCode.Text.Length > 0) Clipboard.SetText(hostCode.Text); };
            hostHelp = Ui.Paragraph("", new Point(14, 72), 286);
            host.Controls.AddRange(new Control[] { hostButton, hostCode, copyButton, hostHelp });
            onlineTab.Controls.Add(host);

            var join = new GroupBox { Text = "Join a game", Location = new Point(350, 158), Size = new Size(312, 200) };
            joinBox = new TextBox { Location = new Point(14, 32), Width = 150, CharacterCasing = CharacterCasing.Normal };
            joinBox.KeyDown += (sender, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; Join(); } };
            joinButton = Ui.PrimaryButton("Join", new Point(176, 26), new Size(120, 36));
            joinButton.Click += (sender, e) => Join();
            joinHelp = Ui.Paragraph("", new Point(14, 72), 286);
            join.Controls.AddRange(new Control[] { joinBox, joinButton, joinHelp });
            onlineTab.Controls.Add(join);
            onlineStatus = Ui.Paragraph("", new Point(22, 372), 640);
            onlineTab.Controls.Add(onlineStatus);
            ShowOnlineMode();

            // Settings
            BuildSettings(settingsTab, out gameFolder);

            // Controls
            controlsTab.Controls.Add(Ui.Heading("Controls", new Point(20, 16)));
            controlsTab.Controls.Add(new TextBox
            {
                Location = new Point(22, 56),
                Size = new Size(640, 380),
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                BackColor = Color.White,
                Font = new Font("Consolas", 9.5F),
                Text = ControlsText,
            });

            ResumeLayout(false);
            Shown += (sender, e) => Startup();
        }

        const string ControlsText =
            "An Xbox or other XInput controller works like the original pad (plug it in before starting the game).\r\n\r\n"
            + "Keyboard (Xbox button in brackets):\r\n"
            + "  Move / steer ............ W A S D or arrow keys  [left stick, d-pad]\r\n"
            + "  A ....................... Enter or left mouse button\r\n"
            + "  B ....................... Esc, Backspace or right mouse button\r\n"
            + "  X / Y ................... X / Y\r\n"
            + "  Left / right trigger .... Q / E\r\n"
            + "  Black / White ........... R / F  [RB / LB on a controller]\r\n"
            + "  Start / Back ............ Space / Tab\r\n\r\n"
            + "Window:\r\n"
            + "  F11 or Alt+Enter ........ fullscreen on/off\r\n"
            + "  F9 ...................... resolution (1x, 2x, 3x, 4x, window size)\r\n"
            + "  F7 / F8 ................. view distance down / up\r\n\r\n"
            + "Conquest with AI players: in the lobby press RB (or R) to move to an empty slot and choose AI; set its team "
            + "like a player's. In a match, d-pad up sends your troops to attack.";

        static TabPage Tab(TabControl tabs, string text)
        {
            var page = new TabPage(text) { BackColor = Ui.Background, UseVisualStyleBackColor = false };
            tabs.TabPages.Add(page);
            return page;
        }

        void BuildSettings(TabPage tab, out Label folderLabel)
        {
            tab.AutoScroll = true;
            tab.Controls.Add(Ui.Heading("Settings", new Point(20, 16)));
            int y = 58;
            Func<string, Control, int> row = (label, control) =>
            {
                tab.Controls.Add(Ui.Caption(label, new Point(22, y + 3)));
                control.Location = new Point(190, y);
                tab.Controls.Add(control);
                y += 32;
                return y;
            };

            var resolution = Ui.Choice(Point.Empty, 220);
            resolution.Items.AddRange(new object[]
            {
                new Ui.Option("Follow the window size", "window"), new Ui.Option("1280 x 720", "1280x720"),
                new Ui.Option("1920 x 1080", "1920x1080"), new Ui.Option("2560 x 1440", "2560x1440"),
                new Ui.Option("3840 x 2160", "3840x2160"), new Ui.Option("Original x2 (960 lines)", "x2"),
                new Ui.Option("Original x3 (1440 lines)", "x3"),
            });
            Bind(resolution, "resolution", "window");
            row("Resolution", resolution);

            var fullscreen = new CheckBox { Text = "Start in fullscreen", AutoSize = true, Checked = settings.GetBool("fullscreen", false) };
            fullscreen.CheckedChanged += (sender, e) => { settings.Set("fullscreen", fullscreen.Checked); settings.Save(); };
            row("Display", fullscreen);
            var widescreen = new CheckBox { Text = "Widescreen (16:9)", AutoSize = true, Checked = settings.GetBool("widescreen", true) };
            widescreen.CheckedChanged += (sender, e) => { settings.Set("widescreen", widescreen.Checked); settings.Save(); };
            row("", widescreen);

            var view = Ui.Choice(Point.Empty, 220);
            view.Items.AddRange(new object[]
            {
                new Ui.Option("Original", "1"), new Ui.Option("1.5x", "1.5"), new Ui.Option("2x", "2"), new Ui.Option("3x", "3"),
                new Ui.Option("4x (recommended)", "4"), new Ui.Option("6x", "6"), new Ui.Option("8x", "8"),
            });
            Bind(view, "view_distance", "4");
            row("View distance", view);

            var camera = Ui.Choice(Point.Empty, 220);
            camera.Items.AddRange(new object[]
            {
                new Ui.Option("Original", "1"), new Ui.Option("20% further (default)", "1.2"), new Ui.Option("40% further", "1.4"),
            });
            Bind(camera, "camera_distance", "1.2");
            row("Camera distance", camera);

            var volume = new TrackBar { Minimum = 0, Maximum = 100, TickFrequency = 10, Width = 220, AutoSize = false, Height = 30 };
            volume.Value = Math.Max(0, Math.Min(100, (int)Math.Round(settings.GetDouble("volume", 1) * 100)));
            volume.ValueChanged += (sender, e) => { settings.Set("volume", volume.Value / 100.0); settings.Save(); };
            row("Volume", volume);

            var relay = new TextBox { Width = 220, Text = settings.Get("relay") };
            relay.Leave += (sender, e) => { settings.Set("relay", relay.Text.Trim()); settings.Save(); };
            row("Relay server", relay);
            var port = new NumericUpDown { Minimum = 1024, Maximum = 65535, Width = 90, Value = int.Parse(settings.Get("port", "3074")) };
            port.ValueChanged += (sender, e) => { settings.Set("port", port.Value.ToString()); settings.Save(); };
            row("UDP port", port);

            var updates = new CheckBox { Text = "Check for updates", AutoSize = true, Checked = settings.GetBool("check_updates", true) };
            updates.CheckedChanged += (sender, e) => { settings.Set("check_updates", updates.Checked); settings.Save(); };
            var channel = Ui.Choice(new Point(340, 0), 100);
            channel.Items.AddRange(new object[] { new Ui.Option("Stable", "stable"), new Ui.Option("Beta", "beta") });
            Bind(channel, "update_channel", "stable");
            row("Updates", updates);
            channel.Location = new Point(340, y - 32);
            tab.Controls.Add(channel);
            var checkNow = Ui.SecondaryButton("Check now", new Point(450, y - 33), new Size(100, 26));
            checkNow.Click += (sender, e) => CheckForUpdates(true);
            tab.Controls.Add(checkNow);

            y += 6;
            folderLabel = Ui.Paragraph("", new Point(190, y + 3), 470);
            tab.Controls.Add(Ui.Caption("Game files", new Point(22, y + 3)));
            tab.Controls.Add(folderLabel);
            y += 30;
            var change = Ui.SecondaryButton("Change game files...", new Point(190, y), new Size(150, 28));
            change.Click += (sender, e) => RunSetup();
            var rebuild = Ui.SecondaryButton("Rebuild mods", new Point(348, y), new Size(110, 28));
            rebuild.Click += (sender, e) => RebuildMods();
            var open = Ui.SecondaryButton("Open saves and logs", new Point(466, y), new Size(150, 28));
            open.Click += (sender, e) => { Directory.CreateDirectory(AppPaths.PlayDir); Process.Start("explorer.exe", "\"" + AppPaths.PlayDir + "\""); };
            tab.Controls.AddRange(new Control[] { change, rebuild, open });
        }

        void Bind(ComboBox box, string key, string fallback)
        {
            Ui.Select(box, settings.Get(key, fallback));
            box.SelectedIndexChanged += (sender, e) =>
            {
                var option = box.SelectedItem as Ui.Option;
                if (option == null) return;
                settings.Set(key, option.Value);
                settings.Save();
            };
        }

        Preset SelectedPreset()
        {
            var option = presetBox.SelectedItem as Ui.Option;
            return presets.Find(p => option != null && p.Id == option.Value) ?? presets[0];
        }

        Preset OnlinePreset()
        {
            return presets.Find(p => p.Online) ?? presets[0];
        }

        void ShowPreset()
        {
            presetDescription.Text = SelectedPreset().Description;
        }

        string OnlineMode()
        {
            return viaDirect.Checked ? "direct" : viaLan.Checked ? "lan" : "relay";
        }

        void ShowOnlineMode()
        {
            string mode = OnlineMode();
            joinBox.Visible = mode != "lan";
            hostCode.Text = "";
            copyButton.Visible = false;
            if (mode == "relay")
            {
                hostHelp.Text = "You get a join code to send to your friends. Then in the game: Network Play > System Link > Create Game.";
                joinHelp.Text = "Type the host's join code, then in the game: Network Play > System Link > Search for Games.";
            }
            else if (mode == "direct")
            {
                hostHelp.Text = "Your router must forward UDP port " + settings.Get("port", "3074") + " to this PC. Friends join with your "
                    + "public IP address. In the game: Network Play > System Link > Create Game.";
                joinHelp.Text = "Type the host's IP address (and :port if not 3074), then in the game: Network Play > System Link > Search.";
            }
            else
            {
                hostHelp.Text = "For players on the same home network. In the game: Network Play > System Link > Create Game.";
                joinHelp.Text = "Finds games on your network. In the game: Network Play > System Link > Search for Games.";
            }
        }

        void Startup()
        {
            ShowGameFolder();
            if (!Game.IsReady(settings))
            {
                RunSetup();
            }
            CheckForUpdates(false);
        }

        void ShowGameFolder()
        {
            gameFolder.Text = Game.IsReady(settings) ? Game.GameDir(settings) : "Not set up yet";
        }

        void RunSetup()
        {
            using (var setup = new SetupForm(settings))
            {
                setup.ShowDialog(this);
            }
            ShowGameFolder();
        }

        void RebuildMods()
        {
            if (game != null) return;
            try
            {
                if (Directory.Exists(AppPaths.ModRootsDir)) Directory.Delete(AppPaths.ModRootsDir, true);
                MessageBox.Show(this, "The mods will be rebuilt the next time you play.", "Academy Launcher");
            }
            catch (IOException error)
            {
                MessageBox.Show(this, error.Message, "Academy Launcher", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        // ---- playing

        void SetBusy(bool busy)
        {
            playButton.Enabled = !busy;
            hostButton.Enabled = !busy;
            joinButton.Enabled = !busy;
            presetBox.Enabled = !busy;
        }

        void Status(string text, bool error = false)
        {
            foreach (Label label in new[] { playStatus, onlineStatus })
            {
                label.Text = text;
                label.ForeColor = error ? Ui.Error : Ui.Text;
            }
        }

        // Prepares the game and the preset's mods off the UI thread, then starts the game with `network` settings.
        void Start(Preset preset, Dictionary<string, string> network, string runningText)
        {
            if (!Game.IsReady(settings))
            {
                RunSetup();
                if (!Game.IsReady(settings)) return;
            }
            SetBusy(true);
            Status("Getting ready...");
            Task.Run(() =>
            {
                Game.PreparePlay(settings, message => BeginInvoke((Action)(() => Status(message))));
                return Game.EnsureModRoot(preset, settings, message => BeginInvoke((Action)(() => Status(message))));
            }).ContinueWith(task => BeginInvoke((Action)(() =>
            {
                if (task.IsFaulted)
                {
                    Exception error = task.Exception.GetBaseException();
                    AppPaths.Log("start failed: " + error);
                    Status(error.Message, true);
                    SetBusy(false);
                    return;
                }
                try
                {
                    game = Game.Launch(settings, task.Result, network);
                    game.EnableRaisingEvents = true;
                    game.Exited += (sender, e) => BeginInvoke((Action)GameExited);
                    Status(runningText);
                    WindowState = FormWindowState.Minimized;
                }
                catch (Exception error)
                {
                    AppPaths.Log("launch failed: " + error);
                    Status("The game could not start: " + error.Message, true);
                    SetBusy(false);
                }
            })));
        }

        void GameExited()
        {
            int code = game.ExitCode;
            game = null;
            SetBusy(false);
            hostCode.Text = "";
            copyButton.Visible = false;
            if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
            if (code != 0)
            {
                Status("The game stopped unexpectedly (code " + code + "). The log is in " + Path.Combine(AppPaths.PlayDir, "cw_runtime.log"), true);
            }
            else
            {
                Status("");
            }
        }

        void Play()
        {
            Preset preset = SelectedPreset();
            Start(preset, new Dictionary<string, string>(), "Playing " + preset.Name + ".");
        }

        Dictionary<string, string> NetworkBase(Preset preset)
        {
            return new Dictionary<string, string>
            {
                { "CW_NET", "1" },
                { "CW_NET_PORT", settings.Get("port", "3074") },
                { "CW_NET_BUILD", Online.BuildId(preset) },
            };
        }

        void Host()
        {
            Preset preset = OnlinePreset();
            Dictionary<string, string> network = NetworkBase(preset);
            string mode = OnlineMode();
            if (mode == "relay")
            {
                string code = Online.NewJoinCode();
                network["CW_NET_RELAY"] = settings.Get("relay");
                network["CW_NET_ROOM"] = code;
                hostCode.Text = code;
                copyButton.Visible = true;
                Start(preset, network, "Hosting with join code " + code + ". Send it to your friends, then create the game (Network Play > System Link > Create Game).");
            }
            else
            {
                if (mode == "lan") network["CW_NET_LAN"] = "1";
                Start(preset, network, "Hosting. In the game: Network Play > System Link > Create Game.");
            }
        }

        void Join()
        {
            Preset preset = OnlinePreset();
            Dictionary<string, string> network = NetworkBase(preset);
            string mode = OnlineMode();
            uint build = Convert.ToUInt32(network["CW_NET_BUILD"], 16);
            if (mode == "lan")
            {
                network["CW_NET_LAN"] = "1";
                Start(preset, network, "In the game: Network Play > System Link > Search for Games.");
                return;
            }
            string target = joinBox.Text.Trim();
            string code = mode == "relay" ? Online.NormalizeCode(target) : null;
            if (mode == "relay" ? code.Length != 6 : target.Length == 0)
            {
                Status(mode == "relay" ? "Type the 6-letter join code from the host." : "Type the host's IP address.", true);
                return;
            }
            string server = mode == "relay" ? settings.Get("relay") : target;
            SetBusy(true);
            Status(mode == "relay" ? "Looking for the game..." : "Contacting the host...");
            Task.Run(() => Online.Query(server, code)).ContinueWith(task => BeginInvoke((Action)(() =>
            {
                SetBusy(false);
                if (task.IsFaulted)
                {
                    Status("Could not reach " + server + ": " + task.Exception.GetBaseException().Message, true);
                    return;
                }
                Online.QueryResult result = task.Result;
                if (mode == "relay")
                {
                    if (!result.Answered)
                    {
                        Status("The relay server (" + server + ") did not answer. Check your internet connection, or try again later.", true);
                        return;
                    }
                    if (result.Players == 0)
                    {
                        Status("No game with code " + code + ". Check the code with the host; their game must be running.", true);
                        return;
                    }
                }
                else if (!result.Answered)
                {
                    DialogResult answer = MessageBox.Show(this,
                        "The host did not answer. They need to be in Network Play > System Link > Create Game, with UDP port 3074 forwarded to their PC.\r\n\r\nStart the game anyway and keep searching?",
                        "Academy Launcher", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                    if (answer != DialogResult.Yes)
                    {
                        Status("");
                        return;
                    }
                }
                if (result.Answered && result.Build != build)
                {
                    Status("The host has a different version or mode. Make sure you both have the latest Academy Launcher.", true);
                    return;
                }
                if (mode == "relay")
                {
                    network["CW_NET_RELAY"] = server;
                    network["CW_NET_ROOM"] = code;
                }
                else
                {
                    network["CW_NET_JOIN"] = target;
                }
                Start(preset, network, "In the game: Network Play > System Link > Search for Games, then pick the host's session.");
            })));
        }

        // ---- updates

        void CheckForUpdates(bool userAsked)
        {
            Task.Run(() => Updater.Check(settings)).ContinueWith(task => BeginInvoke((Action)(() =>
            {
                if (task.IsFaulted)
                {
                    AppPaths.Log("update check failed: " + task.Exception.GetBaseException().Message);
                    if (userAsked) MessageBox.Show(this, "Could not check for updates: " + task.Exception.GetBaseException().Message, "Academy Launcher");
                    return;
                }
                update = task.Result;
                if (update == null)
                {
                    if (userAsked)
                    {
                        MessageBox.Show(this, settings.Get("update_repo").Length == 0 ? "Updates are not configured for this build." : "You have the latest version.", "Academy Launcher");
                    }
                    return;
                }
                updateText.Text = "Version " + update.Version + " is available.";
                updateBar.Visible = true;
                updateBar.BringToFront();
            })));
        }

        void InstallUpdate()
        {
            if (update == null) return;
            if (game != null)
            {
                MessageBox.Show(this, "Close the game first.", "Academy Launcher");
                return;
            }
            updateButton.Enabled = false;
            Task.Run(() => Updater.Install(update, percent => BeginInvoke((Action)(() => updateText.Text = "Downloading version " + update.Version + "... " + percent + "%"))))
                .ContinueWith(task => BeginInvoke((Action)(() =>
                {
                    if (task.IsFaulted)
                    {
                        updateButton.Enabled = true;
                        updateText.Text = "Update failed: " + task.Exception.GetBaseException().Message;
                        return;
                    }
                    Close();
                })));
        }
    }
}
