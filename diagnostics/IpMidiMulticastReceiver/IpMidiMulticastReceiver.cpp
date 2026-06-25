// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//
// Standalone diagnostic receiver for legacy ipMIDI multicast datagrams.
//
// This intentionally does not depend on Windows MIDI Services, COM, the
// ipMIDI transport DLL, or any MIDI headers. It uses the same basic Winsock
// receive pattern as the transport:
//
//   socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
//   setsockopt(SO_REUSEADDR)
//   bind(0.0.0.0:21928)
//   setsockopt(IP_ADD_MEMBERSHIP) with ip_mreq
//     imr_multiaddr = 225.0.0.37
//     imr_interface = INADDR_ANY by default
//   recvfrom()

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
    constexpr char DefaultMulticastAddress[] = "225.0.0.37";
    constexpr uint16_t DefaultUdpPort = 21928;
    constexpr char DefaultInterfaceAddress[] = "0.0.0.0";
    constexpr size_t MaxDatagramSize = 1280;

    std::atomic<bool> g_running{ true };

    BOOL WINAPI ConsoleControlHandler(DWORD controlType)
    {
        switch (controlType)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            g_running = false;
            return TRUE;
        default:
            return FALSE;
        }
    }

    std::string FormatIpv4Address(ULONG networkOrderAddress)
    {
        in_addr address{};
        address.s_addr = networkOrderAddress;

        char buffer[INET_ADDRSTRLEN]{};
        if (InetNtopA(AF_INET, &address, buffer, static_cast<DWORD>(_countof(buffer))) == nullptr)
        {
            return "<invalid>";
        }

        return buffer;
    }

    void PrintLastWinsockError(char const* operation)
    {
        std::cerr << operation << " failed. WSAGetLastError=" << WSAGetLastError() << '\n';
    }

    void PrintHex(uint8_t const* bytes, int byteCount)
    {
        std::ios oldState(nullptr);
        oldState.copyfmt(std::cout);

        for (int index = 0; index < byteCount; ++index)
        {
            if (index > 0)
            {
                std::cout << ' ';
            }

            std::cout
                << std::uppercase
                << std::hex
                << std::setw(2)
                << std::setfill('0')
                << static_cast<unsigned int>(bytes[index]);
        }

        std::cout.copyfmt(oldState);
    }

    bool ParsePort(char const* text, uint16_t& port)
    {
        try
        {
            const auto parsed = std::stoul(text);
            if (parsed == 0 || parsed > 65535)
            {
                return false;
            }

            port = static_cast<uint16_t>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void PrintUsage(char const* exeName)
    {
        std::cout
            << "Usage:\n"
            << "  " << exeName << " [multicast-address] [udp-port] [interface-address]\n\n"
            << "Defaults:\n"
            << "  multicast-address = " << DefaultMulticastAddress << "\n"
            << "  udp-port          = " << DefaultUdpPort << "\n"
            << "  interface-address = " << DefaultInterfaceAddress << " (INADDR_ANY)\n\n"
            << "Examples:\n"
            << "  " << exeName << "\n"
            << "  " << exeName << " 225.0.0.37 21928 0.0.0.0\n"
            << "  " << exeName << " 225.0.0.37 21928 192.168.2.104\n";
    }
}

int main(int argc, char* argv[])
{
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help" || std::string(argv[1]) == "/?"))
    {
        PrintUsage(argv[0]);
        return 0;
    }

    char const* multicastAddressText = argc > 1 ? argv[1] : DefaultMulticastAddress;
    uint16_t udpPort = DefaultUdpPort;
    char const* interfaceAddressText = argc > 3 ? argv[3] : DefaultInterfaceAddress;

    if (argc > 2 && !ParsePort(argv[2], udpPort))
    {
        std::cerr << "Invalid UDP port: " << argv[2] << "\n\n";
        PrintUsage(argv[0]);
        return 1;
    }

    if (!SetConsoleCtrlHandler(ConsoleControlHandler, TRUE))
    {
        PrintLastWinsockError("SetConsoleCtrlHandler");
        return 1;
    }

    WSADATA winsockData{};
    const auto startupResult = WSAStartup(MAKEWORD(2, 2), &winsockData);
    if (startupResult != 0)
    {
        std::cerr << "WSAStartup failed. result=" << startupResult << '\n';
        return 1;
    }

    SOCKET receiveSocket = INVALID_SOCKET;
    auto cleanup = [&]()
    {
        if (receiveSocket != INVALID_SOCKET)
        {
            closesocket(receiveSocket);
            receiveSocket = INVALID_SOCKET;
        }

        WSACleanup();
    };

    receiveSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (receiveSocket == INVALID_SOCKET)
    {
        PrintLastWinsockError("socket");
        cleanup();
        return 1;
    }

    std::cout << "Receive socket created: " << static_cast<uint64_t>(receiveSocket) << '\n';

    int reuseAddress = 1;
    if (setsockopt(receiveSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuseAddress), sizeof(reuseAddress)) == SOCKET_ERROR)
    {
        PrintLastWinsockError("setsockopt(SO_REUSEADDR)");
        cleanup();
        return 1;
    }

    std::cout << "SO_REUSEADDR succeeded: " << reuseAddress << '\n';

    DWORD receiveTimeout = 1000;
    if (setsockopt(receiveSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&receiveTimeout), sizeof(receiveTimeout)) == SOCKET_ERROR)
    {
        PrintLastWinsockError("setsockopt(SO_RCVTIMEO)");
        cleanup();
        return 1;
    }

    sockaddr_in bindAddress{};
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_port = htons(udpPort);
    bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(receiveSocket, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR)
    {
        PrintLastWinsockError("bind");
        cleanup();
        return 1;
    }

    std::cout << "bind succeeded: " << FormatIpv4Address(bindAddress.sin_addr.s_addr) << ':' << udpPort << '\n';

    ip_mreq multicastRequest{};
    if (InetPtonA(AF_INET, multicastAddressText, &multicastRequest.imr_multiaddr) != 1)
    {
        std::cerr << "Invalid multicast address: " << multicastAddressText << '\n';
        cleanup();
        return 1;
    }

    if (InetPtonA(AF_INET, interfaceAddressText, &multicastRequest.imr_interface) != 1)
    {
        std::cerr << "Invalid interface address: " << interfaceAddressText << '\n';
        cleanup();
        return 1;
    }

    std::cout
        << "Joining multicast group with ip_mreq:\n"
        << "  imr_multiaddr = " << FormatIpv4Address(multicastRequest.imr_multiaddr.s_addr) << '\n'
        << "  imr_interface = " << FormatIpv4Address(multicastRequest.imr_interface.s_addr)
        << (multicastRequest.imr_interface.s_addr == htonl(INADDR_ANY) ? " (INADDR_ANY)" : "")
        << '\n';

    if (setsockopt(receiveSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<char*>(&multicastRequest), sizeof(multicastRequest)) == SOCKET_ERROR)
    {
        PrintLastWinsockError("setsockopt(IP_ADD_MEMBERSHIP)");
        cleanup();
        return 1;
    }

    std::cout << "IP_ADD_MEMBERSHIP succeeded.\n";
    std::cout << "Waiting for UDP datagrams. Press Ctrl+C to stop.\n\n";

    uint64_t datagramCount = 0;
    uint8_t buffer[MaxDatagramSize]{};

    while (g_running)
    {
        sockaddr_in sourceAddress{};
        int sourceAddressLength = sizeof(sourceAddress);

        const auto received = recvfrom(
            receiveSocket,
            reinterpret_cast<char*>(buffer),
            static_cast<int>(sizeof(buffer)),
            0,
            reinterpret_cast<sockaddr*>(&sourceAddress),
            &sourceAddressLength);

        if (received == SOCKET_ERROR)
        {
            const auto error = WSAGetLastError();
            if (error == WSAETIMEDOUT)
            {
                continue;
            }

            std::cerr
                << "recvfrom failed. WSAGetLastError=" << error
                << " running=" << std::boolalpha << g_running.load()
                << '\n';
            continue;
        }

        ++datagramCount;

        const auto now = std::chrono::system_clock::now();
        const auto nowTime = std::chrono::system_clock::to_time_t(now);
        tm localTime{};
        localtime_s(&localTime, &nowTime);

        std::cout
            << '[' << std::put_time(&localTime, "%H:%M:%S") << "] "
            << '#' << datagramCount
            << " bytes=" << received
            << " from=" << FormatIpv4Address(sourceAddress.sin_addr.s_addr)
            << ':' << ntohs(sourceAddress.sin_port)
            << " payload=";

        PrintHex(buffer, received);
        std::cout << '\n';
    }

    setsockopt(receiveSocket, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<char*>(&multicastRequest), sizeof(multicastRequest));
    cleanup();

    std::cout << "\nStopped. Received datagrams: " << datagramCount << '\n';
    return 0;
}
