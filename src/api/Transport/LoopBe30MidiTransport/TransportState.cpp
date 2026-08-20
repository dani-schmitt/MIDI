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

HRESULT TransportState::InitializeEngine(bool loopbackEnabled)
{
    std::scoped_lock lock(m_engineMutex);

    if (m_engine == nullptr)
    {
        auto engine = std::make_shared<LoopBe30Engine>();
        RETURN_IF_FAILED(engine->Initialize(loopbackEnabled));
        m_engine = std::move(engine);
    }

    return S_OK;
}

std::shared_ptr<LoopBe30Engine> TransportState::GetEngine()
{
    std::scoped_lock lock(m_engineMutex);
    return m_engine;
}

void TransportState::ShutdownEngine()
{
    std::shared_ptr<LoopBe30Engine> engine;
    {
        std::scoped_lock lock(m_engineMutex);
        engine = std::move(m_engine);
    }

    if (engine != nullptr)
    {
        engine->Shutdown();
    }
}



HRESULT
TransportState::ConstructEndpointManager()
{
    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<CMidi2LoopBe30EndpointManager>(&m_endpointManager));

    return S_OK;
}


HRESULT
TransportState::ConstructConfigurationManager()
{
    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<CMidi2LoopBe30ConfigurationManager>(&m_configurationManager));

    return S_OK;
}


