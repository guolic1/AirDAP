using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Management;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.ServiceProcess;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace AirDAP.Manager
{
    internal sealed class Settings
    {
        public int HttpPort = 8080;
        public int UsbipPort = 3242;
        public bool HttpEnabled = true;
        public bool Automatic = true;

        public void Validate()
        {
            if (HttpPort < 1 || HttpPort > 65535 || UsbipPort < 1 || UsbipPort > 65535)
                throw new InvalidOperationException("端口必须在 1–65535 之间。");
            if (HttpEnabled && HttpPort == UsbipPort)
                throw new InvalidOperationException("Web 与 USB/IP 端口不能相同。");
        }
        public string Url { get { return "http://airdap.localhost:" + HttpPort; } }
    }

    internal sealed class Installation
    {
        public string Name;
        public string State;
        public string Command;
        public string StartMode;
        public string ProgramDirectory;
        public string DataDirectory;
        public Settings Settings;
        public string Executable { get { return Path.Combine(ProgramDirectory, "airdap-service.exe"); } }
    }

    internal static class Manager
    {
        internal const string ServiceName = "AirDAP";
        internal const string LegacyName = "AirDAPNative";
        internal static readonly string ProgramRoot = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        internal static readonly string DataRoot = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
        internal static readonly string ProgramDirectory = Path.Combine(ProgramRoot, "AirDAP");
        internal static readonly string DataDirectory = Path.Combine(DataRoot, "AirDAP");

        // sc.exe receives separately quoted arguments, never a shell command or user executable path.
        internal static string Quote(string value)
        {
            return "\"" + Regex.Replace(value, "(\\\\*)\"", "$1$1\\\"").TrimEnd('\\')
                + new string('\\', value.Reverse().TakeWhile(c => c == '\\').Count() * 2) + "\"";
        }

        internal static string Command(string executable, string data, string name, Settings settings)
        {
            settings.Validate();
            return Quote(executable) + " --windows-service --service-name " + name +
                " --data-dir " + Quote(data) + " --http-port " + settings.HttpPort +
                " --usbip-port " + settings.UsbipPort + (settings.HttpEnabled ? "" : " --no-http");
        }

        internal static Settings ParseCommand(string command, string executable, string data, string name)
        {
            string prefix = Quote(executable) + " --windows-service --service-name " + name +
                " --data-dir " + Quote(data);
            var match = Regex.Match(command, "^" + Regex.Escape(prefix) +
                @" --http-port ([0-9]{1,5}) --usbip-port ([0-9]{1,5})( --no-http)?$", RegexOptions.IgnoreCase);
            if (!match.Success)
                throw new InvalidOperationException("发现同名服务，但路径或启动参数不属于受支持的 AirDAP 安装。未修改该服务。");
            var result = new Settings { HttpPort = int.Parse(match.Groups[1].Value),
                UsbipPort = int.Parse(match.Groups[2].Value), HttpEnabled = !match.Groups[3].Success };
            result.Validate();
            return result;
        }

        private static Installation Find(string name)
        {
            using (var searcher = new ManagementObjectSearcher(
                "SELECT Name,State,PathName,StartMode FROM Win32_Service WHERE Name='" + name + "'"))
            using (var results = searcher.Get())
            {
                foreach (ManagementObject item in results)
                    using (item) return new Installation { Name = name, State = (string)item["State"],
                        Command = (string)item["PathName"], StartMode = (string)item["StartMode"] };
            }
            return null;
        }

        internal static Installation Inspect()
        {
            var current = Find(ServiceName);
            var legacy = Find(LegacyName);
            if (current != null && legacy != null)
                throw new InvalidOperationException("同时存在 AirDAP 和 AirDAPNative 服务。请先核对重复安装，管理器未修改任何服务。");
            var result = current ?? legacy;
            if (result == null) return null;
            // Migration keeps the protected legacy data directory instead of copying credentials.
            foreach (string leaf in new[] { "AirDAP", "AirDAPNative" })
            {
                string program = Path.Combine(ProgramRoot, leaf), data = Path.Combine(DataRoot, leaf);
                try { result.Settings = ParseCommand(result.Command, Path.Combine(program, "airdap-service.exe"), data, result.Name); }
                catch (InvalidOperationException) { continue; }
                result.ProgramDirectory = program;
                result.DataDirectory = data;
                result.Settings.Automatic = result.StartMode == "Auto";
                return result;
            }
            throw new InvalidOperationException("同名服务不是此管理器支持的 Rust 服务（可能是旧 Python 服务或自定义安装）。请先用原安装工具迁移或移除它。");
        }

        internal static string Hash(Stream source)
        {
            using (var hash = SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(source)).Replace("-", "").ToLowerInvariant();
        }
        internal static string InstalledHash(Installation installation)
        {
            NoReparse(installation.Executable);
            using (var source = File.OpenRead(installation.Executable)) return Hash(source);
        }
        internal static Stream Payload()
        {
            var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("AirDAP.Service.exe");
            if (stream == null) throw new InvalidOperationException("管理器缺少内嵌服务，请重新下载完整发行版。");
            return stream;
        }
        internal static string PayloadHash()
        {
            using (var source = Payload()) return Hash(source);
        }

        internal static void NoReparse(string path)
        {
            string full = Path.GetFullPath(path);
            for (string item = full; !string.IsNullOrEmpty(item); item = Path.GetDirectoryName(item))
            {
                try
                {
                    if ((File.GetAttributes(item) & FileAttributes.ReparsePoint) != 0)
                        throw new InvalidOperationException("拒绝通过符号链接或目录联接操作安装文件：" + item);
                }
                // Missing components are expected before first installation; dangling links still have attributes.
                catch (FileNotFoundException) { }
                catch (DirectoryNotFoundException) { }
            }
        }
        private static DirectorySecurity PrivateAcl()
        {
            var security = new DirectorySecurity();
            security.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
            security.SetAccessRuleProtection(true, false);
            foreach (var sid in new[] { WellKnownSidType.LocalSystemSid, WellKnownSidType.BuiltinAdministratorsSid })
                security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid, null), FileSystemRights.FullControl,
                    InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
            return security;
        }
        private static void PrivateDirectory(string path)
        {
            NoReparse(path);
            if (!Directory.Exists(path)) Directory.CreateDirectory(path, PrivateAcl());
            // Do not silently broaden existing ACLs or traverse a legacy credentials tree.
            var security = Directory.GetAccessControl(path);
            var owner = security.GetOwner(typeof(SecurityIdentifier));
            using (var identity = WindowsIdentity.GetCurrent())
                if (!owner.Equals(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null)) &&
                    !owner.Equals(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null)) &&
                    !owner.Equals(identity.User))
                    throw new InvalidOperationException("目录所有者不是 SYSTEM、Administrators 或当前管理员，请先核对所有权：" + path);
            var rules = security.GetAccessRules(true, true, typeof(SecurityIdentifier));
            foreach (FileSystemAccessRule rule in rules)
                if (rule.AccessControlType == AccessControlType.Allow &&
                    !rule.IdentityReference.Equals(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null)) &&
                    !rule.IdentityReference.Equals(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null)))
                    throw new InvalidOperationException("目录权限不符合要求，请先由管理员核对：" + path);
        }
        private static void CheckPorts(Settings settings)
        {
            settings.Validate();
            foreach (int port in settings.HttpEnabled ? new[] { settings.HttpPort, settings.UsbipPort } : new[] { settings.UsbipPort })
            {
                var listener = new TcpListener(IPAddress.Loopback, port);
                listener.ExclusiveAddressUse = true;
                try { listener.Start(); }
                catch (SocketException e) { throw new InvalidOperationException("端口 " + port + " 已占用或不可用，请选择其他端口。", e); }
                finally { listener.Stop(); }
            }
        }

        private static string Run(string executable, params string[] args)
        {
            using (var process = new Process())
            {
                process.StartInfo = new ProcessStartInfo(executable, string.Join(" ", args.Select(Quote))) {
                    UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
                process.Start();
                Task<string> stdout = process.StandardOutput.ReadToEndAsync(), stderr = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(30000))
                    throw new System.TimeoutException("系统命令执行超时，请刷新状态后核对结果；没有重复执行该操作。");
                if (process.ExitCode != 0)
                    throw new InvalidOperationException(Path.GetFileName(executable) + " 失败（" + process.ExitCode + "）：" + stdout.Result + stderr.Result);
                return stdout.Result;
            }
        }
        private static void Sc(params string[] args)
        {
            Run(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "sc.exe"), args);
        }
        private static void Wait(string name, ServiceControllerStatus state)
        {
            using (var service = new ServiceController(name))
                try { service.WaitForStatus(state, TimeSpan.FromSeconds(state == ServiceControllerStatus.Stopped ? 120 : 45)); }
                catch (System.ServiceProcess.TimeoutException e) {
                    throw new InvalidOperationException("等待服务" + (state == ServiceControllerStatus.Stopped ? "停止" : "启动") +
                        "超时。未强制结束进程；若正在 OTA，请等待操作结束后刷新。", e);
                }
        }
        private static void Stop(string name)
        {
            using (var service = new ServiceController(name))
            {
                if (service.Status == ServiceControllerStatus.Stopped) return;
                if (service.Status != ServiceControllerStatus.StopPending) service.Stop();
            }
            Wait(name, ServiceControllerStatus.Stopped);
        }
        private static void Start(string name)
        {
            using (var service = new ServiceController(name))
            {
                if (service.Status == ServiceControllerStatus.Running) return;
                service.Start();
            }
            Wait(name, ServiceControllerStatus.Running);
        }
        private static void DeleteService(string name)
        {
            Sc("delete", name);
            var deadline = DateTime.UtcNow.AddSeconds(15);
            while (Find(name) != null)
            {
                if (DateTime.UtcNow >= deadline) throw new InvalidOperationException("服务已标记删除，请关闭 services.msc 等占用服务句柄的程序后重试。");
                Thread.Sleep(200);
            }
        }
        private static void Create(string name, string command, string mode)
        {
            Sc("create", name, "binPath=", command, "start=", mode, "DisplayName=", "AirDAP");
        }
        private static void Recovery(string name)
        {
            Sc("description", name, "AirDAP USB/IP and local device management");
            Sc("failure", name, "reset=", "86400", "actions=", "restart/5000/restart/15000/restart/60000");
        }
        private static string Mode(Installation installation)
        {
            return installation.StartMode == "Auto" ? "auto" : installation.StartMode == "Disabled" ? "disabled" : "demand";
        }
        private static string Stage(string directory)
        {
            PrivateDirectory(directory);
            string candidate = Path.Combine(directory, "airdap-service-" + Guid.NewGuid().ToString("N") + ".exe");
            using (var payload = Payload())
            using (var target = new FileStream(candidate, FileMode.CreateNew, FileAccess.Write, FileShare.None)) payload.CopyTo(target);
            try
            {
                string version = Run(candidate, "--version").Trim();
                if (!version.StartsWith("airdap-service ", StringComparison.Ordinal)) throw new InvalidOperationException("内嵌程序版本校验失败。");
                return candidate;
            }
            catch { File.Delete(candidate); throw; }
        }

        internal static string Execute(string action, Settings settings)
        {
            using (var identity = WindowsIdentity.GetCurrent())
                if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
                    throw new InvalidOperationException("服务管理需要管理员权限，请右键以管理员身份运行。");
            using (var mutex = new Mutex(false, @"Global\AirDAP.ServiceManager"))
            {
                bool locked;
                try { locked = mutex.WaitOne(0); } catch (AbandonedMutexException) { locked = true; }
                if (!locked) throw new InvalidOperationException("另一个管理器正在修改服务，请等待其完成。");
                try { return ExecuteLocked(action, settings); }
                finally { mutex.ReleaseMutex(); }
            }
        }
        private static string ExecuteLocked(string action, Settings settings)
        {
            var installed = Inspect();
            if (action == "install")
            {
                if (installed != null) throw new InvalidOperationException("服务已经安装，请使用更新。");
                CheckPorts(settings);
                NoReparse(ProgramDirectory);
                if (Directory.Exists(ProgramDirectory) && Directory.EnumerateFileSystemEntries(ProgramDirectory).Any())
                    throw new InvalidOperationException("安装目录已有文件，未覆盖。请先核对 " + ProgramDirectory);
                PrivateDirectory(DataDirectory);
                string staged = Stage(ProgramDirectory), executable = Path.Combine(ProgramDirectory, "airdap-service.exe");
                bool registered = false;
                try
                {
                    File.Move(staged, executable);
                    Create(ServiceName, Command(executable, DataDirectory, ServiceName, settings), settings.Automatic ? "auto" : "demand");
                    registered = true;
                    Recovery(ServiceName);
                    Start(ServiceName);
                }
                catch (Exception failure)
                {
                    if (failure is System.TimeoutException)
                        throw new InvalidOperationException("安装命令结果尚未确认，程序文件已保留。请等待系统命令结束并检查服务状态。", failure);
                    try
                    {
                        if (registered) { Stop(ServiceName); DeleteService(ServiceName); }
                        // An uncertain create result must not leave a service pointing at a removed binary.
                        if (Find(ServiceName) == null) { File.Delete(executable); File.Delete(staged); }
                    }
                    catch (Exception rollback) { throw new AggregateException("安装失败，清理也未完成，请保留当前文件并检查服务状态。", failure, rollback); }
                    throw;
                }
                return "安装成功，服务已启动。";
            }
            if (installed == null) throw new InvalidOperationException("尚未安装 AirDAP 服务。");
            NoReparse(installed.Executable);
            PrivateDirectory(installed.ProgramDirectory);
            PrivateDirectory(installed.DataDirectory);
            if (action == "start") { Start(installed.Name); return "服务已启动。"; }
            if (action == "stop") { Stop(installed.Name); return "服务已停止。"; }
            if (action == "restart") { Stop(installed.Name); Start(installed.Name); return "服务已重启。"; }
            if (action == "uninstall")
            {
                Stop(installed.Name);
                DeleteService(installed.Name);
                File.Delete(installed.Executable);
                if (!Directory.EnumerateFileSystemEntries(installed.ProgramDirectory).Any()) Directory.Delete(installed.ProgramDirectory);
                return "服务和程序已卸载。设备配置、凭据和日志保留于 " + installed.DataDirectory;
            }
            if (action != "update" && action != "configure") throw new InvalidOperationException("未知管理操作。");
            if (installed.Name == LegacyName && action == "configure")
                throw new InvalidOperationException("请先更新旧版服务，将服务名迁移为 AirDAP。");
            bool running = installed.State == "Running";
            string candidate = action == "update" ? Stage(installed.ProgramDirectory) : null;
            string backup = installed.Executable + ".backup-" + Guid.NewGuid().ToString("N");
            bool replaced = false, migrated = false, oldDeleted = false, configured = false;
            try
            {
                Stop(installed.Name);
                if (action == "configure")
                {
                    CheckPorts(settings);
                    configured = true;
                    Sc("config", installed.Name, "binPath=", Command(installed.Executable, installed.DataDirectory, installed.Name, settings),
                        "start=", settings.Automatic ? "auto" : "demand");
                }
                else
                {
                    File.Replace(candidate, installed.Executable, backup);
                    replaced = true;
                    if (installed.Name == LegacyName)
                    {
                        // Keep the original registration until the replacement is known to start.
                        Create(ServiceName, Command(installed.Executable, installed.DataDirectory, ServiceName, installed.Settings), Mode(installed));
                        migrated = true;
                        Recovery(ServiceName);
                    }
                }
                string name = migrated ? ServiceName : installed.Name;
                if (running) Start(name);
                if (migrated) { DeleteService(installed.Name); oldDeleted = true; }
            }
            catch (Exception failure)
            {
                if (failure is System.TimeoutException)
                    throw new InvalidOperationException("系统命令结果尚未确认，未自动恢复或删除备份。请检查服务状态及 " + backup, failure);
                try
                {
                    if (migrated) { Stop(ServiceName); DeleteService(ServiceName); }
                    else Stop(installed.Name);
                    if (replaced) File.Replace(backup, installed.Executable, null);
                    if (oldDeleted) { Create(installed.Name, installed.Command, Mode(installed)); Recovery(installed.Name); }
                    if (configured) Sc("config", installed.Name, "binPath=", installed.Command, "start=", Mode(installed));
                    if (running) Start(installed.Name);
                }
                catch (Exception rollback) { throw new AggregateException("操作失败，自动恢复也失败。请保留备份 " + backup + " 并检查服务状态。", failure, rollback); }
                throw new InvalidOperationException("操作失败，已恢复原程序与设置：" + failure.Message, failure);
            }
            finally { if (candidate != null && File.Exists(candidate)) File.Delete(candidate); }
            if (replaced && File.Exists(backup)) File.Delete(backup);
            return action == "configure" ? "设置已保存，保留原运行/停止状态。" : "服务已更新，原配置和凭据已保留。";
        }

        internal static string DriverStatus()
        {
            bool client = File.Exists(Path.Combine(ProgramRoot, "USBip", "usbip.exe"));
            using (var searcher = new ManagementObjectSearcher("SELECT Name FROM Win32_SystemDriver WHERE Name='usbip2_ude'"))
            using (var results = searcher.Get())
                return client && results.Count > 0 ? "USB/IP 客户端和驱动已安装" : "USB/IP 客户端或驱动未检测到，USB 映射前需单独安装";
        }
    }
}
