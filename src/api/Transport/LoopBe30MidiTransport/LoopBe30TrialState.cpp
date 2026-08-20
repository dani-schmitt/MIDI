// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

#ifdef LOOPBE30_TRIAL
#include <sddl.h>
#include <wincrypt.h>

namespace
{
    constexpr wchar_t RuntimeStateKey[] =
        L"SOFTWARE\\nerds.de\\LoopBe30\\Runtime\\{E458A817-AFD3-4565-AF79-3B46020B073B}";
    constexpr wchar_t RuntimeStateValue[] = L"{3BC4E20E-46AD-47D7-A835-BE4F4182B9B0}";
    constexpr wchar_t MidiServiceAccount[] = L"NT SERVICE\\midisrv";
    constexpr uint64_t EvaluationDurationMilliseconds = 60ull * 60ull * 1000ull;
    constexpr uint32_t EvaluationDurationSeconds = 60u * 60u;
    constexpr uint32_t RecordVersion = 1;
    constexpr uint32_t RecordStarted = 1;

    constexpr std::array<BYTE, 32> ProtectionEntropy
    {
        0xE4, 0x58, 0xA8, 0x17, 0xAF, 0xD3, 0x45, 0x65,
        0xAF, 0x79, 0x3B, 0x46, 0x02, 0x0B, 0x07, 0x3B,
        0x4F, 0x4E, 0x3D, 0x10, 0x27, 0x68, 0x42, 0x9A,
        0x92, 0xF0, 0xAA, 0xA0, 0xE8, 0xE2, 0xAC, 0x5E,
    };

#pragma pack(push, 1)
    struct RuntimeRecord
    {
        uint32_t Version{};
        uint32_t Flags{};
        uint64_t StartTick{};
        GUID Nonce{};
    };
#pragma pack(pop)

    DATA_BLOB EntropyBlob() noexcept
    {
        return DATA_BLOB
        {
            static_cast<DWORD>(ProtectionEntropy.size()),
            const_cast<BYTE*>(ProtectionEntropy.data())
        };
    }

    bool IsZeroGuid(GUID const& value) noexcept
    {
        return InlineIsEqualGUID(value, GUID_NULL) != FALSE;
    }

