// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================

#pragma once

class MidiLoopBe30TransportTelemetryProvider : public wil::TraceLoggingProvider
{
    IMPLEMENT_TRACELOGGING_CLASS_WITHOUT_TELEMETRY(
        MidiLoopBe30TransportTelemetryProvider,
        "nerds.de.LoopBe30.Transport",
        // {0a7f8baf-42fc-535e-70ee-e402bcbdbffa}
        // From PS> [System.Diagnostics.Tracing.EventSource]::new("nerds.de.LoopBe30.Transport").Guid
        (0x0a7f8baf,0x42fc,0x535e,0x70,0xee,0xe4,0x02,0xbc,0xbd,0xbf,0xfa))
};

using namespace ATL;

class ATL_NO_VTABLE CMidi2LoopBe30Transport :
    public CComObjectRootEx<CComMultiThreadModel>,
    public CComCoClass<CMidi2LoopBe30Transport, &CLSID_Midi2LoopBe30Transport>,
    public IMidiTransport
{
public:
    CMidi2LoopBe30Transport()
    {
    }

    DECLARE_REGISTRY_RESOURCEID(IDR_MIDI2LOOPBE30TRANSPORT)

    BEGIN_COM_MAP(CMidi2LoopBe30Transport)
        COM_INTERFACE_ENTRY(IMidiTransport)
    END_COM_MAP()

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    STDMETHOD(Activate)(_In_ REFIID, _Out_  void**);

private:
    wil::com_ptr_nothrow<IMidiEndpointManager> m_EndpointManager;

};

OBJECT_ENTRY_AUTO(__uuidof(Midi2LoopBe30Transport), CMidi2LoopBe30Transport)

