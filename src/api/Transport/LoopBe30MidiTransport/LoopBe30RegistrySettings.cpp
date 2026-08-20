// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

namespace
{
    constexpr wchar_t ParametersKey[] = L"SOFTWARE\\nerds.de\\LoopBe30\\Parameters";
    constexpr uint32_t ValidMuteMask = (1u << LOOPBE30_MAX_PORT_COUNT) - 1u;

    bool ReadDword(wchar_t const* name, DWORD& value) noexcept
    {
        wil::unique_hkey key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ParametersKey, 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, key.put()) != ERROR_SUCCESS) return false;
        DWORD type{};
        DWORD size{ sizeof(value) };
        return RegQueryValueExW(key.get(), name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD && size == sizeof(value);
    }

    HRESULT WriteDword(wchar_t const* name, DWORD value) noexcept
    {
        wil::unique_hkey key;
        DWORD disposition{};
        const auto createResult = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ParametersKey, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, key.put(), &disposition);
        if (createResult != ERROR_SUCCESS) return HRESULT_FROM_WIN32(createResult);
        return HRESULT_FROM_WIN32(RegSetValueExW(key.get(), name, 0, REG_DWORD,
            reinterpret_cast<BYTE const*>(&value), sizeof(value)));
    }
}

uint8_t LoopBe30RegistrySettings::ReadWantedPorts() noexcept
{
    DWORD value{};
    if (!ReadDword(L"WantedPorts", value) || value > LOOPBE30_MAX_PORT_COUNT)
        return LOOPBE30_DEFAULT_PORT_COUNT;
    return static_cast<uint8_t>(value);
}

uint32_t LoopBe30RegistrySettings::ReadMuteMask() noexcept
{
    DWORD value{};
    return ReadDword(L"MuteMask", value) ? value & ValidMuteMask : 0;
}

bool LoopBe30RegistrySettings::ReadFeedbackDetectionEnabled() noexcept
{
    DWORD value{};
    return !ReadDword(L"FeedbackDetectionEnabled", value) || value != 0;
}

HRESULT LoopBe30RegistrySettings::WriteActualPorts(uint8_t actualPorts) noexcept
{
    RETURN_HR_IF(E_INVALIDARG, actualPorts > LOOPBE30_MAX_PORT_COUNT);
    return WriteDword(L"ActualPorts", actualPorts);
}

HRESULT LoopBe30RegistrySettings::WriteMuteMask(uint32_t muteMask) noexcept
{
    return WriteDword(L"MuteMask", muteMask & ValidMuteMask);
}

HRESULT LoopBe30RegistrySettings::WriteFeedbackDetectionEnabled(bool enabled) noexcept
{
    return WriteDword(L"FeedbackDetectionEnabled", enabled ? 1u : 0u);
}
