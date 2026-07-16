// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <boost/circular_buffer.hpp>

#pragma warning(push)
#pragma warning(disable : 4996)
#include <libmidi2/bytestreamToUMP.h>
#include <libmidi2/umpToBytestream.h>
#pragma warning(pop)

inline constexpr size_t IP_MIDI_MAX_DATAGRAM_SIZE = 1280;
inline constexpr size_t IP_MIDI_QUEUE_CAPACITY = 256;
inline constexpr size_t IP_MIDI_MAX_CALLBACKS_PER_PORT = 16;
inline constexpr uint32_t IP_MIDI_OVERFLOW_TRIP_COUNT = 64;
inline constexpr uint64_t IP_MIDI_OVERFLOW_WINDOW_MILLISECONDS = 2000;

struct IpMidiNetworkStatus
{
    uint32_t SafetyMuteMask{};
    uint32_t EffectiveMuteMask{};
    uint32_t OverflowMask{};
    uint32_t ReceiveFailureMask{};
    uint32_t SendFailureMask{};
    uint32_t Generation{};
};

struct IpMidiPacket
{
    std::array<uint8_t, IP_MIDI_MAX_DATAGRAM_SIZE> Bytes{};
    uint16_t Length{};
    LONGLONG Timestamp{};
};

class IpMidiNetworkEngine
{
public:
    using TrialMuteAppliedCallback = std::function<HRESULT()>;
    using SafetyMuteAppliedCallback = std::function<HRESULT(uint8_t)>;

    HRESULT Initialize(_In_ bool loopbackEnabled);
    HRESULT PreparePort(_In_ uint8_t portIndex, _In_ bool muted);
    void RemoveLastPreparedPort();
    HRESULT Start();
    void Shutdown();

    uint8_t PortCount() const noexcept { return m_portCount; }

    HRESULT QueueOutgoingUmp(
        _In_ uint8_t portIndex,
        _In_reads_bytes_(size) PVOID message,
        _In_ UINT size,
        _In_ LONGLONG timestamp);
    HRESULT RegisterCallback(_In_ uint8_t portIndex, _In_ IMidiCallback* callback, _Out_ uint64_t* registrationId);
    void UnregisterCallback(_In_ uint8_t portIndex, _In_ uint64_t registrationId);
    void NotifyPortOpened() noexcept;
    IpMidiTrialStatus GetTrialStatus() noexcept;
    bool TrialMuteLocked() noexcept;
    void SetTrialMuteAppliedCallback(_In_ TrialMuteAppliedCallback callback);
    void SetSafetyMuteAppliedCallback(_In_ SafetyMuteAppliedCallback callback);

    HRESULT SetLoopbackEnabled(_In_ bool enabled);
    bool LoopbackEnabled() const noexcept { return m_loopbackEnabled.load(); }
    HRESULT SetPortMuted(_In_ uint8_t portIndex, _In_ bool muted);
    void RestorePortConfiguredMuteState(_In_ uint8_t portIndex, _In_ bool muted) noexcept;
    void RestorePortSafetyMute(_In_ uint8_t portIndex, _In_ IpMidiNetworkStatus const& previousStatus) noexcept;
    bool IsPortConfiguredMuted(_In_ uint8_t portIndex) const noexcept;
    bool IsPortEffectivelyMuted(_In_ uint8_t portIndex) const noexcept;
    uint32_t ConfiguredMuteMask() const noexcept;
    uint32_t EffectiveMuteMask() const noexcept;
    IpMidiNetworkStatus GetNetworkStatus() const noexcept;

private:
    struct CallbackRegistration
    {
        uint64_t Id{};
        wil::com_ptr_nothrow<IMidiCallback> Callback;
    };

    struct PortContext
    {
        explicit PortContext(uint8_t index) :
            PortIndex(index),
            UdpPort(static_cast<uint16_t>(IP_MIDI_BASE_UDP_PORT + index)),
            OutgoingQueue(IP_MIDI_QUEUE_CAPACITY),
            IncomingQueue(IP_MIDI_QUEUE_CAPACITY)
        {
            ByteStreamToUmp.defaultGroup = 0;
        }

        uint8_t PortIndex{};
        uint16_t UdpPort{};
        SOCKET ReceiveSocket{ INVALID_SOCKET };
        WSAEVENT ReceiveEvent{ WSA_INVALID_EVENT };
        ip_mreq MulticastMembership{};

        boost::circular_buffer<IpMidiPacket> OutgoingQueue;
        boost::circular_buffer<IpMidiPacket> IncomingQueue;

        std::mutex OutgoingConverterMutex;
        std::mutex IncomingConverterMutex;
        std::mutex SendMutex;
        std::mutex CallbackMutex;
        std::mutex CallbackDispatchMutex;
        std::array<CallbackRegistration, IP_MIDI_MAX_CALLBACKS_PER_PORT> Callbacks{};
        uint64_t NextCallbackRegistrationId{ 1 };

        umpToBytestream UmpToByteStream;
        bytestreamToUMP ByteStreamToUmp;

        std::atomic<bool> ConfiguredMuted{};
        std::atomic<bool> SafetyMuted{};
        std::atomic<bool> Muted{};

