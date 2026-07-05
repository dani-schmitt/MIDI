// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

namespace
{
    constexpr wchar_t ParametersKey[] = L"SOFTWARE\\nerds.de\\ipMIDI\\Parameters";
}

uint8_t IpMidiRegistrySettings::ReadWantedPorts() noexcept
{
    wil::unique_hkey key;
    const auto openResult = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        ParametersKey,
        0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        key.put());

    if (openResult != ERROR_SUCCESS)
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"WantedPorts registry key unavailable; using default", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingUInt32(openResult, "registry error"));
        return IP_MIDI_DEFAULT_PORT_COUNT;
    }

    DWORD value{};
    DWORD valueType{};
    DWORD valueSize{ sizeof(value) };
    const auto queryResult = RegQueryValueExW(
        key.get(), L"WantedPorts", nullptr, &valueType, reinterpret_cast<BYTE*>(&value), &valueSize);

    if (queryResult != ERROR_SUCCESS || valueType != REG_DWORD || valueSize != sizeof(value) ||
        value < IP_MIDI_MIN_PORT_COUNT || value > IP_MIDI_MAX_PORT_COUNT)
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"WantedPorts registry value invalid; using default", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingUInt32(queryResult, "registry error"),
            TraceLoggingUInt32(value, "registry value"),
            TraceLoggingUInt32(valueType, "registry type"));
        return IP_MIDI_DEFAULT_PORT_COUNT;
    }

    return static_cast<uint8_t>(value);
}

HRESULT IpMidiRegistrySettings::WriteActualPorts(uint8_t actualPorts) noexcept
{
    wil::unique_hkey key;
    DWORD disposition{};
    const auto createResult = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        ParametersKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_WOW64_64KEY,
        nullptr,
        key.put(),
        &disposition);

    if (createResult != ERROR_SUCCESS)
    {
        return HRESULT_FROM_WIN32(createResult);
    }

    const DWORD value = actualPorts;
    const auto setResult = RegSetValueExW(
        key.get(), L"ActualPorts", 0, REG_DWORD, reinterpret_cast<BYTE const*>(&value), sizeof(value));
    return HRESULT_FROM_WIN32(setResult);
}
