// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================


#include "pch.h"

_Use_decl_annotations_
HRESULT
CMidi2IpMidiBidi::Initialize(
    LPCWSTR endpointId,
    PTRANSPORTCREATIONPARAMS,
    DWORD *,
    IMidiCallback * Callback,
    LONGLONG Context,
    GUID /* SessionId */
)
{
    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(endpointId, "endpoint id")
        );

    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(Context);
    m_endpointId = internal::NormalizeEndpointInterfaceIdWStringCopy(endpointId);

    HRESULT hr = S_OK;

    // TODO: This should use SWD properties and not a string search

    if (internal::EndpointInterfaceIdContainsString(m_endpointId, MIDI_IP_MIDI_INSTANCE_ID_PREFIX))
    {
        TraceLoggingWrite(
            MidiIpMidiTransportTelemetryProvider::Provider(),
            MIDI_TRACE_EVENT_INFO,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingLevel(WINEVENT_LEVEL_INFO),
            TraceLoggingPointer(this, "this"),
            TraceLoggingWideString(L"Initializing Side-A Bidi", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingWideString(m_endpointId.c_str(), "endpoint id")
        );

        m_device = TransportState::Current().GetEndpointTable()->GetDeviceById(endpointId);
        RETURN_HR_IF_NULL(E_INVALIDARG, m_device);

    }
    else
    {
        // we don't understand this endpoint id

        TraceLoggingWrite(
            MidiIpMidiTransportTelemetryProvider::Provider(),
            MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
            TraceLoggingPointer(this, "this"),
            TraceLoggingWideString(L"We don't understand the endpoint Id", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingWideString(m_endpointId.c_str(), "endpoint id")
        );

        return E_FAIL;
    }

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Unable to find matching device in device table", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingWideString(m_endpointId.c_str(), "endpoint id")
    );

    return hr;
}

HRESULT
CMidi2IpMidiBidi::Shutdown()
{
    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(m_endpointId.c_str(), "endpoint id")
        );

    m_device.reset();

    return S_OK;
}

#pragma push_macro("SendMessage")
#undef SendMessage
_Use_decl_annotations_
HRESULT
CMidi2IpMidiBidi::SendMidiMessage(
    MessageOptionFlags optionFlags,
    PVOID Message,
    UINT Size,
    LONGLONG Position
)
{

    UNREFERENCED_PARAMETER(optionFlags);
    UNREFERENCED_PARAMETER(Message);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Position);

    return S_OK;
}
#pragma pop_macro("SendMessage")

_Use_decl_annotations_
HRESULT
CMidi2IpMidiBidi::Callback(
    MessageOptionFlags /*optionFlags*/ ,
    PVOID /*Message*/ ,
    UINT /*Size*/ ,
    LONGLONG /*Position*/ ,
    LONGLONG /*Context*/
)
{

    return S_OK;
}
