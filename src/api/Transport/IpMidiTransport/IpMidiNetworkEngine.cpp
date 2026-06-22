// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

namespace
{
    constexpr char IP_MIDI_MULTICAST_ADDRESS[] = "225.0.0.37";
    constexpr uint16_t IP_MIDI_MULTICAST_PORT = 21928;
}

HRESULT IpMidiNetworkEngine::Start()
{
    std::scoped_lock lifecycleLock(m_lifecycleMutex);

    if (m_started)
    {
        return S_OK;
    }

    m_stopping = false;
    RETURN_IF_FAILED(CreateSockets());

    m_byteStreamToUmp.defaultGroup = 0;

    m_senderThread = std::jthread(std::bind_front(&IpMidiNetworkEngine::SenderWorker, this));
    m_receiverThread = std::jthread(std::bind_front(&IpMidiNetworkEngine::ReceiverWorker, this));
    m_dispatcherThread = std::jthread(std::bind_front(&IpMidiNetworkEngine::DispatcherWorker, this));
    m_started = true;

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI network workers started", MIDI_TRACE_EVENT_MESSAGE_FIELD));

    return S_OK;
}

void IpMidiNetworkEngine::Shutdown()
{
    {
        std::scoped_lock callbackDispatchLock(m_callbackDispatchMutex);
        std::scoped_lock lifecycleLock(m_lifecycleMutex);

        if (!m_started)
        {
            return;
        }

        m_stopping = true;
        m_started = false;
    }

    m_outgoingWakeup.notify_all();
    m_incomingWakeup.notify_all();

    if (m_receiveSocket != INVALID_SOCKET)
    {
        shutdown(m_receiveSocket, SD_BOTH);
    }

    m_senderThread.request_stop();
    m_receiverThread.request_stop();
    m_dispatcherThread.request_stop();

    if (m_senderThread.joinable()) m_senderThread.join();
    if (m_receiverThread.joinable()) m_receiverThread.join();
    if (m_dispatcherThread.joinable()) m_dispatcherThread.join();

    CloseSockets();

    {
        std::scoped_lock outgoingLock(m_outgoingMutex);
        m_outgoingQueue.clear();
    }

    {
        std::scoped_lock incomingLock(m_incomingMutex);
        m_incomingQueue.clear();
    }

    {
        std::scoped_lock callbackLock(m_callbackMutex);
        for (auto& registration : m_callbacks)
        {
            registration.Callback.reset();
            registration.Id = 0;
        }
    }

    TraceLoggingWrite(
        MidiIpMidiTransportTelemetryProvider::Provider(),
        MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI network workers stopped", MIDI_TRACE_EVENT_MESSAGE_FIELD));
}

HRESULT IpMidiNetworkEngine::CreateSockets()
{
    WSADATA winsockData{};
    const auto startupResult = WSAStartup(MAKEWORD(2, 2), &winsockData);
    if (startupResult != 0)
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"Winsock initialization failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(startupResult, "winsock error"));
        return HRESULT_FROM_WIN32(startupResult);
    }
    m_winsockStarted = true;

    m_receiveSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_receiveSocket == INVALID_SOCKET)
    {
        const auto error = WSAGetLastError();
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI receive socket creation failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(error, "winsock error"));
        CloseSockets();
        return HRESULT_FROM_WIN32(error);
    }

    int reuseAddress = 1;
    if (setsockopt(m_receiveSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuseAddress), sizeof(reuseAddress)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI SO_REUSEADDR configuration failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(error, "winsock error"));
        CloseSockets();
        return HRESULT_FROM_WIN32(error);
    }

    DWORD receiveTimeout = 250;
    if (setsockopt(m_receiveSocket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<char*>(&receiveTimeout), sizeof(receiveTimeout)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI receive timeout configuration failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(error, "winsock error"));
        CloseSockets();
        return HRESULT_FROM_WIN32(error);
    }

    sockaddr_in receiveAddress{};
    receiveAddress.sin_family = AF_INET;
    receiveAddress.sin_port = htons(IP_MIDI_MULTICAST_PORT);
    receiveAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_receiveSocket, reinterpret_cast<sockaddr*>(&receiveAddress), sizeof(receiveAddress)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI receive socket bind failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(error, "winsock error"));
        CloseSockets();
        return HRESULT_FROM_WIN32(error);
    }

    if (InetPtonA(AF_INET, IP_MIDI_MULTICAST_ADDRESS, &m_multicastMembership.imr_multiaddr) != 1)
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI multicast address conversion failed", MIDI_TRACE_EVENT_MESSAGE_FIELD));
        CloseSockets();
        return HRESULT_FROM_WIN32(WSAEINVAL);
    }
    m_multicastMembership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(m_receiveSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
        reinterpret_cast<char*>(&m_multicastMembership), sizeof(m_multicastMembership)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI multicast membership failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(error, "winsock error"));
        CloseSockets();
        return HRESULT_FROM_WIN32(error);
    }

    m_sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sendSocket == INVALID_SOCKET)
    {
        const auto error = WSAGetLastError();
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI send socket creation failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(error, "winsock error"));
        CloseSockets();
        return HRESULT_FROM_WIN32(error);
    }

    u_char multicastTtl = 1;
    u_char multicastLoopback = 0;
    if (setsockopt(m_sendSocket, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<char*>(&multicastTtl), sizeof(multicastTtl)) == SOCKET_ERROR ||
        setsockopt(m_sendSocket, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<char*>(&multicastLoopback), sizeof(multicastLoopback)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI multicast send configuration failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(error, "winsock error"));
        CloseSockets();
        return HRESULT_FROM_WIN32(error);
    }

    RefreshLocalIpv4Addresses();
    return S_OK;
}

