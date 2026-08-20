// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

#include <cstdint>
#include <mutex>

enum class LoopBe30TrialStateKind : uint8_t
{
    Retail,
    NotStarted,
    Active,
    Expired,
    Faulted,
};

struct LoopBe30TrialStatus
{
    bool IsTrial{};
    LoopBe30TrialStateKind State{ LoopBe30TrialStateKind::Retail };
    uint32_t RemainingSeconds{};
    bool RebootRequired{};
};

class LoopBe30TrialState
{
public:
    HRESULT Initialize() noexcept;
    void Shutdown() noexcept;

    // Starts the one-per-boot evaluation period. Calling this more than once does not
    // move the original deadline.
    HRESULT StartOnFirstPortOpen() noexcept;

    LoopBe30TrialStatus GetStatus() noexcept;
    bool IsTrafficAllowed() noexcept;
    DWORD RemainingWaitMilliseconds() noexcept;

    WSAEVENT NotificationEvent() const noexcept { return m_notificationEvent; }
    void HandleRegistryNotification() noexcept;

private:
#ifdef LOOPBE30_TRIAL
    HRESULT OpenExistingStateLocked() noexcept;
    HRESULT CreateStateLocked() noexcept;
    HRESULT LoadRecordLocked() noexcept;
    HRESULT ProtectAndWriteRecordLocked(_In_ uint64_t startTick) noexcept;
    HRESULT ArmNotificationLocked() noexcept;
    void RefreshExpirationLocked() noexcept;
    void SetFaultedLocked(_In_ HRESULT reason) noexcept;
#endif

    std::mutex m_mutex;
    LoopBe30TrialStateKind m_state{
#ifdef LOOPBE30_TRIAL
        LoopBe30TrialStateKind::NotStarted
#else
        LoopBe30TrialStateKind::Retail
#endif
    };
    uint64_t m_startTick{};
    wil::unique_hkey m_stateKey;
    WSAEVENT m_notificationEvent{ WSA_INVALID_EVENT };
    bool m_notificationArmed{};
};

