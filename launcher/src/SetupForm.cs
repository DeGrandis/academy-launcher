using System;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AcademyLauncher
{
    // First run (and "Change game files"): imports the player's own copy of the game from a disc image or a folder.
    public sealed class SetupForm : Form
    {
        readonly Settings settings;
        readonly Button isoButton;
        readonly Button folderButton;
        readonly Button closeButton;
        readonly ProgressBar progress;
        readonly Label status;
        CancellationTokenSource cancel;

        public SetupForm(Settings settings)
        {
            this.settings = settings;
            SuspendLayout();
            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;
            Text = "Academy Launcher - game files";
            Icon = Ui.AppIcon;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(560, 330);
            BackColor = Ui.Background;
            Font = Ui.BodyFont;

            Controls.Add(Ui.Heading("Set up your game files", new Point(24, 20)));
            Controls.Add(Ui.Paragraph(
                "Academy Launcher runs your own copy of Star Wars: The Clone Wars (original Xbox, North American release). "
                + "Choose a disc image (.iso) made from your disc, or a folder that already holds the extracted game "
                + "(default.xbe, data.zwp, ...). Nothing is uploaded; the files stay on this PC.",
                new Point(24, 62), 512));

            isoButton = Ui.PrimaryButton("Choose disc image (.iso)...", new Point(24, 150), new Size(250, 40));
            isoButton.Click += (sender, e) => ChooseIso();
            Controls.Add(isoButton);
            folderButton = Ui.SecondaryButton("Choose extracted folder...", new Point(286, 150), new Size(250, 40));
            folderButton.Click += (sender, e) => ChooseFolder();
            Controls.Add(folderButton);

            progress = new ProgressBar { Location = new Point(24, 212), Size = new Size(512, 18), Visible = false };
            Controls.Add(progress);
            status = Ui.Paragraph("", new Point(24, 238), 512);
            Controls.Add(status);

            closeButton = Ui.SecondaryButton("Close", new Point(436, 282), new Size(100, 32));
            closeButton.Click += (sender, e) =>
            {
                if (cancel != null) cancel.Cancel();
                else Close();
            };
            Controls.Add(closeButton);
            ResumeLayout(false);
        }

        void Busy(bool busy)
        {
            isoButton.Enabled = !busy;
            folderButton.Enabled = !busy;
            closeButton.Text = busy ? "Cancel" : "Close";
            progress.Visible = busy;
        }

        void ChooseIso()
        {
            using (var dialog = new OpenFileDialog { Filter = "Disc images (*.iso)|*.iso|All files (*.*)|*.*", Title = "Choose your Star Wars: The Clone Wars disc image" })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                string iso = dialog.FileName;
                cancel = new CancellationTokenSource();
                CancellationToken token = cancel.Token;
                Busy(true);
                progress.Value = 0;
                status.Text = "Copying the game files from the disc image...";
                Task.Run(() =>
                {
                    Game.ImportIso(iso, settings, (done, total) =>
                    {
                        int percent = total > 0 ? (int)(done * 100 / total) : 0;
                        BeginInvoke((Action)(() => { if (progress.Value != percent) progress.Value = percent; }));
                    }, token);
                    BeginInvoke((Action)(() => status.Text = "Preparing the game..."));
                    Game.PreparePlay(settings, message => { });
                }).ContinueWith(task => BeginInvoke((Action)(() => Finished(task))));
            }
        }

        void ChooseFolder()
        {
            using (var dialog = new FolderBrowserDialog { Description = "Choose the folder that holds default.xbe and data.zwp" })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                string folder = dialog.SelectedPath;
                cancel = new CancellationTokenSource();
                Busy(true);
                progress.Style = ProgressBarStyle.Marquee;
                status.Text = "Checking the game files...";
                Task.Run(() =>
                {
                    Game.UseFolder(folder, settings);
                    Game.PreparePlay(settings, message => { });
                }).ContinueWith(task => BeginInvoke((Action)(() => Finished(task))));
            }
        }

        void Finished(Task task)
        {
            cancel = null;
            progress.Style = ProgressBarStyle.Continuous;
            Busy(false);
            if (task.IsFaulted)
            {
                Exception error = task.Exception.GetBaseException();
                AppPaths.Log("setup failed: " + error);
                status.ForeColor = Ui.Error;
                status.Text = error is OperationCanceledException ? "Cancelled." : error.Message;
                return;
            }
            status.ForeColor = Ui.Text;
            DialogResult = DialogResult.OK;
            Close();
        }
    }
}