void IpMidiNetworkEngine::CloseSockets()
{
    if (m_receiveSocket != INVALID_SOCKET)
    {
        setsockopt(m_receiveSocket, IPPROTO_IP, IP_DROP_MEMBERSHIP,
            reinterpret_cast<char*>(&m_multicastMembership), sizeof(m_multicastMembership));
        closesocket(m_receiveSocket);
        m_receiveSocket = INVALID_SOCKET;
    }

    if (m_sendSocket != INVALID_SOCKET)
    {
        closesocket(m_sendSocket);
        m_sendSocket = INVALID_SOCKET;
    }

    if (m_winsockStarted)
    {
        WSACleanup();
        m_winsockStarted = false;
    }
}

void IpMidiNetworkEngine::RefreshLocalIpv4Addresses()
{
    m_localIpv4AddressCount = 0;

    ULONG bufferLength = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, nullptr, &bufferLength) != ERROR_BUFFER_OVERFLOW)
    {
        return;
    }

    std::vector<uint8_t> buffer(bufferLength);
    auto addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addresses, &bufferLength) != NO_ERROR)
    {
        return;
    }

    for (auto adapter = addresses; adapter != nullptr && m_localIpv4AddressCount < m_localIpv4Addresses.size(); adapter = adapter->Next)
    {
        for (auto unicast = adapter->FirstUnicastAddress;
            unicast != nullptr && m_localIpv4AddressCount < m_localIpv4Addresses.size();
            unicast = unicast->Next)
        {
            if (unicast->Address.lpSockaddr->sa_family == AF_INET)
            {
                const auto address = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                m_localIpv4Addresses[m_localIpv4AddressCount++] = address->sin_addr.s_addr;
            }
        }
    }
}

bool IpMidiNetworkEngine::IsLocalAddress(ULONG address) const
{
    for (uint8_t index = 0; index < m_localIpv4AddressCount; ++index)
    {
        if (m_localIpv4Addresses[index] == address)
        {
            return true;
        }
    }

    return false;
}

