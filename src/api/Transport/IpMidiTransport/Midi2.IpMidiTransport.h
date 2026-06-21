// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================

#pragma once

class MidiIpMidiTransportTelemetryProvider : public wil::TraceLoggingProvider
{
    IMPLEMENT_TRACELOGGING_CLASS_WITH_MICROSOFT_TELEMETRY(
        MidiIpMidiTransportTelemetryProvider,
        "Microsoft.Windows.Midi2.IpMidiTransport",
        // {76682b52-a931-45d8-b044-3e3a09f091e5}
        // From PS> [System.Diagnostics.Tracing.EventSource]::new("Microsoft.Windows.Midi2.IpMidiTransport").Guid
        (0x76682b52,0xa931,0x45d8,0xb0,0x44,0x3e,0x3a,0x09,0xf0,0x91,0xe5))
};

using namespace ATL;

class ATL_NO_VTABLE CMidi2IpMidiTransport :
    public CComObjectRootEx<CComMultiThreadModel>,
    public CComCoClass<CMidi2IpMidiTransport, &CLSID_Midi2IpMidiTransport>,
    public IMidiTransport
{
public:
    CMidi2IpMidiTransport()
    {
    }

    DECLARE_REGISTRY_RESOURCEID(IDR_MIDI2IPMIDITRANSPORT)

    BEGIN_COM_MAP(CMidi2IpMidiTransport)
        COM_INTERFACE_ENTRY(IMidiTransport)
    END_COM_MAP()

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    STDMETHOD(Activate)(_In_ REFIID, _Out_  void**);

private:
    wil::com_ptr_nothrow<IMidiEndpointManager> m_EndpointManager;

};

OBJECT_ENTRY_AUTO(__uuidof(Midi2IpMidiTransport), CMidi2IpMidiTransport)
