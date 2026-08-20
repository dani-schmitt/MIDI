using Microsoft.Win32;
using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using WixToolset.Dtf.WindowsInstaller;

namespace custom_actions
{
    public class CustomActions
    {
        private const uint SC_MANAGER_CONNECT = 0x0001;
        private const uint SERVICE_QUERY_CONFIG = 0x0001;
        private const uint SERVICE_DISABLED = 0x00000004;
        private const uint SPDRP_HARDWAREID = 0x00000001;
        private const uint SPDRP_SERVICE = 0x00000004;
        private const uint COINIT_MULTITHREADED = 0x0;
        private const uint CLSCTX_INPROC_SERVER = 0x1;
        private const int ERROR_INSUFFICIENT_BUFFER = 122;
        private const int ERROR_NO_MORE_ITEMS = 259;
        private static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct QUERY_SERVICE_CONFIG
        {
            public uint dwServiceType;
            public uint dwStartType;
            public uint dwErrorControl;
            public IntPtr lpBinaryPathName;
            public IntPtr lpLoadOrderGroup;
            public uint dwTagId;
            public IntPtr lpDependencies;
            public IntPtr lpServiceStartName;
            public IntPtr lpDisplayName;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct SP_DEVINFO_DATA
        {
            public uint cbSize;
            public Guid ClassGuid;
            public uint DevInst;
            public IntPtr Reserved;
        }

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr OpenSCManager(string machineName, string databaseName, uint desiredAccess);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr OpenService(IntPtr serviceManager, string serviceName, uint desiredAccess);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool QueryServiceConfig(IntPtr service, IntPtr queryServiceConfig, uint bufferSize, out uint bytesNeeded);

        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool CloseServiceHandle(IntPtr handle);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, string enumerator, IntPtr hwndParent, uint flags);

        [DllImport("setupapi.dll", SetLastError = true)]
        private static extern bool SetupDiEnumDeviceInfo(IntPtr deviceInfoSet, uint memberIndex, ref SP_DEVINFO_DATA deviceInfoData);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool SetupDiGetDeviceRegistryProperty(
            IntPtr deviceInfoSet,
            ref SP_DEVINFO_DATA deviceInfoData,
            uint property,
            out uint propertyRegDataType,
            byte[] propertyBuffer,
            uint propertyBufferSize,
            out uint requiredSize);

        [DllImport("setupapi.dll", SetLastError = true)]
        private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

        [DllImport("ole32.dll")]
        private static extern int CoInitializeEx(IntPtr reserved, uint coInit);

        [DllImport("ole32.dll")]
        private static extern void CoUninitialize();

        [DllImport("ole32.dll")]
        private static extern int CoCreateInstance(
            ref Guid classId,
            IntPtr outer,
            uint context,
            ref Guid interfaceId,
            out IntPtr instance);

        private static bool TryGetServiceStartType(string serviceName, out uint startType)
        {
            startType = 0;
            IntPtr serviceManager = OpenSCManager(null, null, SC_MANAGER_CONNECT);
            if (serviceManager == IntPtr.Zero)
            {
                return false;
            }

            try
            {
                IntPtr service = OpenService(serviceManager, serviceName, SERVICE_QUERY_CONFIG);
                if (service == IntPtr.Zero)
                {
                    return false;
                }

                try
                {
                    QueryServiceConfig(service, IntPtr.Zero, 0, out uint bytesNeeded);
                    if (bytesNeeded == 0 || Marshal.GetLastWin32Error() != ERROR_INSUFFICIENT_BUFFER)
                    {
                        return false;
                    }

                    IntPtr buffer = Marshal.AllocHGlobal((int)bytesNeeded);
                    try
                    {
                        if (!QueryServiceConfig(service, buffer, bytesNeeded, out bytesNeeded))
                        {
                            return false;
                        }

                        QUERY_SERVICE_CONFIG config = Marshal.PtrToStructure<QUERY_SERVICE_CONFIG>(buffer);
                        startType = config.dwStartType;
                        return true;
                    }
                    finally
                    {
                        Marshal.FreeHGlobal(buffer);
                    }
                }
                finally
                {
                    CloseServiceHandle(service);
                }
            }
            finally
            {
                CloseServiceHandle(serviceManager);
            }
        }

