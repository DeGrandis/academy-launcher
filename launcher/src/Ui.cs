using System;
using System.Drawing;
using System.Windows.Forms;

namespace AcademyLauncher
{
    // Look and feel shared by the launcher's windows (coordinates are at 96 DPI; forms scale them).
    static class Ui
    {
        public static readonly Color Header = Color.FromArgb(18, 32, 58);
        public static readonly Color Accent = Color.FromArgb(40, 110, 200);
        public static readonly Color AccentHover = Color.FromArgb(55, 130, 225);
        public static readonly Color Background = Color.FromArgb(246, 248, 251);
        public static readonly Color Text = Color.FromArgb(28, 33, 41);
        public static readonly Color Muted = Color.FromArgb(96, 106, 120);
        public static readonly Color Error = Color.FromArgb(185, 40, 40);
        public static readonly Color Notice = Color.FromArgb(255, 236, 170);

        public static readonly Font BodyFont = new Font("Segoe UI", 9.5F);
        public static readonly Font HeadingFont = new Font("Segoe UI Semibold", 14F);
        public static readonly Font TitleFont = new Font("Segoe UI Semibold", 18F);
        public static readonly Font ButtonFont = new Font("Segoe UI Semibold", 10F);
        public static readonly Font BigButtonFont = new Font("Segoe UI Semibold", 15F);
        public static readonly Font CodeFont = new Font("Consolas", 22F, FontStyle.Bold);

        static Icon icon;
        public static Icon AppIcon
        {
            get
            {
                if (icon == null)
                {
                    try { icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); }
                    catch (ArgumentException) { icon = SystemIcons.Application; }
                }
                return icon;
            }
        }

        public static Label Heading(string text, Point location)
        {
            return new Label { Text = text, Location = location, AutoSize = true, Font = HeadingFont, ForeColor = Text, BackColor = Color.Transparent };
        }

        public static Label Paragraph(string text, Point location, int width)
        {
            return new Label
            {
                Text = text,
                Location = location,
                AutoSize = true,
                MaximumSize = new Size(width, 0),
                ForeColor = Text,
                BackColor = Color.Transparent,
            };
        }

        public static Label Caption(string text, Point location)
        {
            return new Label { Text = text, Location = location, AutoSize = true, ForeColor = Muted, BackColor = Color.Transparent };
        }

        public static Button PrimaryButton(string text, Point location, Size size)
        {
            var button = new Button
            {
                Text = text,
                Location = location,
                Size = size,
                FlatStyle = FlatStyle.Flat,
                BackColor = Accent,
                ForeColor = Color.White,
                Font = ButtonFont,
                Cursor = Cursors.Hand,
                UseVisualStyleBackColor = false,
            };
            button.FlatAppearance.BorderSize = 0;
            button.FlatAppearance.MouseOverBackColor = AccentHover;
            button.EnabledChanged += (sender, e) => button.BackColor = button.Enabled ? Accent : Color.FromArgb(150, 165, 185);
            return button;
        }

        public static Button SecondaryButton(string text, Point location, Size size)
        {
            return new Button { Text = text, Location = location, Size = size, Font = BodyFont, UseVisualStyleBackColor = true };
        }

        public static ComboBox Choice(Point location, int width)
        {
            return new ComboBox { Location = location, Width = width, DropDownStyle = ComboBoxStyle.DropDownList };
        }

        // A combo box of (label, value) pairs.
        public sealed class Option
        {
            public readonly string Label;
            public readonly string Value;
            public Option(string label, string value) { Label = label; Value = value; }
            public override string ToString() { return Label; }
        }

        public static void Select(ComboBox box, string value)
        {
            foreach (object item in box.Items)
            {
                var option = item as Option;
                if (option != null && option.Value == value) { box.SelectedItem = item; return; }
            }
            if (box.Items.Count > 0 && box.SelectedIndex < 0) box.SelectedIndex = 0;
        }
    }
}
