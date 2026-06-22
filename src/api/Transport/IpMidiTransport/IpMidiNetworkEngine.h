// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
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
inline constexpr size_t IP_MIDI_MAX_CALLBACKS = 16;

struct IpMidiPacket
{
    std::array<uint8_t, IP_MIDI_MAX_DATAGRAM_SIZE> Bytes{};
    uint16_t Length{};
    LONGLONG Timestamp{};
};

class IpMidiNetworkEngine
{
public:
    HRESULT Start();
    void Shutdown();

    HRESULT QueueOutgoingUmp(_In_reads_bytes_(size) PVOID message, _In_ UINT size, _In_ LONGLONG timestamp);
    HRESULT RegisterCallback(_In_ IMidiCallback* callback, _Out_ uint64_t* registrationId);
    void UnregisterCallback(_In_ uint64_t registrationId);

private:
    struct CallbackRegistration
    {
        uint64_t Id{};
        wil::com_ptr_nothrow<IMidiCallback> Callback;
    };

    HRESULT CreateSockets();
    void CloseSockets();
    void RefreshLocalIpv4Addresses();
    bool IsLocalAddress(_In_ ULONG address) const;

    void SenderWorker(_In_ std::stop_token stopToken);
    void ReceiverWorker(_In_ std::stop_token stopToken);
    void DispatcherWorker(_In_ std::stop_token stopToken);
    void DispatchIncomingPacket(_In_ IpMidiPacket const& packet);

    bool TryEnqueueOutgoing(_In_ IpMidiPacket const& packet);
    bool TryEnqueueIncoming(_In_ IpMidiPacket const& packet);

    std::mutex m_lifecycleMutex;
    std::mutex m_outgoingMutex;
    std::mutex m_incomingMutex;
    std::mutex m_converterMutex;
    std::mutex m_callbackMutex;
    std::mutex m_callbackDispatchMutex;

    std::condition_variable m_outgoingWakeup;
    std::condition_variable m_incomingWakeup;

    boost::circular_buffer<IpMidiPacket> m_outgoingQueue{ IP_MIDI_QUEUE_CAPACITY };
    boost::circular_buffer<IpMidiPacket> m_incomingQueue{ IP_MIDI_QUEUE_CAPACITY };

    std::array<CallbackRegistration, IP_MIDI_MAX_CALLBACKS> m_callbacks{};
    uint64_t m_nextCallbackRegistrationId{ 1 };

    std::array<ULONG, 32> m_localIpv4Addresses{};
    uint8_t m_localIpv4AddressCount{};

    SOCKET m_receiveSocket{ INVALID_SOCKET };
    SOCKET m_sendSocket{ INVALID_SOCKET };
    ip_mreq m_multicastMembership{};
    bool m_winsockStarted{ false };
    bool m_started{ false };
    std::atomic<bool> m_stopping{ false };

    std::jthread m_senderThread;
    std::jthread m_receiverThread;
    std::jthread m_dispatcherThread;

    umpToBytestream m_umpToByteStream;
    bytestreamToUMP m_byteStreamToUmp;

    std::atomic<uint64_t> m_outgoingDropCount{};
    std::atomic<uint64_t> m_incomingDropCount{};
    std::atomic<uint64_t> m_unsupportedOutgoingCount{};
    std::atomic<uint64_t> m_malformedIncomingCount{};
};
