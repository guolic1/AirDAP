using System;

namespace AirDAP.Manager
{
    // Test-only payload: accepts the version check but cannot register with SCM.
    internal static class FailureFixture
    {
        private static int Main(string[] args)
        {
            if (args.Length == 1 && args[0] == "--version") { Console.WriteLine("airdap-service failure-fixture"); return 0; }
            return 1;
        }
    }
#if FAILURE_PROBE
    internal static class FailureProbe
    {
        private static int Main()
        {
            try { Manager.Execute("update", new Settings()); }
            catch (InvalidOperationException e)
            {
                if (e.Message.StartsWith("操作失败，已恢复原程序与设置：", StringComparison.Ordinal)) return 0;
                Console.Error.WriteLine(e); return 1;
            }
            Console.Error.WriteLine("Broken payload update unexpectedly succeeded.");
            return 1;
        }
    }
#endif
}