        private static bool RegistryKeyExists(RegistryHive hive, RegistryView view, string path)
        {
            using (RegistryKey baseKey = RegistryKey.OpenBaseKey(hive, view))
            using (RegistryKey key = baseKey.OpenSubKey(path, writable: false))
            {
                return key != null;
            }
        }

        private static bool RegistryValueExists(RegistryHive hive, RegistryView view, string path, string valueName)
        {
            using (RegistryKey baseKey = RegistryKey.OpenBaseKey(hive, view))
            using (RegistryKey key = baseKey.OpenSubKey(path, writable: false))
            {
                return key?.GetValue(valueName, null, RegistryValueOptions.DoNotExpandEnvironmentNames) != null;
            }
        }

        private static bool HasWdmaud2Registration()
        {
            const string drivers32Path = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32";
            using (RegistryKey localMachine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
            using (RegistryKey drivers32 = localMachine.OpenSubKey(drivers32Path, writable: false))
            {
                if (drivers32 == null)
                {
                    return false;
                }

                for (int slot = 0; slot <= 9; ++slot)
                {
                    string valueName = slot == 0 ? "midi" : "midi" + slot;
                    string value = drivers32.GetValue(valueName, string.Empty, RegistryValueOptions.DoNotExpandEnvironmentNames) as string;
                    if (string.Equals(value?.Trim(), "wdmaud2.drv", StringComparison.OrdinalIgnoreCase))
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        private static bool IsWindows11OrNewer()
        {
            const string currentVersionPath = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
            using (RegistryKey localMachine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
            using (RegistryKey currentVersion = localMachine.OpenSubKey(currentVersionPath, writable: false))
            {
                string buildText = currentVersion?.GetValue("CurrentBuildNumber", string.Empty) as string;
                return int.TryParse(buildText, out int buildNumber) && buildNumber >= 22000;
            }
        }

        private static bool IsMidiServiceComRegistered()
        {
            const string midiServiceTransportClsid = @"CLSID\{2BA15E4E-5417-4A66-85B8-2B2260EFBC84}\InProcServer32";
            return RegistryKeyExists(RegistryHive.ClassesRoot, RegistryView.Registry64, midiServiceTransportClsid);
        }

        private static bool CanActivateMidiServiceTransport(Session session)
        {
            Guid classId = new Guid("2BA15E4E-5417-4A66-85B8-2B2260EFBC84");
            Guid unknownId = new Guid("00000000-0000-0000-C000-000000000046");
            int initializeResult = CoInitializeEx(IntPtr.Zero, COINIT_MULTITHREADED);
            bool uninitialize = initializeResult >= 0;
            IntPtr instance = IntPtr.Zero;

            try
            {
                int result = CoCreateInstance(ref classId, IntPtr.Zero, CLSCTX_INPROC_SERVER, ref unknownId, out instance);
                session.Log($"CheckLoopBe30Prerequisites: MIDI service COM activation returned 0x{result:X8}.");
                return result >= 0 && instance != IntPtr.Zero;
            }
            finally
            {
                if (instance != IntPtr.Zero)
                {
                    Marshal.Release(instance);
                }

                if (uninitialize)
                {
                    CoUninitialize();
                }
            }
        }

        private static string GetDeviceProperty(IntPtr deviceInfoSet, ref SP_DEVINFO_DATA deviceInfoData, uint property)
        {
            SetupDiGetDeviceRegistryProperty(
                deviceInfoSet,
                ref deviceInfoData,
                property,
                out uint propertyType,
                null,
                0,
                out uint requiredSize);

            if (requiredSize == 0 || Marshal.GetLastWin32Error() != ERROR_INSUFFICIENT_BUFFER)
            {
                return string.Empty;
            }

            byte[] buffer = new byte[requiredSize];
            if (!SetupDiGetDeviceRegistryProperty(
                deviceInfoSet,
                ref deviceInfoData,
                property,
                out propertyType,
                buffer,
                (uint)buffer.Length,
                out requiredSize))
            {
                return string.Empty;
            }

            return Encoding.Unicode.GetString(buffer).TrimEnd('\0');
        }

        private static bool HasLegacyLoopBe30Device(Session session)
        {
            Guid mediaClassGuid = new Guid("4D36E96C-E325-11CE-BFC1-08002BE10318");
            // A zero flag set returns every installed device in the Media setup class,
            // including non-present devices. Do not use DIGCF_PRESENT here.
            IntPtr deviceInfoSet = SetupDiGetClassDevs(ref mediaClassGuid, null, IntPtr.Zero, 0);
            if (deviceInfoSet == INVALID_HANDLE_VALUE)
            {
                session.Log($"CheckLoopBe30Prerequisites: SetupDiGetClassDevs failed with {Marshal.GetLastWin32Error()}.");
                return false;
            }

            try
            {
                for (uint index = 0; ; ++index)
                {
                    SP_DEVINFO_DATA deviceInfoData = new SP_DEVINFO_DATA
                    {
                        cbSize = (uint)Marshal.SizeOf<SP_DEVINFO_DATA>()
                    };

                    if (!SetupDiEnumDeviceInfo(deviceInfoSet, index, ref deviceInfoData))
                    {
                        int error = Marshal.GetLastWin32Error();
                        if (error != ERROR_NO_MORE_ITEMS)
                        {
                            session.Log($"CheckLoopBe30Prerequisites: SetupDiEnumDeviceInfo failed with {error}.");
                        }
                        break;
                    }

                    string hardwareIds = GetDeviceProperty(deviceInfoSet, ref deviceInfoData, SPDRP_HARDWAREID);
                    string serviceName = GetDeviceProperty(deviceInfoSet, ref deviceInfoData, SPDRP_SERVICE);
                    bool hardwareIdMatches = hardwareIds
                        .Split(new[] { '\0' }, StringSplitOptions.RemoveEmptyEntries)
                        .Any(id => string.Equals(id.Trim(), "*LoopBe30", StringComparison.OrdinalIgnoreCase));

                    if (hardwareIdMatches || string.Equals(serviceName.Trim(), "LoopBe30", StringComparison.OrdinalIgnoreCase))
                    {
                        session.Log($"CheckLoopBe30Prerequisites: Legacy LoopBe30 device detected at index {index}.");
                        return true;
                    }
                }
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(deviceInfoSet);
            }

            return false;
        }

        private static bool HasLegacyLoopBe30Installation(Session session)
        {
            const string uninstallRoot = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall";
            string[] uninstallKeys = { "LoopBe30", "LoopBe30 Trial" };

            foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
            {
                foreach (string uninstallKey in uninstallKeys)
                {
                    if (RegistryKeyExists(RegistryHive.LocalMachine, view, uninstallRoot + "\\" + uninstallKey))
                    {
                        session.Log($"CheckLoopBe30Prerequisites: Legacy uninstall registration found in {view}: {uninstallKey}.");
                        return true;
                    }
                }
            }

            if (RegistryKeyExists(RegistryHive.LocalMachine, RegistryView.Registry64, @"SYSTEM\CurrentControlSet\Services\LoopBe30"))
            {
                session.Log("CheckLoopBe30Prerequisites: Legacy LoopBe30 service registration found.");
                return true;
            }

            return HasLegacyLoopBe30Device(session);
        }

        [CustomAction]
        public static ActionResult CheckLoopBe30Prerequisites(Session session)
        {
            session.Log("CheckLoopBe30Prerequisites: Started");

            try
            {
                bool windowsSupported = IsWindows11OrNewer();
                bool serviceExecutableExists = File.Exists(Path.Combine(Environment.SystemDirectory, "midisrv.exe"));
                bool serviceExists = TryGetServiceStartType("midisrv", out uint serviceStartType);
                bool serviceDisabled = serviceExists && serviceStartType == SERVICE_DISABLED;
                bool wdmaud2Registered = HasWdmaud2Registration();
                bool comRegistered = IsMidiServiceComRegistered();
                bool comActivates = comRegistered && CanActivateMidiServiceTransport(session);
                bool oldDriverFound = HasLegacyLoopBe30Installation(session);

                bool serviceAbsent = !serviceExecutableExists && !serviceExists && !wdmaud2Registered && !comRegistered;
                bool serviceReady = windowsSupported && serviceExecutableExists && serviceExists && !serviceDisabled &&
                    wdmaud2Registered && comRegistered && comActivates;

                session["LOOPBE30_OS_SUPPORTED"] = windowsSupported ? "1" : "0";
                session["LOOPBE30_SERVICE_ABSENT"] = serviceAbsent ? "1" : "0";
                session["LOOPBE30_SERVICE_READY"] = serviceReady ? "1" : "0";
                session["LOOPBE30_SERVICE_INCOMPLETE"] = !serviceAbsent && !serviceReady ? "1" : "0";
                session["LOOPBE30_OLD_DRIVER_FOUND"] = oldDriverFound ? "1" : "0";

                session.Log(
                    "CheckLoopBe30Prerequisites: " +
                    $"WindowsSupported={windowsSupported}; ServiceExe={serviceExecutableExists}; " +
                    $"ServiceExists={serviceExists}; ServiceStart={serviceStartType}; Wdmaud2={wdmaud2Registered}; " +
                    $"ComRegistered={comRegistered}; ComActivates={comActivates}; OldDriver={oldDriverFound}; " +
                    $"ServiceAbsent={serviceAbsent}; ServiceReady={serviceReady}.");

                return ActionResult.Success;
            }
            catch (Exception ex)
            {
                session.Log("ERROR: CheckLoopBe30Prerequisites: Exception " + ex);
                session["LOOPBE30_OS_SUPPORTED"] = "0";
                session["LOOPBE30_SERVICE_ABSENT"] = "0";
                session["LOOPBE30_SERVICE_READY"] = "0";
                session["LOOPBE30_SERVICE_INCOMPLETE"] = "1";
                session["LOOPBE30_OLD_DRIVER_FOUND"] = "0";
                return ActionResult.Success;
            }
        }

        [CustomAction]
        public static ActionResult InitializeLoopBe30RegistryValues(Session session)
        {
            session.Log("InitializeLoopBe30RegistryValues: Started");

            try
            {
                SecurityIdentifier midiServiceSid = (SecurityIdentifier)new NTAccount(
                    "NT SERVICE", "midisrv").Translate(typeof(SecurityIdentifier));

                using (RegistryKey localMachine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
                using (RegistryKey loopBe30 = localMachine.CreateSubKey(
                    @"SOFTWARE\nerds.de\LoopBe30",
                    RegistryKeyPermissionCheck.ReadWriteSubTree))
                using (RegistryKey parameters = loopBe30?.CreateSubKey(
                    "Parameters",
                    RegistryKeyPermissionCheck.ReadWriteSubTree))
                using (RegistryKey runtime = loopBe30?.CreateSubKey(
                    "Runtime",
                    RegistryKeyPermissionCheck.ReadWriteSubTree))
                {
                    if (loopBe30 == null || parameters == null || runtime == null)
                    {
                        session.Log("ERROR: InitializeLoopBe30RegistryValues: Unable to create LoopBe30 registry keys.");
                        return ActionResult.Failure;
                    }

                    SetParametersRegistrySecurity(parameters, midiServiceSid);
                    SetRuntimeRegistrySecurity(runtime, midiServiceSid);

                    EnsureRegistryDword(parameters, "WantedPorts", 2);
                    EnsureRegistryDword(parameters, "ActualPorts", 0);
                    EnsureRegistryDword(parameters, "MuteMask", 0);
                    EnsureRegistryDword(parameters, "FeedbackDetectionEnabled", 1);
                }

                session.Log("InitializeLoopBe30RegistryValues: Completed");
                return ActionResult.Success;
            }
            catch (Exception ex)
            {
                session.Log("ERROR: InitializeLoopBe30RegistryValues: Exception " + ex.ToString());
                return ActionResult.Failure;
            }
        }

        [CustomAction]
        public static ActionResult RemoveLoopBe30RegistryValues(Session session)
        {
            session.Log("RemoveLoopBe30RegistryValues: Started");
            try
            {
                using (RegistryKey localMachine = RegistryKey.OpenBaseKey(
                    RegistryHive.LocalMachine, RegistryView.Registry64))
                {
                    localMachine.DeleteSubKeyTree(@"SOFTWARE\nerds.de\LoopBe30", false);
                }
                session.Log("RemoveLoopBe30RegistryValues: Completed");
                return ActionResult.Success;
            }
            catch (Exception ex)
            {
                session.Log("ERROR: RemoveLoopBe30RegistryValues: Exception " + ex);
                return ActionResult.Failure;
            }
        }

        private static void SetParametersRegistrySecurity(
            RegistryKey key,
            SecurityIdentifier midiServiceSid)
        {
            SecurityIdentifier systemSid =
                new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
            SecurityIdentifier administratorsSid =
                new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
            SecurityIdentifier usersSid =
                new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null);

            RegistrySecurity security = new RegistrySecurity();
            security.SetAccessRuleProtection(true, false);
            security.SetOwner(administratorsSid);
            security.AddAccessRule(CreateRegistryAccessRule(
                systemSid, RegistryRights.FullControl));
            security.AddAccessRule(CreateRegistryAccessRule(
                administratorsSid, RegistryRights.FullControl));
            security.AddAccessRule(CreateRegistryAccessRule(
                midiServiceSid, RegistryRights.FullControl));
            security.AddAccessRule(CreateRegistryAccessRule(
                usersSid, RegistryRights.ReadKey));
            key.SetAccessControl(security);
        }

        private static void SetRuntimeRegistrySecurity(
            RegistryKey key,
            SecurityIdentifier midiServiceSid)
        {
            SecurityIdentifier systemSid =
                new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
            SecurityIdentifier administratorsSid =
                new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);

            RegistrySecurity security = new RegistrySecurity();
            security.SetAccessRuleProtection(true, false);
            security.SetOwner(systemSid);
            security.AddAccessRule(CreateRegistryAccessRule(
                systemSid, RegistryRights.FullControl));
            security.AddAccessRule(CreateRegistryAccessRule(
                administratorsSid, RegistryRights.ReadKey));
            security.AddAccessRule(CreateRegistryAccessRule(
                midiServiceSid, RegistryRights.FullControl));
            key.SetAccessControl(security);
        }

        private static RegistryAccessRule CreateRegistryAccessRule(
            SecurityIdentifier identity,
            RegistryRights rights)
        {
            return new RegistryAccessRule(
                identity,
                rights,
                InheritanceFlags.ContainerInherit,
                PropagationFlags.None,
                AccessControlType.Allow);
        }

        private static void EnsureRegistryDword(RegistryKey key, string valueName, int defaultValue)
        {
            if (key.GetValue(valueName, null, RegistryValueOptions.DoNotExpandEnvironmentNames) == null)
            {
                key.SetValue(valueName, defaultValue, RegistryValueKind.DWord);
            }
        }

    }
}



