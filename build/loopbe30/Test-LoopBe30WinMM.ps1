[CmdletBinding()]
param(
    [string] $PortName = '02. Internal MIDI',
    [ValidateRange(1, 120)]
    [int] $DurationSeconds = 5,
    [ValidateRange(1, 1000)]
    [int] $SendIntervalMilliseconds = 100,

    [switch] $EchoInput
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class LoopBe30WinMmSmoke
{
    private const uint CallbackFunction = 0x00030000;
    private const uint MimData = 0x3C3;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MidiInCaps
    {
        public ushort ManufacturerId;
        public ushort ProductId;
        public uint DriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
        public uint Support;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MidiOutCaps
    {
        public ushort ManufacturerId;
        public ushort ProductId;
        public uint DriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
        public ushort Technology;
        public ushort Voices;
        public ushort Notes;
        public ushort ChannelMask;
        public uint Support;
    }

    private delegate void MidiInCallback(
        IntPtr handle,
        uint message,
        UIntPtr instance,
        UIntPtr parameter1,
        UIntPtr parameter2);

    [DllImport("winmm.dll")]
    private static extern uint midiInGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
    private static extern uint midiInGetDevCapsW(UIntPtr deviceId, ref MidiInCaps caps, uint size);
    [DllImport("winmm.dll")]
    private static extern uint midiInOpen(out IntPtr handle, uint deviceId, MidiInCallback callback, UIntPtr instance, uint flags);
    [DllImport("winmm.dll")]
    private static extern uint midiInStart(IntPtr handle);
    [DllImport("winmm.dll")]
    private static extern uint midiInStop(IntPtr handle);
    [DllImport("winmm.dll")]
    private static extern uint midiInReset(IntPtr handle);
    [DllImport("winmm.dll")]
    private static extern uint midiInClose(IntPtr handle);

    [DllImport("winmm.dll")]
    private static extern uint midiOutGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
    private static extern uint midiOutGetDevCapsW(UIntPtr deviceId, ref MidiOutCaps caps, uint size);
    [DllImport("winmm.dll")]
    private static extern uint midiOutOpen(out IntPtr handle, uint deviceId, IntPtr callback, UIntPtr instance, uint flags);
    [DllImport("winmm.dll")]
    private static extern uint midiOutShortMsg(IntPtr handle, uint message);
    [DllImport("winmm.dll")]
    private static extern uint midiOutReset(IntPtr handle);
    [DllImport("winmm.dll")]
    private static extern uint midiOutClose(IntPtr handle);

    private static uint FindInput(string exactName)
    {
        var matches = new List<uint>();
        for (uint index = 0; index < midiInGetNumDevs(); ++index)
        {
            var caps = new MidiInCaps();
            if (midiInGetDevCapsW((UIntPtr)index, ref caps, (uint)Marshal.SizeOf<MidiInCaps>()) == 0 &&
                String.Equals(caps.Name, exactName, StringComparison.Ordinal)) matches.Add(index);
        }
        if (matches.Count != 1) throw new InvalidOperationException(
            "Expected exactly one WinMM input named '" + exactName + "', found " + matches.Count + ".");
        return matches[0];
    }

    private static uint FindOutput(string exactName)
    {
        var matches = new List<uint>();
        for (uint index = 0; index < midiOutGetNumDevs(); ++index)
        {
            var caps = new MidiOutCaps();
            if (midiOutGetDevCapsW((UIntPtr)index, ref caps, (uint)Marshal.SizeOf<MidiOutCaps>()) == 0 &&
                String.Equals(caps.Name, exactName, StringComparison.Ordinal)) matches.Add(index);
        }
        if (matches.Count != 1) throw new InvalidOperationException(
            "Expected exactly one WinMM output named '" + exactName + "', found " + matches.Count + ".");
        return matches[0];
    }

    public static int Run(string exactName, int durationSeconds, int sendIntervalMilliseconds, bool echoInput)
    {
        uint inputId = FindInput(exactName);
        uint outputId = FindOutput(exactName);
        int received = 0;
        int echoed = 0;
        IntPtr output = IntPtr.Zero;
        MidiInCallback callback = delegate(IntPtr handle, uint message, UIntPtr instance, UIntPtr parameter1, UIntPtr parameter2)
        {
            if (message == MimData)
            {
                int count = Interlocked.Increment(ref received);
                if (echoInput && count < 100000 && output != IntPtr.Zero &&
                    midiOutShortMsg(output, unchecked((uint)parameter1.ToUInt64())) == 0)
                    Interlocked.Increment(ref echoed);
            }
        };

        IntPtr input = IntPtr.Zero;
        uint result = midiInOpen(out input, inputId, callback, UIntPtr.Zero, CallbackFunction);
        if (result != 0) throw new InvalidOperationException("midiInOpen failed: " + result);
        try
        {
            result = midiInStart(input);
            if (result != 0) throw new InvalidOperationException("midiInStart failed: " + result);
            result = midiOutOpen(out output, outputId, IntPtr.Zero, UIntPtr.Zero, 0);
            if (result != 0) throw new InvalidOperationException("midiOutOpen failed: " + result);
            try
            {
                int sends = echoInput ? 1 : Math.Max(1, durationSeconds * 1000 / sendIntervalMilliseconds);
                for (int index = 0; index < sends; ++index)
                {
                    result = midiOutShortMsg(output, 0x00643C90u);
                    if (result != 0) throw new InvalidOperationException("midiOutShortMsg failed: " + result);
                    Thread.Sleep(sendIntervalMilliseconds);
                }
                Thread.Sleep(250);
                Console.WriteLine("Port='{0}' inputId={1} outputId={2} seeds={3} echoed={4} received={5}",
                    exactName, inputId, outputId, sends, echoed, received);
                if (echoInput) return received > 0 ? 0 : 1;
                return received == sends ? 0 : 1;
            }
            finally
            {
                if (output != IntPtr.Zero)
                {
                    midiOutReset(output);
                    midiOutClose(output);
                }
            }
        }
        finally
        {
            if (input != IntPtr.Zero)
            {
                midiInStop(input);
                midiInReset(input);
                midiInClose(input);
            }
            GC.KeepAlive(callback);
        }
    }
}
'@

$result = [LoopBe30WinMmSmoke]::Run(
    $PortName, $DurationSeconds, $SendIntervalMilliseconds, $EchoInput.IsPresent)
if ($result -ne 0) {
    throw "WinMM loopback delivery failed for '$PortName'."
}
