using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Management;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace AirDAP.Manager
{
    internal static class ManagerTests
    {
        private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
        private static void Reject(Action action)
        {
            try { action(); } catch (InvalidOperationException) { return; }
            throw new Exception("Unsafe input was accepted");
        }
        [STAThread]
        private static int Main(string[] args)
        {
            if (args.Length > 0 && args[0] == "--echo")
            {
                foreach (string argument in args.Skip(1)) Console.WriteLine(Convert.ToBase64String(Encoding.UTF8.GetBytes(argument)));
                return 0;
            }
            if (args.Length == 1 && args[0] == "--system-test")
            {
                try { SystemTest(); return 0; }
                catch (Exception e) { Console.Error.WriteLine(e); return 1; }
            }
            try
            {
                foreach (int port in new[] { 0, -1, 65536 }) Reject(() => new Settings { HttpPort = port }.Validate());
                Reject(() => new Settings { UsbipPort = 8080 }.Validate());
                var settings = new Settings { HttpPort = 18080, UsbipPort = 3243 };
                string exe = @"C:\Program Files\AirDAP\airdap-service.exe", data = @"C:\ProgramData\AirDAP";
                string command = Manager.Command(exe, data, "AirDAP", settings);
                var parsed = Manager.ParseCommand(command, exe, data, "AirDAP");
                Check(parsed.HttpPort == 18080 && parsed.UsbipPort == 3243 && parsed.Url == "http://airdap.localhost:18080", "Selected port lost");
                foreach (string bad in new[] { command + " --extra", command.Replace("airdap-service.exe", "python.exe"),
                    command.Replace("--http-port 18080", "--http-port 0"), command.Replace(data, data + "-other"),
                    command.Replace("--service-name AirDAP", "--service-name Foreign") })
                    Reject(() => Manager.ParseCommand(bad, exe, data, "AirDAP"));
                string legacy = Manager.Command(exe, data, "AirDAPNative", new Settings { HttpEnabled = false });
                Check(!Manager.ParseCommand(legacy, exe, data, "AirDAPNative").HttpEnabled, "Legacy no-http lost");
                Console.WriteLine("PASS: port validation, installation ownership, legacy command compatibility");

                string[] values = { "", "plain", "with spaces", @"C:\path with space\", "a\"b", "\\\"x\\", "中文", "$(echo unsafe); &whoami" };
                using (var process = Process.Start(new ProcessStartInfo(Assembly.GetExecutingAssembly().Location,
                    "--echo " + string.Join(" ", values.Select(Manager.Quote))) {
                    UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true }))
                {
                    foreach (string value in values) Check(process.StandardOutput.ReadLine() == Convert.ToBase64String(Encoding.UTF8.GetBytes(value)), "Argument quoting corrupted data");
                    process.WaitForExit(); Check(process.ExitCode == 0, "Argument child failed");
                }
                Console.WriteLine("PASS: real Windows process argument roundtrip");
                string junction = Path.Combine(Path.GetTempPath(), "airdap-manager-link-" + Guid.NewGuid().ToString("N"));
                string target = junction + "-target";
                Directory.CreateDirectory(target);
                try
                {
                    string create = "New-Item -ItemType Junction -Path '" + junction.Replace("'", "''") +
                        "' -Target '" + target.Replace("'", "''") + "' | Out-Null";
                    SystemCommand(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), @"WindowsPowerShell\v1.0\powershell.exe"),
                        "-NoProfile", "-NonInteractive", "-EncodedCommand", Convert.ToBase64String(Encoding.Unicode.GetBytes(create)));
                    Reject(() => Manager.NoReparse(junction));
                    Directory.Delete(target);
                    Reject(() => Manager.NoReparse(junction));
                }
                finally
                {
                    // Delete only the test-created junction, without traversing its target.
                    Directory.Delete(junction);
                    if (Directory.Exists(target)) Directory.Delete(target);
                }
                Console.WriteLine("PASS: real NTFS junction and dangling junction rejected");
                using (var payload = Manager.Payload())
                {
                    Check(payload.ReadByte() == 'M' && payload.ReadByte() == 'Z', "Missing embedded Windows executable");
                    Check(Manager.PayloadHash().Length == 64, "Payload hash invalid");
                }
                Application.EnableVisualStyles();
                using (var form = new ManagerWindow())
                using (var bitmap = new Bitmap(form.Width, form.Height))
                {
                    form.ShowInTaskbar = false;
                    form.Opacity = 0;
                    form.Show();
                    var deadline = DateTime.UtcNow.AddSeconds(5);
                    while (DateTime.UtcNow < deadline) { Application.DoEvents(); System.Threading.Thread.Sleep(20); }
                    form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
                    if (args.Length > 0) bitmap.Save(args[0]);
                }
                Console.WriteLine("PASS: embedded payload and WinForms layout creation (no system modifications)");
                return 0;
            }
            catch (Exception e) { Console.Error.WriteLine(e); return 1; }
        }

        private static int FreePort()
        {
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start(); int port = ((IPEndPoint)listener.LocalEndpoint).Port; listener.Stop(); return port;
        }
        private static void SystemCommand(string executable, params string[] arguments)
        {
            using (var process = Process.Start(new ProcessStartInfo(executable, string.Join(" ", arguments.Select(Manager.Quote))) {
                UseShellExecute = false, CreateNoWindow = true }))
            {
                Check(process.WaitForExit(180000), "System test child timed out; inspect the service before retrying");
                Check(process.ExitCode == 0, "System test child failed: " + executable);
            }
        }
        private static void Healthy()
        {
            var installation = Manager.Inspect();
            Check(installation != null && installation.State == "Running", "Service not Running");
            Check(Manager.InstalledHash(installation) == Manager.PayloadHash(), "Installed binary changed unexpectedly");
            var request = (HttpWebRequest)WebRequest.Create("http://127.0.0.1:" + installation.Settings.HttpPort + "/");
            request.Proxy = null; request.Timeout = 5000;
            using (var response = (HttpWebResponse)request.GetResponse()) Check(response.StatusCode == HttpStatusCode.OK, "Web not ready");
        }
        private static void SystemTest()
        {
            Check(new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator),
                "Run --system-test from an elevated terminal. This test installs and removes a real AirDAP service.");
            using (var mutex = new Mutex(false, @"Global\AirDAP.ServiceManager"))
            {
                Check(mutex.WaitOne(0), "Another manager is running");
                try
                {
                    Check(Manager.Inspect() == null, "Refusing to change an existing service");
                    Check(!Directory.Exists(Manager.ProgramDirectory) && !Directory.Exists(Manager.DataDirectory), "Refusing to change existing program/data directories");
                    string marker = Path.Combine(Manager.DataDirectory, "manager-system-test.marker"), owner = Guid.NewGuid().ToString();
                    var settings = new Settings { HttpPort = FreePort(), UsbipPort = FreePort(), Automatic = false };
                    while (settings.HttpPort == settings.UsbipPort) settings.UsbipPort = FreePort();
                    try
                    {
                        Manager.Execute("install", settings);
                        File.WriteAllText(marker, owner);
                        Healthy(); Console.WriteLine("PASS: real SCM install and HTTP readiness");
                        Manager.Execute("stop", settings); Check(Manager.Inspect().State == "Stopped", "Stop failed");
                        Manager.Execute("start", settings); Manager.Execute("restart", settings); Healthy();
                        settings.HttpPort = FreePort(); while (settings.HttpPort == settings.UsbipPort) settings.HttpPort = FreePort();
                        settings.Automatic = true;
                        Manager.Execute("configure", settings);
                        Check(Manager.Inspect().Settings.Automatic && Manager.Inspect().Settings.HttpPort == settings.HttpPort, "Configuration not persisted");
                        Healthy();
                        Manager.Execute("update", settings); Healthy();
                        Check(File.ReadAllText(marker) == owner, "Update lost data");
                        string probe = Path.Combine(Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location), "AirDAP-FailureProbe.exe");
                        SystemCommand(probe); Healthy(); Check(File.ReadAllText(marker) == owner, "Rollback lost data");
                        Console.WriteLine("PASS: start/stop/restart, selected port, update and failed-update rollback");

                        // Create the precise legacy registration without moving or copying its data.
                        Manager.Execute("stop", settings);
                        string sc = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "sc.exe");
                        string legacy = Manager.Command(Manager.Inspect().Executable, Manager.DataDirectory, Manager.LegacyName, settings);
                        SystemCommand(sc, "delete", Manager.ServiceName);
                        SystemCommand(sc, "create", Manager.LegacyName, "binPath=", legacy, "start=", "demand");
                        Manager.Execute("start", settings); Healthy();
                        SystemCommand(probe); Healthy(); Check(Manager.Inspect().Name == Manager.LegacyName, "Failed migration lost old registration");
                        Manager.Execute("update", settings); Healthy();
                        Check(Manager.Inspect().Name == Manager.ServiceName, "Legacy service name not migrated");
                        Manager.Execute("uninstall", settings);
                        Check(Manager.Inspect() == null && !Directory.Exists(Manager.ProgramDirectory), "Uninstall left service/program");
                        Check(File.ReadAllText(marker) == owner, "Uninstall deleted user data");
                        Console.WriteLine("PASS: legacy migration, migration rollback, uninstall retaining data");
                    }
                    finally
                    {
                        // Leave all state for diagnosis on failure; remove only successful test-owned data.
                        if (Manager.Inspect() == null && File.Exists(marker) && File.ReadAllText(marker) == owner)
                        {
                            string[] allowed = { "manager-system-test.marker", "service.log", "service.lock" };
                            var files = Directory.GetFileSystemEntries(Manager.DataDirectory);
                            Check(files.All(p => File.Exists(p) && allowed.Contains(Path.GetFileName(p))), "Unexpected test data; retained for inspection");
                            foreach (string path in files) { Manager.NoReparse(path); File.Delete(path); }
                            Directory.Delete(Manager.DataDirectory);
                        }
                    }
                }
                finally { mutex.ReleaseMutex(); }
            }
        }
    }
}
