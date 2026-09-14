using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AirDAP.Manager
{
    internal sealed class ManagerWindow : Form
    {
        private readonly Label state = new Label { AutoSize = true, Text = "正在读取服务状态…", Font = new Font("Microsoft YaHei UI", 15, FontStyle.Bold) };
        private readonly Label details = new Label { AutoSize = true, MaximumSize = new Size(730, 0) };
        private readonly Label driver = new Label { AutoSize = true, Text = "正在检测 USB/IP…" };
        private readonly NumericUpDown httpPort = new NumericUpDown { Minimum = 1, Maximum = 65535, Value = 8080, Width = 120 };
        private readonly NumericUpDown usbPort = new NumericUpDown { Minimum = 1, Maximum = 65535, Value = 3242, Width = 120 };
        private readonly CheckBox http = new CheckBox { Text = "启用本机 Web 管理", Checked = true, AutoSize = true };
        private readonly CheckBox automatic = new CheckBox { Text = "开机自动启动服务", Checked = true, AutoSize = true };
        private readonly TextBox log = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, BackColor = Color.White };
        private readonly FlowLayoutPanel actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = true };
        private readonly TableLayoutPanel settingsPanel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 3 };
        private readonly Button install, update, configure, start, stop, restart, uninstall, open, refresh;
        private Installation current;
        private bool busy;
        private bool readable;

        internal ManagerWindow()
        {
            Text = "AirDAP 服务管理器";
            Font = new Font("Microsoft YaHei UI", 10);
            ClientSize = new Size(830, 680);
            MinimumSize = new Size(840, 720);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.FromArgb(245, 247, 250);
            AutoScaleMode = AutoScaleMode.Dpi;
            var root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(24), ColumnCount = 1, RowCount = 8 };
            foreach (int height in new[] { 42, 32, 115, 105, 94 }) root.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 33));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 27));
            root.Controls.Add(new Label { Text = "AirDAP  服务管理器", AutoSize = true,
                Font = new Font("Microsoft YaHei UI", 20, FontStyle.Bold), ForeColor = Color.FromArgb(20, 48, 65) }, 0, 0);
            root.Controls.Add(new Label { Text = "安装、更新和管理本机服务。关闭此窗口后，已启动的服务会继续运行。", AutoSize = true }, 0, 1);
            var card = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false,
                BackColor = Color.White, Padding = new Padding(12) };
            card.Controls.Add(state); card.Controls.Add(details);
            root.Controls.Add(card, 0, 2);
            foreach (int width in new[] { 140, 150, 160, 230 }) settingsPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, width));
            settingsPanel.Controls.Add(new Label { Text = "Web 端口", AutoSize = true }, 0, 0);
            settingsPanel.Controls.Add(httpPort, 1, 0);
            settingsPanel.Controls.Add(new Label { Text = "USB/IP 端口", AutoSize = true }, 2, 0);
            settingsPanel.Controls.Add(usbPort, 3, 0);
            settingsPanel.Controls.Add(http, 0, 1); settingsPanel.SetColumnSpan(http, 2);
            settingsPanel.Controls.Add(automatic, 2, 1); settingsPanel.SetColumnSpan(automatic, 2);
            var url = new Label { AutoSize = true, ForeColor = Color.FromArgb(0, 99, 145) };
            settingsPanel.Controls.Add(url, 0, 2); settingsPanel.SetColumnSpan(url, 4);
            EventHandler updateUrl = delegate { url.Text = http.Checked ? "管理地址：http://airdap.localhost:" + httpPort.Value : "Web 管理已关闭"; httpPort.Enabled = http.Checked; };
            httpPort.ValueChanged += updateUrl; http.CheckedChanged += updateUrl; updateUrl(null, EventArgs.Empty);
            root.Controls.Add(settingsPanel, 0, 3);
            install = ActionButton("安装服务", "install");
            update = ActionButton("更新服务", "update");
            configure = ActionButton("应用设置", "configure");
            start = ActionButton("启动", "start"); stop = ActionButton("停止", "stop"); restart = ActionButton("重启", "restart");
            uninstall = ActionButton("卸载服务", "uninstall");
            open = Button("打开管理页面", delegate { if (current != null) Open(current.Settings.Url); });
            refresh = Button("刷新状态", async delegate { await RefreshState(); });
            actions.Controls.Add(open); actions.Controls.Add(refresh);
            root.Controls.Add(actions, 0, 4);
            root.Controls.Add(log, 0, 5);
            var drivers = new FlowLayoutPanel { Dock = DockStyle.Fill };
            drivers.Controls.Add(driver);
            var driverLink = new LinkLabel { Text = "获取 USB/IP 驱动", AutoSize = true };
            driverLink.LinkClicked += delegate { Open("https://github.com/vadimgrn/usbip-win2/releases"); };
            drivers.Controls.Add(driverLink);
            root.Controls.Add(drivers, 0, 6);
            root.Controls.Add(new Label { Text = "卸载保留设备配置与凭据。Web 仅监听本机。更新使用本管理器随附的服务程序。", AutoSize = true, ForeColor = Color.DimGray }, 0, 7);
            Controls.Add(root);
            SetButtons(false);
            Shown += async delegate { await RefreshState(); };
            FormClosing += delegate(object sender, FormClosingEventArgs e) {
                if (busy) { e.Cancel = true; MessageBox.Show(this, "操作尚未结束，请等待完成。", Text, MessageBoxButtons.OK, MessageBoxIcon.Information); }
            };
        }
        private Button Button(string text, EventHandler click)
        {
            var button = new Button { Text = text, AutoSize = true, Height = 34, Padding = new Padding(6, 2, 6, 2), Margin = new Padding(0, 5, 8, 3) };
            button.Click += click;
            return button;
        }
        private Button ActionButton(string text, string action)
        {
            var button = Button(text, async delegate { await Perform(action); });
            actions.Controls.Add(button);
            return button;
        }
        private void SetButtons(bool enabled)
        {
            bool installed = current != null;
            actions.Enabled = !busy;
            settingsPanel.Enabled = !busy;
            install.Enabled = enabled && !installed;
            update.Enabled = configure.Enabled = start.Enabled = stop.Enabled = restart.Enabled = uninstall.Enabled = enabled && installed;
            open.Enabled = enabled && installed && current.Settings.HttpEnabled && current.State == "Running";
            refresh.Enabled = !busy;
        }
        private void Log(string message) { log.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + message + Environment.NewLine); }
        private void Open(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); }
            catch (Exception e) { Log("无法打开浏览器：" + e.Message); }
        }
        private static string ErrorText(Exception error)
        {
            var aggregate = error as AggregateException;
            return aggregate == null ? error.Message : aggregate.Message + Environment.NewLine +
                string.Join(Environment.NewLine, System.Linq.Enumerable.Select(aggregate.Flatten().InnerExceptions, e => e.Message));
        }
        private async Task RefreshState()
        {
            if (busy) return;
            busy = true; SetButtons(false);
            try
            {
                var snapshot = await Task.Run(() => {
                    var installation = Manager.Inspect();
                    return new { Installation = installation, Hash = installation == null ? "" : Manager.InstalledHash(installation),
                        BundledHash = Manager.PayloadHash(), Driver = Manager.DriverStatus() };
                });
                current = snapshot.Installation;
                readable = true;
                state.Text = current == null ? "尚未安装服务" : current.Name + " · " + StateName(current.State);
                details.Text = "随附构建：" + snapshot.BundledHash.Substring(0, 12) +
                    (current == null ? "\n选择端口后点击安装，即可启动服务。" : "    已安装：" + snapshot.Hash.Substring(0, 12) +
                    "\n数据目录：" + current.DataDirectory);
                driver.Text = snapshot.Driver;
                if (current != null)
                {
                    httpPort.Value = current.Settings.HttpPort; usbPort.Value = current.Settings.UsbipPort;
                    http.Checked = current.Settings.HttpEnabled; automatic.Checked = current.Settings.Automatic;
                    if (current.Name == Manager.LegacyName) Log("检测到旧版 AirDAPNative，点击更新可将服务名迁移为 AirDAP，保留原数据目录。");
                }
            }
            catch (Exception e) { readable = false; state.Text = "无法安全管理此安装"; details.Text = "请查看下方详细信息。"; Log(ErrorText(e)); }
            finally { busy = false; SetButtons(readable); }
        }
        private static string StateName(string value)
        {
            switch (value) { case "Running": return "运行中"; case "Stopped": return "已停止";
                case "Start Pending": return "正在启动"; case "Stop Pending": return "正在停止"; default: return value; }
        }
        private async Task Perform(string action)
        {
            if (busy || !readable) return;
            var settings = new Settings { HttpPort = (int)httpPort.Value, UsbipPort = (int)usbPort.Value,
                HttpEnabled = http.Checked, Automatic = automatic.Checked };
            if (action == "uninstall" && MessageBox.Show(this, "卸载 AirDAP 服务及服务程序？\n设备配置、凭据和日志会保留。",
                "卸载 AirDAP", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            busy = true; SetButtons(false);
            Log("正在执行 " + action + "，请等待。停止服务可能需要等待设备操作结束。");
            try { Log(await Task.Run(() => Manager.Execute(action, settings))); }
            catch (Exception e) { Log(ErrorText(e)); MessageBox.Show(this, ErrorText(e), "操作未完成", MessageBoxButtons.OK, MessageBoxIcon.Error); }
            finally { busy = false; }
            await RefreshState();
        }
    }

    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new ManagerWindow());
        }
    }
}
