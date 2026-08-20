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


    wil::com_ptr<CMidi2LoopBe30EndpointManager> GetEndpointManager()
    {
        return m_endpointManager;
    }

    wil::com_ptr<CMidi2LoopBe30ConfigurationManager> GetConfigurationManager()
    {
        return m_configurationManager;
    }

    std::shared_ptr<MidiLoopBe30DeviceTable> GetEndpointTable()
    {
        return m_endpointTable;
    }

    std::shared_ptr<TransportWorkQueue> GetEndpointWorkQueue()
    {
        return m_workQueue;
    }

    HRESULT InitializeEngine(_In_ bool loopbackEnabled);
    std::shared_ptr<LoopBe30Engine> GetEngine();
    void ShutdownEngine();

    HRESULT Shutdown()
    {
        ShutdownEngine();
        m_endpointManager.reset();
        m_configurationManager.reset();

        return S_OK;
    }


    HRESULT ConstructEndpointManager();
    HRESULT ConstructConfigurationManager();


private:
    TransportState();
    ~TransportState();


    wil::com_ptr<CMidi2LoopBe30EndpointManager> m_endpointManager{ nullptr };
    wil::com_ptr<CMidi2LoopBe30ConfigurationManager> m_configurationManager{ nullptr };

    std::shared_ptr<MidiLoopBe30DeviceTable> m_endpointTable = std::make_shared<MidiLoopBe30DeviceTable>();

    std::shared_ptr<TransportWorkQueue> m_workQueue = std::make_shared<TransportWorkQueue>();
    std::mutex m_engineMutex;
    std::shared_ptr<LoopBe30Engine> m_engine;

};


