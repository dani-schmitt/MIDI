// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================


#pragma once

// singleton
class TransportState
{

public:
    static TransportState& Current();

    // no copying
    TransportState(_In_ const TransportState&) = delete;
    TransportState& operator=(_In_ const TransportState&) = delete;


    wil::com_ptr<CMidi2IpMidiEndpointManager> GetEndpointManager()
    {
        return m_endpointManager;
    }

    wil::com_ptr<CMidi2IpMidiConfigurationManager> GetConfigurationManager()
    {
        return m_configurationManager;
    }

    std::shared_ptr<MidiIpMidiDeviceTable> GetEndpointTable()
    {
        return m_endpointTable;
    }

    std::shared_ptr<TransportWorkQueue> GetEndpointWorkQueue()
    {
        return m_workQueue;
    }

    HRESULT InitializeNetworkEngine(_In_ bool loopbackEnabled);
    std::shared_ptr<IpMidiNetworkEngine> GetNetworkEngine();
    void ShutdownNetworkEngine();

    HRESULT Shutdown()
    {
        ShutdownNetworkEngine();
        m_endpointManager.reset();
        m_configurationManager.reset();

        return S_OK;
    }


    HRESULT ConstructEndpointManager();
    HRESULT ConstructConfigurationManager();


private:
    TransportState();
    ~TransportState();


    wil::com_ptr<CMidi2IpMidiEndpointManager> m_endpointManager{ nullptr };
    wil::com_ptr<CMidi2IpMidiConfigurationManager> m_configurationManager{ nullptr };

    std::shared_ptr<MidiIpMidiDeviceTable> m_endpointTable = std::make_shared<MidiIpMidiDeviceTable>();

    std::shared_ptr<TransportWorkQueue> m_workQueue = std::make_shared<TransportWorkQueue>();
    std::mutex m_networkEngineMutex;
    std::shared_ptr<IpMidiNetworkEngine> m_networkEngine;

};
