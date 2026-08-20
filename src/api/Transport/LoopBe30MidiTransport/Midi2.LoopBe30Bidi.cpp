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
CMidi2LoopBe30Bidi::Initialize(
    LPCWSTR endpointId,
    PTRANSPORTCREATIONPARAMS,
    DWORD *,
    IMidiCallback * Callback,
    LONGLONG Context,
    GUID /* SessionId */
)
{
    TraceLoggingWrite(
        MidiLoopBe30TransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(endpointId, "endpoint id")
        );

    RETURN_HR_IF_NULL(E_INVALIDARG, Callback);
    m_callback = Callback;
    m_callbackContext = Context;
    m_endpointId = internal::NormalizeEndpointInterfaceIdWStringCopy(endpointId);

    HRESULT hr = S_OK;

    // TODO: This should use SWD properties and not a string search

    if (internal::EndpointInterfaceIdContainsString(m_endpointId, MIDI_LOOPBE30_INSTANCE_ID_PREFIX))
    {
        TraceLoggingWrite(
            MidiLoopBe30TransportTelemetryProvider::Provider(),
            MIDI_TRACE_EVENT_INFO,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingLevel(WINEVENT_LEVEL_INFO),
            TraceLoggingPointer(this, "this"),
            TraceLoggingWideString(L"Initializing Side-A Bidi", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingWideString(m_endpointId.c_str(), "endpoint id")
        );

        m_device = TransportState::Current().GetEndpointTable()->GetDeviceById(endpointId);
        RETURN_HR_IF_NULL(E_INVALIDARG, m_device);
        m_portIndex = m_device->Definition->PortIndex;

        m_engine = TransportState::Current().GetEngine();
        RETURN_HR_IF_NULL(E_UNEXPECTED, m_engine);
        // This is the first operation that represents a client opening a UMP or
        // legacy WinMM port. Service startup and configuration queries do not start
        // the Trial evaluation period.
        m_engine->NotifyPortOpened();
        // Match the proven ipMIDI callback chain. The engine calls this Bidi
        // object's IMidiCallback implementation on its MTA worker; Callback()
        // then forwards to the MIDI Service callback using the original service
        // context captured above.
        RETURN_IF_FAILED(m_engine->RegisterCallback(
            m_portIndex, static_cast<IMidiCallback*>(this), 0, &m_callbackRegistrationId));

    }
    else
    {
        // we don't understand this endpoint id

        TraceLoggingWrite(
            MidiLoopBe30TransportTelemetryProvider::Provider(),
            MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
            TraceLoggingPointer(this, "this"),
            TraceLoggingWideString(L"We don't understand the endpoint Id", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingWideString(m_endpointId.c_str(), "endpoint id")
        );

        return E_FAIL;
    }

    return hr;
}

HRESULT
CMidi2LoopBe30Bidi::Shutdown()
{
    TraceLoggingWrite(
        MidiLoopBe30TransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(m_endpointId.c_str(), "endpoint id")
        );

    if (m_engine != nullptr)
    {
        m_engine->UnregisterCallback(m_portIndex, m_callbackRegistrationId);
        m_engine.reset();
    }

    m_callbackRegistrationId = 0;
    m_callback.reset();
    m_callbackContext = 0;
    m_device.reset();

    return S_OK;
}

#pragma push_macro("SendMessage")
#undef SendMessage
_Use_decl_annotations_
HRESULT
CMidi2LoopBe30Bidi::SendMidiMessage(
    MessageOptionFlags optionFlags,
    PVOID Message,
    UINT Size,
    LONGLONG Position
)
{

    RETURN_HR_IF_NULL(E_INVALIDARG, Message);
    RETURN_HR_IF_NULL(E_UNEXPECTED, m_engine);
    return m_engine->QueueOutgoingUmp(m_portIndex, optionFlags, Message, Size, Position);
}
#pragma pop_macro("SendMessage")

_Use_decl_annotations_
HRESULT CMidi2LoopBe30Bidi::Callback(
    MessageOptionFlags optionFlags,
    PVOID message,
    UINT size,
    LONGLONG position,
    LONGLONG /*context*/)
{
    RETURN_HR_IF_NULL(E_UNEXPECTED, m_callback);
    return m_callback->Callback(optionFlags, message, size, position, m_callbackContext);
}