HRESULT IpMidiNetworkEngine::QueueOutgoingUmp(PVOID message, UINT size, LONGLONG timestamp)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, message);
    RETURN_HR_IF(E_INVALIDARG, size == 0 || size % sizeof(uint32_t) != 0);

    IpMidiPacket packet{};
    packet.Timestamp = timestamp;

    {
        std::scoped_lock converterLock(m_converterMutex);
        const auto words = static_cast<uint32_t const*>(message);
        const auto wordCount = size / sizeof(uint32_t);

        for (UINT offset = 0; offset < wordCount;)
        {
            const auto messageType = internal::GetUmpMessageTypeFromFirstWord(words[offset]);
            const auto messageWordCount = internal::GetUmpLengthInMidiWordsFromFirstWord(words[offset]);

            if ((messageType != 0x1 && messageType != 0x2 && messageType != 0x3) ||
                messageWordCount == 0 || offset + messageWordCount > wordCount)
            {
                ++m_unsupportedOutgoingCount;
                TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                    TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                    TraceLoggingWideString(L"Unsupported or incomplete outgoing UMP message dropped", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                    TraceLoggingUInt8(messageType, "UMP message type"));
                return S_OK;
            }

            for (uint8_t wordIndex = 0; wordIndex < messageWordCount; ++wordIndex)
            {
                m_umpToByteStream.UMPStreamParse(words[offset + wordIndex]);
                while (m_umpToByteStream.availableBS())
                {
                    if (packet.Length == packet.Bytes.size())
                    {
                        ++m_unsupportedOutgoingCount;
                        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                            TraceLoggingWideString(L"Outgoing UMP conversion exceeded legacy ipMIDI datagram size", MIDI_TRACE_EVENT_MESSAGE_FIELD));
                        return S_OK;
                    }

                    packet.Bytes[packet.Length++] = m_umpToByteStream.readBS();
                }
            }

            offset += messageWordCount;
        }
    }

    if (packet.Length > 0)
    {
        TryEnqueueOutgoing(packet);
    }

    return S_OK;
}

HRESULT IpMidiNetworkEngine::RegisterCallback(IMidiCallback* callback, uint64_t* registrationId)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, callback);
    RETURN_HR_IF_NULL(E_INVALIDARG, registrationId);

    std::scoped_lock dispatchLock(m_callbackDispatchMutex);
    std::scoped_lock callbackLock(m_callbackMutex);
    RETURN_HR_IF(E_UNEXPECTED, m_stopping.load());

    for (auto& registration : m_callbacks)
    {
        if (registration.Id == 0)
        {
            registration.Id = m_nextCallbackRegistrationId++;
            registration.Callback = callback;
            *registrationId = registration.Id;
            return S_OK;
        }
    }

    return E_OUTOFMEMORY;
}

void IpMidiNetworkEngine::UnregisterCallback(uint64_t registrationId)
{
    if (registrationId == 0)
    {
        return;
    }

    std::scoped_lock dispatchLock(m_callbackDispatchMutex);
    std::scoped_lock callbackLock(m_callbackMutex);
    for (auto& registration : m_callbacks)
    {
        if (registration.Id == registrationId)
        {
            registration.Callback.reset();
            registration.Id = 0;
            return;
        }
    }
}

bool IpMidiNetworkEngine::TryEnqueueOutgoing(IpMidiPacket const& packet)
{
    {
        std::scoped_lock queueLock(m_outgoingMutex);
        if (m_outgoingQueue.full())
        {
            ++m_outgoingDropCount;
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI outgoing queue overflow", MIDI_TRACE_EVENT_MESSAGE_FIELD));
            return false;
        }
        m_outgoingQueue.push_back(packet);
    }

    m_outgoingWakeup.notify_one();
    return true;
}

bool IpMidiNetworkEngine::TryEnqueueIncoming(IpMidiPacket const& packet)
{
    {
        std::scoped_lock queueLock(m_incomingMutex);
        if (m_incomingQueue.full())
        {
            ++m_incomingDropCount;
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI incoming queue overflow", MIDI_TRACE_EVENT_MESSAGE_FIELD));
            return false;
        }
        m_incomingQueue.push_back(packet);
    }

    m_incomingWakeup.notify_one();
    return true;
}

void IpMidiNetworkEngine::SenderWorker(std::stop_token stopToken)
{
    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI sender worker started", MIDI_TRACE_EVENT_MESSAGE_FIELD));

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(IP_MIDI_MULTICAST_PORT);
    if (InetPtonA(AF_INET, IP_MIDI_MULTICAST_ADDRESS, &destination.sin_addr) != 1)
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI multicast destination conversion failed", MIDI_TRACE_EVENT_MESSAGE_FIELD));
        return;
    }

    while (!stopToken.stop_requested() && !m_stopping)
    {
        IpMidiPacket packet{};
        {
            std::unique_lock queueLock(m_outgoingMutex);
            m_outgoingWakeup.wait(queueLock, [this, &stopToken] { return m_stopping || stopToken.stop_requested() || !m_outgoingQueue.empty(); });
            if (m_stopping || stopToken.stop_requested()) break;
            packet = m_outgoingQueue.front();
            m_outgoingQueue.pop_front();
        }

        const auto sent = sendto(m_sendSocket, reinterpret_cast<char const*>(packet.Bytes.data()), packet.Length, 0,
            reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
        if (sent == SOCKET_ERROR || sent != packet.Length)
        {
            const auto error = WSAGetLastError();
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI multicast send failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingInt32(error, "winsock error"));
        }
    }

    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI sender worker stopped", MIDI_TRACE_EVENT_MESSAGE_FIELD));
}