    HRESULT CreateRuntimeStateSecurityDescriptor(
        _Out_ wil::unique_hlocal& securityDescriptor) noexcept
    {
        try
        {
            DWORD sidSize{};
            DWORD domainSize{};
            SID_NAME_USE sidType{};
            LookupAccountNameW(
                nullptr, MidiServiceAccount, nullptr, &sidSize, nullptr, &domainSize, &sidType);
            RETURN_LAST_ERROR_IF(GetLastError() != ERROR_INSUFFICIENT_BUFFER);

            std::vector<BYTE> sidBytes(sidSize);
            std::vector<wchar_t> domainName(domainSize);
            RETURN_LAST_ERROR_IF(!LookupAccountNameW(
                nullptr,
                MidiServiceAccount,
                sidBytes.data(),
                &sidSize,
                domainName.data(),
                &domainSize,
                &sidType));

            wil::unique_hlocal_string sidString;
            RETURN_LAST_ERROR_IF(!ConvertSidToStringSidW(sidBytes.data(), sidString.put()));

            const std::wstring security =
                L"D:P(A;;KA;;;SY)(A;;KR;;;BA)(A;;KA;;;" +
                std::wstring{ sidString.get() } + L")";

            PSECURITY_DESCRIPTOR descriptor{};
            RETURN_LAST_ERROR_IF(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                security.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
            securityDescriptor.reset(descriptor);
            return S_OK;
        }
        catch (...)
        {
            return wil::ResultFromCaughtException();
        }
    }
}
#endif

HRESULT LoopBe30TrialState::Initialize() noexcept
{
    std::scoped_lock lock(m_mutex);

#ifndef LOOPBE30_TRIAL
    m_state = LoopBe30TrialStateKind::Retail;
    return S_OK;
#else
    if (m_notificationEvent == WSA_INVALID_EVENT)
    {
        m_notificationEvent = WSACreateEvent();
        if (m_notificationEvent == WSA_INVALID_EVENT)
        {
            const auto hr = HRESULT_FROM_WIN32(WSAGetLastError());
            SetFaultedLocked(hr);
            return hr;
        }
    }

    const auto openResult = OpenExistingStateLocked();
    if (openResult == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        openResult == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
    {
        m_state = LoopBe30TrialStateKind::NotStarted;
        m_startTick = 0;
        return S_OK;
    }
    if (FAILED(openResult))
    {
        SetFaultedLocked(openResult);
        return openResult;
    }

    const auto notifyResult = ArmNotificationLocked();
    if (FAILED(notifyResult))
    {
        SetFaultedLocked(notifyResult);
        return notifyResult;
    }

    // Arm before reading so a change in the open-to-read interval is either
    // observed by this read or signalled by the notification event.
    const auto loadResult = LoadRecordLocked();
    if (FAILED(loadResult))
    {
        SetFaultedLocked(loadResult);
        return loadResult;
    }

    // Re-protect an existing record with machine scope. This migrates records
    // written by earlier builds and keeps the Trial state readable if Windows
    // changes the midisrv service account.
    const auto migrateResult = ProtectAndWriteRecordLocked(m_startTick);
    if (FAILED(migrateResult))
    {
        SetFaultedLocked(migrateResult);
        return migrateResult;
    }
    const auto rearmResult = ArmNotificationLocked();
    if (FAILED(rearmResult))
    {
        SetFaultedLocked(rearmResult);
        return rearmResult;
    }

    RefreshExpirationLocked();
    TraceLoggingWrite(MidiLoopBe30TransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"LoopBe30 Trial runtime state restored", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingUInt64(m_startTick, "start tick"),
        TraceLoggingBool(m_state == LoopBe30TrialStateKind::Expired, "expired"));
    return S_OK;
#endif
}

void LoopBe30TrialState::Shutdown() noexcept
{
    std::scoped_lock lock(m_mutex);
    m_notificationArmed = false;
    m_stateKey.reset();
    if (m_notificationEvent != WSA_INVALID_EVENT)
    {
        WSACloseEvent(m_notificationEvent);
        m_notificationEvent = WSA_INVALID_EVENT;
    }
}

HRESULT LoopBe30TrialState::StartOnFirstPortOpen() noexcept
{
#ifndef LOOPBE30_TRIAL
    return S_OK;
#else
    std::scoped_lock lock(m_mutex);
    RefreshExpirationLocked();
    if (m_state == LoopBe30TrialStateKind::Active || m_state == LoopBe30TrialStateKind::Expired)
    {
        return S_OK;
    }
    if (m_state == LoopBe30TrialStateKind::Faulted)
    {
        return E_ACCESSDENIED;
    }

    const auto createResult = CreateStateLocked();
    if (FAILED(createResult))
    {
        SetFaultedLocked(createResult);
        return createResult;
    }

    if (createResult == S_FALSE)
    {
        const auto notifyResult = ArmNotificationLocked();
        if (FAILED(notifyResult))
        {
            SetFaultedLocked(notifyResult);
            return notifyResult;
        }
        const auto loadResult = LoadRecordLocked();
        if (FAILED(loadResult))
        {
            SetFaultedLocked(loadResult);
            return loadResult;
        }
        RefreshExpirationLocked();
        return S_OK;
    }

    const auto startTick = GetTickCount64();
    const auto writeResult = ProtectAndWriteRecordLocked(startTick);
    if (FAILED(writeResult))
    {
        SetFaultedLocked(writeResult);
        return writeResult;
    }

    m_startTick = startTick;
    m_state = LoopBe30TrialStateKind::Active;
    const auto notifyResult = ArmNotificationLocked();
    if (FAILED(notifyResult))
    {
        SetFaultedLocked(notifyResult);
        return notifyResult;
    }
    const auto loadResult = LoadRecordLocked();
    if (FAILED(loadResult))
    {
        SetFaultedLocked(loadResult);
        return loadResult;
    }

    TraceLoggingWrite(MidiLoopBe30TransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"LoopBe30 Trial evaluation period started", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingUInt32(EvaluationDurationSeconds, "duration seconds"));
    return S_OK;
#endif
}

LoopBe30TrialStatus LoopBe30TrialState::GetStatus() noexcept
{
    std::scoped_lock lock(m_mutex);
#ifdef LOOPBE30_TRIAL
    RefreshExpirationLocked();
    LoopBe30TrialStatus status{ true, m_state, 0, false };
    if (m_state == LoopBe30TrialStateKind::NotStarted)
    {
        status.RemainingSeconds = EvaluationDurationSeconds;
    }
    else if (m_state == LoopBe30TrialStateKind::Active)
    {
        const auto elapsed = GetTickCount64() - m_startTick;
        const auto remaining = EvaluationDurationMilliseconds - elapsed;
        status.RemainingSeconds = static_cast<uint32_t>((remaining + 999ull) / 1000ull);
    }
    else if (m_state == LoopBe30TrialStateKind::Expired || m_state == LoopBe30TrialStateKind::Faulted)
    {
        status.RebootRequired = true;
    }
    return status;
#else
    return LoopBe30TrialStatus{ false, LoopBe30TrialStateKind::Retail, 0, false };
#endif
}

bool LoopBe30TrialState::IsTrafficAllowed() noexcept
{
#ifndef LOOPBE30_TRIAL
    return true;
#else
    std::scoped_lock lock(m_mutex);
    RefreshExpirationLocked();
    return m_state == LoopBe30TrialStateKind::NotStarted || m_state == LoopBe30TrialStateKind::Active;
#endif
}

DWORD LoopBe30TrialState::RemainingWaitMilliseconds() noexcept
{
#ifndef LOOPBE30_TRIAL
    return WSA_INFINITE;
#else
    std::scoped_lock lock(m_mutex);
    const auto previousState = m_state;
    RefreshExpirationLocked();
    if (previousState == LoopBe30TrialStateKind::Active && m_state != LoopBe30TrialStateKind::Active)
    {
        return 0;
    }
    if (m_state != LoopBe30TrialStateKind::Active)
    {
        return WSA_INFINITE;
    }

    const auto now = GetTickCount64();
    if (now < m_startTick || now - m_startTick >= EvaluationDurationMilliseconds)
    {
        RefreshExpirationLocked();
        return 0;
    }
    const auto elapsed = now - m_startTick;
    const auto remaining = EvaluationDurationMilliseconds - elapsed;
    return static_cast<DWORD>((std::min)(remaining, static_cast<uint64_t>(MAXDWORD - 1)));
#endif
}

void LoopBe30TrialState::HandleRegistryNotification() noexcept
{
#ifdef LOOPBE30_TRIAL
    std::scoped_lock lock(m_mutex);
    if (m_notificationEvent != WSA_INVALID_EVENT)
    {
        WSAResetEvent(m_notificationEvent);
    }
    if (m_notificationArmed)
    {
        m_notificationArmed = false;
        SetFaultedLocked(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        TraceLoggingWrite(MidiLoopBe30TransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"LoopBe30 Trial runtime state changed unexpectedly", MIDI_TRACE_EVENT_MESSAGE_FIELD));
    }
#endif
}

#ifdef LOOPBE30_TRIAL
HRESULT LoopBe30TrialState::OpenExistingStateLocked() noexcept
{
    m_stateKey.reset();
    const auto result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        RuntimeStateKey,
        0,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_NOTIFY | KEY_WOW64_64KEY,
        m_stateKey.put());
    return HRESULT_FROM_WIN32(result);
}

HRESULT LoopBe30TrialState::CreateStateLocked() noexcept
{
    wil::unique_hlocal securityDescriptor;
    RETURN_IF_FAILED(CreateRuntimeStateSecurityDescriptor(securityDescriptor));
    SECURITY_ATTRIBUTES securityAttributes
    {
        sizeof(SECURITY_ATTRIBUTES),
        securityDescriptor.get(),
        FALSE
    };

    m_stateKey.reset();
    DWORD disposition{};
    const auto createResult = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        RuntimeStateKey,
        0,
        nullptr,
        REG_OPTION_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_NOTIFY | KEY_WOW64_64KEY,
        &securityAttributes,
        m_stateKey.put(),
        &disposition);
    if (createResult != ERROR_SUCCESS)
    {
        return HRESULT_FROM_WIN32(createResult);
    }

