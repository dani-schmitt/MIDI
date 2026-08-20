// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================


#pragma once

#include "pch.h"


_Use_decl_annotations_
std::shared_ptr<MidiLoopBe30Device> MidiLoopBe30DeviceTable::GetDevice(
    winrt::guid const& associationId)
{
    if (m_devices.find(associationId) != m_devices.end())
    {
        return m_devices[associationId];
    }
    else
    {
        return nullptr;
    }
}


_Use_decl_annotations_
std::shared_ptr<MidiLoopBe30Device> MidiLoopBe30DeviceTable::GetDeviceById(
    std::wstring const& endpointDeviceId)
{
    auto cleanId = internal::NormalizeEndpointInterfaceIdWStringCopy(endpointDeviceId);

    for (auto const& [key, device] : m_devices)
    {
        if (cleanId == internal::NormalizeEndpointInterfaceIdWStringCopy(device->Definition->CreatedEndpointInterfaceId))
        {
            return device;
        }
    }

    return nullptr;
}


_Use_decl_annotations_
void MidiLoopBe30DeviceTable::SetDevice(
    winrt::guid const& associationId,
    std::shared_ptr<MidiLoopBe30Device> device)
{
    TraceLoggingWrite(
        MidiLoopBe30TransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingPointer(this, "this"),
        TraceLoggingWideString(L"Enter", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingGuid(associationId, "association id")
    );

    m_devices[associationId] = device;
}

_Use_decl_annotations_
void MidiLoopBe30DeviceTable::RemoveDevice(
    winrt::guid const& associationId)
{
    if (auto device = m_devices.find(associationId); device != m_devices.end())
    {
        device->second->Shutdown();

        m_devices.erase(associationId);
        device->second.reset();
    }
}

_Use_decl_annotations_
bool MidiLoopBe30DeviceTable::IsUniqueIdentifierInUse(
    std::wstring const& uniqueIdentifier)
{
    auto cleanId = internal::ToLowerTrimmedWStringCopy(uniqueIdentifier);

    for (auto const& [key, device] : m_devices)
    {
        if (cleanId == internal::ToLowerTrimmedWStringCopy(device->Definition->EndpointUniqueIdentifier))
        {
            return true;
        }
    }

    return false;
}