void IpMidiNetworkEngine::ReceiverWorker(std::stop_token stopToken)
{
    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI receiver worker started", MIDI_TRACE_EVENT_MESSAGE_FIELD));

    while (!stopToken.stop_requested() && !m_stopping)
    {
        IpMidiPacket packet{};
        sockaddr_in source{};
        int sourceLength = sizeof(source);
        const auto received = recvfrom(m_receiveSocket, reinterpret_cast<char*>(packet.Bytes.data()), static_cast<int>(packet.Bytes.size()), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength);

        if (received == SOCKET_ERROR)
        {
            const auto error = WSAGetLastError();
            if (m_stopping || stopToken.stop_requested() || error == WSAETIMEDOUT || error == WSAEINTR || error == WSAENOTSOCK)
            {
                continue;
            }

            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI multicast receive failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingInt32(error, "winsock error"));
            continue;
        }

        if (received == 0)
        {
            ++m_malformedIncomingCount;
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"Empty ipMIDI datagram dropped", MIDI_TRACE_EVENT_MESSAGE_FIELD));
            continue;
        }

        if (IsLocalAddress(source.sin_addr.s_addr))
        {
            continue;
        }

        packet.Length = static_cast<uint16_t>(received);
        packet.Timestamp = internal::GetCurrentMidiTimestamp();
        TryEnqueueIncoming(packet);
    }

    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI receiver worker stopped", MIDI_TRACE_EVENT_MESSAGE_FIELD));
}

void IpMidiNetworkEngine::DispatcherWorker(std::stop_token stopToken)
{
    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI dispatcher worker started", MIDI_TRACE_EVENT_MESSAGE_FIELD));

    while (!stopToken.stop_requested() && !m_stopping)
    {
        IpMidiPacket packet{};
        {
            std::unique_lock queueLock(m_incomingMutex);
            m_incomingWakeup.wait(queueLock, [this, &stopToken] { return m_stopping || stopToken.stop_requested() || !m_incomingQueue.empty(); });
            if (m_stopping || stopToken.stop_requested()) break;
            packet = m_incomingQueue.front();
            m_incomingQueue.pop_front();
        }

        DispatchIncomingPacket(packet);
    }

    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI dispatcher worker stopped", MIDI_TRACE_EVENT_MESSAGE_FIELD));
}

void IpMidiNetworkEngine::DispatchIncomingPacket(IpMidiPacket const& packet)
{
    std::array<uint32_t, IP_MIDI_MAX_DATAGRAM_SIZE> umpWords{};
    size_t umpWordCount = 0;

    for (uint16_t index = 0; index < packet.Length; ++index)
    {
        m_byteStreamToUmp.bytestreamParse(packet.Bytes[index]);
        while (m_byteStreamToUmp.availableUMP())
        {
            if (umpWordCount == umpWords.size())
            {
                ++m_malformedIncomingCount;
                TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                    TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                    TraceLoggingWideString(L"Incoming ipMIDI datagram translated beyond dispatch capacity", MIDI_TRACE_EVENT_MESSAGE_FIELD));
                return;
            }
            umpWords[umpWordCount++] = m_byteStreamToUmp.readUMP();
        }
    }

    if (umpWordCount == 0)
    {
        return;
    }

    std::scoped_lock dispatchLock(m_callbackDispatchMutex);
    if (m_stopping)
    {
        return;
    }

    for (auto const& registration : m_callbacks)
    {
        if (registration.Id != 0 && registration.Callback != nullptr)
        {
            LOG_IF_FAILED(registration.Callback->Callback(
                MessageOptionFlags_ContextContainsGroupIndex,
                umpWords.data(),
                static_cast<UINT>(umpWordCount * sizeof(uint32_t)),
                packet.Timestamp,
                0));
        }
    }
}