        std::array<uint64_t, IP_MIDI_OVERFLOW_TRIP_COUNT> OutgoingOverflowTimestamps{};
        size_t OutgoingOverflowTimestampHead{};
        size_t OutgoingOverflowTimestampCount{};
        std::array<uint64_t, IP_MIDI_OVERFLOW_TRIP_COUNT> IncomingOverflowTimestamps{};
        size_t IncomingOverflowTimestampHead{};
        size_t IncomingOverflowTimestampCount{};

        std::atomic<uint64_t> OutgoingDropCount{};
        std::atomic<uint64_t> IncomingDropCount{};
        std::atomic<uint64_t> UnsupportedOutgoingCount{};
        std::atomic<uint64_t> MalformedIncomingCount{};
    };

    HRESULT CreateSharedResources();
    HRESULT CreateConfiguredSendSocket(_Out_ SOCKET& sendSocket);
    HRESULT RecreateSendSocket();
    HRESULT SendPacketWithRecovery(_In_ sockaddr_in const& destination, _In_ IpMidiPacket const& packet);
    HRESULT CreateReceiveResources(_In_ PortContext& port);
    HRESULT RecoverReceiveResources(_In_ PortContext& port);
    void CloseReceiveResources(_In_ PortContext& port);
    void CloseSharedResources();
    void ResetPortData(_In_ PortContext& port);
    void RefreshLocalIpv4Addresses();
    bool IsLocalAddress(_In_ ULONG address) const;

    bool TryEnqueueOutgoing(_In_ PortContext& port, _In_ IpMidiPacket const& packet);
    bool TryEnqueueIncoming(_In_ PortContext& port, _In_ IpMidiPacket const& packet);
    bool TryDequeueOutgoing(_Out_ uint8_t& portIndex, _Out_ IpMidiPacket& packet);
    bool TryDequeueIncoming(_Out_ uint8_t& portIndex, _Out_ IpMidiPacket& packet);
    bool HasOutgoingPackets() const;
    bool HasIncomingPackets() const;

    void SenderWorker(_In_ std::stop_token stopToken);
    void ReceiverWorker(_In_ std::stop_token stopToken);
    void DispatcherWorker(_In_ std::stop_token stopToken);
    void DispatchIncomingPacket(_In_ PortContext& port, _In_ IpMidiPacket const& packet);
    bool RefreshTrialEnforcement() noexcept;
    void ApplyTrialTrafficBlock() noexcept;
    void ApplyTrialMuteOnReceiverThread() noexcept;
    void NotifyTrialMuteApplied() noexcept;
    void RequestSafetyMute(_In_ uint8_t portIndex, _In_ uint32_t reasonMask) noexcept;
    void ApplyPendingSafetyMutesOnReceiverThread() noexcept;
    void ClearSafetyFault(_In_ PortContext& port) noexcept;
    void NotifySafetyMuteApplied(_In_ uint8_t portIndex) noexcept;

    PortContext* GetPort(_In_ uint8_t portIndex) const noexcept;

    std::mutex m_lifecycleMutex;
    std::mutex m_sendSocketMutex;
    std::mutex m_reconfigureMutex;
    std::condition_variable m_reconfigureComplete;
    mutable std::mutex m_outgoingMutex;
    mutable std::mutex m_incomingMutex;
    std::condition_variable m_outgoingWakeup;
    std::condition_variable m_incomingWakeup;
    std::mutex m_trialCallbackMutex;
    std::mutex m_safetyCallbackMutex;

    std::array<std::unique_ptr<PortContext>, IP_MIDI_MAX_PORT_COUNT> m_ports{};
    uint8_t m_portCount{};
    uint8_t m_nextOutgoingPort{};
    uint8_t m_nextIncomingPort{};

    std::array<ULONG, 32> m_localIpv4Addresses{};
    uint8_t m_localIpv4AddressCount{};

    SOCKET m_sendSocket{ INVALID_SOCKET };
    WSAEVENT m_stopEvent{ WSA_INVALID_EVENT };
    WSAEVENT m_reconfigureEvent{ WSA_INVALID_EVENT };
    bool m_reconfigurePending{};
    bool m_reconfigureFinished{};
    uint8_t m_reconfigurePortIndex{};
    bool m_reconfigureMuted{};
    HRESULT m_reconfigureResult{ S_OK };
    bool m_winsockStarted{};
    bool m_initialized{};
    std::atomic<bool> m_started{};
    std::atomic<bool> m_stopping{ true };
    std::atomic<bool> m_loopbackEnabled{};
    std::atomic<bool> m_trialTrafficBlocked{};
    std::atomic<bool> m_trialMutePending{};
    std::atomic<uint32_t> m_safetyMutePendingMask{};
    std::atomic<uint32_t> m_safetyMuteMask{};
    std::atomic<uint32_t> m_overflowMask{};
    std::atomic<uint32_t> m_receiveFailureMask{};
    std::atomic<uint32_t> m_sendFailureMask{};
    std::atomic<uint32_t> m_networkStatusGeneration{};
    TrialMuteAppliedCallback m_trialMuteAppliedCallback;
    SafetyMuteAppliedCallback m_safetyMuteAppliedCallback;
    IpMidiTrialState m_trialState;

    std::jthread m_senderThread;
    std::jthread m_receiverThread;
    std::jthread m_dispatcherThread;
};