    if (disposition == REG_OPENED_EXISTING_KEY)
    {
        return S_FALSE;
    }
    return S_OK;
}

HRESULT LoopBe30TrialState::LoadRecordLocked() noexcept
{
    DWORD valueType{};
    DWORD protectedSize{};
    auto queryResult = RegQueryValueExW(
        m_stateKey.get(), RuntimeStateValue, nullptr, &valueType, nullptr, &protectedSize);
    if (queryResult != ERROR_SUCCESS || valueType != REG_BINARY || protectedSize == 0)
    {
        return HRESULT_FROM_WIN32(queryResult == ERROR_SUCCESS ? ERROR_INVALID_DATA : queryResult);
    }

    try
    {
        std::vector<BYTE> protectedBytes(protectedSize);
        queryResult = RegQueryValueExW(m_stateKey.get(), RuntimeStateValue, nullptr, &valueType,
            protectedBytes.data(), &protectedSize);
        if (queryResult != ERROR_SUCCESS || valueType != REG_BINARY)
        {
            return HRESULT_FROM_WIN32(queryResult == ERROR_SUCCESS ? ERROR_INVALID_DATA : queryResult);
        }

        DATA_BLOB protectedBlob{ protectedSize, protectedBytes.data() };
        auto entropy = EntropyBlob();
        DATA_BLOB plainBlob{};
        if (!CryptUnprotectData(&protectedBlob, nullptr, &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &plainBlob))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        wil::unique_hlocal_ptr<BYTE> plainBytes{ plainBlob.pbData };
        if (plainBlob.cbData != sizeof(RuntimeRecord))
        {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        const auto record = *reinterpret_cast<RuntimeRecord const*>(plainBlob.pbData);
        const auto now = GetTickCount64();
        if (record.Version != RecordVersion || record.Flags != RecordStarted ||
            record.StartTick > now || IsZeroGuid(record.Nonce))
        {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        m_startTick = record.StartTick;
        m_state = LoopBe30TrialStateKind::Active;
        return S_OK;
    }
    catch (...)
    {
        return wil::ResultFromCaughtException();
    }
}

HRESULT LoopBe30TrialState::ProtectAndWriteRecordLocked(uint64_t startTick) noexcept
{
    RuntimeRecord record{ RecordVersion, RecordStarted, startTick, GUID_NULL };
    RETURN_IF_FAILED(CoCreateGuid(&record.Nonce));

    DATA_BLOB plainBlob{ sizeof(record), reinterpret_cast<BYTE*>(&record) };
    auto entropy = EntropyBlob();
    DATA_BLOB protectedBlob{};
    if (!CryptProtectData(&plainBlob, L"LoopBe30 runtime state", &entropy, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN | CRYPTPROTECT_LOCAL_MACHINE, &protectedBlob))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    wil::unique_hlocal_ptr<BYTE> protectedBytes{ protectedBlob.pbData };

    const auto setResult = RegSetValueExW(
        m_stateKey.get(), RuntimeStateValue, 0, REG_BINARY, protectedBlob.pbData, protectedBlob.cbData);
    return HRESULT_FROM_WIN32(setResult);
}

HRESULT LoopBe30TrialState::ArmNotificationLocked() noexcept
{
    if (m_notificationEvent == WSA_INVALID_EVENT || !m_stateKey)
    {
        return E_HANDLE;
    }

    WSAResetEvent(m_notificationEvent);
    const auto result = RegNotifyChangeKeyValue(
        m_stateKey.get(),
        FALSE,
        REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_SECURITY |
            REG_NOTIFY_THREAD_AGNOSTIC,
        m_notificationEvent,
        TRUE);
    m_notificationArmed = result == ERROR_SUCCESS;
    return HRESULT_FROM_WIN32(result);
}

void LoopBe30TrialState::RefreshExpirationLocked() noexcept
{
    if (m_state != LoopBe30TrialStateKind::Active)
    {
        return;
    }

    const auto now = GetTickCount64();
    if (now < m_startTick)
    {
        SetFaultedLocked(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        return;
    }

    if (now - m_startTick >= EvaluationDurationMilliseconds)
    {
        m_state = LoopBe30TrialStateKind::Expired;
        TraceLoggingWrite(MidiLoopBe30TransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"LoopBe30 Trial evaluation period expired", MIDI_TRACE_EVENT_MESSAGE_FIELD));
    }
}

void LoopBe30TrialState::SetFaultedLocked(HRESULT reason) noexcept
{
    m_state = LoopBe30TrialStateKind::Faulted;
    TraceLoggingWrite(MidiLoopBe30TransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"LoopBe30 Trial runtime state faulted", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingHResult(reason, MIDI_TRACE_EVENT_HRESULT_FIELD));
}
#endif

