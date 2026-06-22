// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================

#include "pch.h"


TransportState::TransportState() = default;
TransportState::~TransportState() = default;

TransportState& TransportState::Current()
{
    // explanation: http://www.modernescpp.com/index.php/thread-safe-initialization-of-data/

    static TransportState current;

    return current;
}

HRESULT TransportState::InitializeNetworkEngine()
{
    std::scoped_lock lock(m_networkEngineMutex);

    if (m_networkEngine == nullptr)
    {
        auto networkEngine = std::make_shared<IpMidiNetworkEngine>();
        RETURN_IF_FAILED(networkEngine->Start());
        m_networkEngine = std::move(networkEngine);
    }

    return S_OK;
}

std::shared_ptr<IpMidiNetworkEngine> TransportState::GetNetworkEngine()
{
    std::scoped_lock lock(m_networkEngineMutex);
    return m_networkEngine;
}

void TransportState::ShutdownNetworkEngine()
{
    std::shared_ptr<IpMidiNetworkEngine> networkEngine;
    {
        std::scoped_lock lock(m_networkEngineMutex);
        networkEngine = std::move(m_networkEngine);
    }

    if (networkEngine != nullptr)
    {
        networkEngine->Shutdown();
    }
}



HRESULT
TransportState::ConstructEndpointManager()
{
    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<CMidi2IpMidiEndpointManager>(&m_endpointManager));

    return S_OK;
}


HRESULT
TransportState::ConstructConfigurationManager()
{
    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<CMidi2IpMidiConfigurationManager>(&m_configurationManager));

    return S_OK;
}
