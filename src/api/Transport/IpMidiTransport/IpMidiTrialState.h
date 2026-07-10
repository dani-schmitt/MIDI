// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

#include <cstdint>
#include <mutex>

enum class IpMidiTrialStateKind : uint8_t
{
    Retail,
    NotStarted,
    Active,
    Expired,
    Faulted,
};

struct IpMidiTrialStatus
{
    bool IsTrial{};
    IpMidiTrialStateKind State{ IpMidiTrialStateKind::Retail };
    uint32_t RemainingSeconds{};
    bool RebootRequired{};
};

class IpMidiTrialState
{
public:
    HRESULT Initialize() noexcept;
    void Shutdown() noexcept;

    // Starts the one-per-boot evaluation period. Calling this more than once does not
    // move the original deadline.
    HRESULT StartOnFirstPortOpen() noexcept;

    IpMidiTrialStatus GetStatus() noexcept;
    bool IsTrafficAllowed() noexcept;
    DWORD RemainingWaitMilliseconds() noexcept;

    WSAEVENT NotificationEvent() const noexcept { return m_notificationEvent; }
    void HandleRegistryNotification() noexcept;

private:
#ifdef IPMIDI_TRIAL
    HRESULT OpenExistingStateLocked() noexcept;
    HRESULT CreateStateLocked() noexcept;
    HRESULT LoadRecordLocked() noexcept;
    HRESULT ProtectAndWriteRecordLocked(_In_ uint64_t startTick) noexcept;
    HRESULT ArmNotificationLocked() noexcept;
    void RefreshExpirationLocked() noexcept;
    void SetFaultedLocked(_In_ HRESULT reason) noexcept;
#endif

    std::mutex m_mutex;
    IpMidiTrialStateKind m_state{
#ifdef IPMIDI_TRIAL
        IpMidiTrialStateKind::NotStarted
#else
        IpMidiTrialStateKind::Retail
#endif
    };
    uint64_t m_startTick{};
    wil::unique_hkey m_stateKey;
    WSAEVENT m_notificationEvent{ WSA_INVALID_EVENT };
    bool m_notificationArmed{};
};
