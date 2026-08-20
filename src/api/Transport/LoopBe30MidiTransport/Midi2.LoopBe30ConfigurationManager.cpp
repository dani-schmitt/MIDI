// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

namespace
{
    constexpr wchar_t GetStatusCommand[] = L"getStatus";
    constexpr wchar_t GetTrialStatusCommand[] = L"getTrialStatus";
    constexpr wchar_t SetFeedbackDetectionEnabledCommand[] = L"setFeedbackDetectionEnabled";
    constexpr wchar_t EnabledArgument[] = L"enabled";

    wchar_t const* TrialStateName(LoopBe30TrialStateKind state) noexcept
    {
        switch (state)
        {
        case LoopBe30TrialStateKind::Retail: return L"retail";
        case LoopBe30TrialStateKind::NotStarted: return L"notStarted";
        case LoopBe30TrialStateKind::Active: return L"active";
        case LoopBe30TrialStateKind::Expired: return L"expired";
        default: return L"faulted";
        }
    }

    wchar_t const* DetectorStateName(LoopBe30DetectorState state) noexcept
    {
        switch (state)
        {
        case LoopBe30DetectorState::Disabled: return L"disabled";
        case LoopBe30DetectorState::WaitingForTraffic: return L"waiting";
        case LoopBe30DetectorState::Observing: return L"observing";
        case LoopBe30DetectorState::BelowRateThreshold: return L"belowThreshold";
        case LoopBe30DetectorState::PatternNotRepeating: return L"notRepeating";
        case LoopBe30DetectorState::FeedbackDetected: return L"feedback";
        case LoopBe30DetectorState::QueueOverload: return L"queueOverload";
        case LoopBe30DetectorState::DeliveryFailure: return L"deliveryFailure";
        default: return L"waiting";
        }
    }

    void SetTrialStatus(json::JsonObject& response, LoopBe30TrialStatus const& status)
    {
        response.SetNamedValue(L"isTrial", json::JsonValue::CreateBooleanValue(status.IsTrial));
        response.SetNamedValue(L"trialState", json::JsonValue::CreateStringValue(TrialStateName(status.State)));
        response.SetNamedValue(L"remainingSeconds", json::JsonValue::CreateNumberValue(status.RemainingSeconds));
        response.SetNamedValue(L"rebootRequired", json::JsonValue::CreateBooleanValue(status.RebootRequired));
    }

    void SetStatusResponse(
        json::JsonObject& response,
        LoopBe30TransportStatus const& status,
        LoopBe30TrialStatus const& trial,
        uint8_t actualPorts)
    {
        internal::SetConfigurationResponseObjectSuccess(response);
        response.SetNamedValue(L"actualPorts", json::JsonValue::CreateNumberValue(actualPorts));
        response.SetNamedValue(L"manualMuteMask", json::JsonValue::CreateNumberValue(status.ManualMuteMask));
        response.SetNamedValue(L"safetyMuteMask", json::JsonValue::CreateNumberValue(status.SafetyMuteMask));
        response.SetNamedValue(L"effectiveMuteMask", json::JsonValue::CreateNumberValue(status.EffectiveMuteMask));
        response.SetNamedValue(L"feedbackMuteMask", json::JsonValue::CreateNumberValue(status.FeedbackMuteMask));
        response.SetNamedValue(L"queueOverloadMask", json::JsonValue::CreateNumberValue(status.QueueOverloadMask));
        response.SetNamedValue(L"deliveryFailureMask", json::JsonValue::CreateNumberValue(status.DeliveryFailureMask));
        response.SetNamedValue(L"feedbackDetectionEnabled",
            json::JsonValue::CreateBooleanValue(status.FeedbackDetectionEnabled));
        response.SetNamedValue(L"generation", json::JsonValue::CreateNumberValue(status.Generation));
        SetTrialStatus(response, trial);

        json::JsonArray diagnostics;
        for (uint8_t index = 0; index < actualPorts; ++index)
        {
            auto const& item = status.Diagnostics[index];
            json::JsonObject object;
            object.SetNamedValue(L"port", json::JsonValue::CreateNumberValue(index + 1));
            object.SetNamedValue(L"bytesPerSecond", json::JsonValue::CreateNumberValue(item.BytesPerSecond));
            object.SetNamedValue(L"repeatPeriod", json::JsonValue::CreateNumberValue(item.BestRepeatingPeriod));
            object.SetNamedValue(L"repeatPercent", json::JsonValue::CreateNumberValue(item.BestMatchPercent));
            object.SetNamedValue(L"comparisons", json::JsonValue::CreateNumberValue(item.ComparisonCount));
            object.SetNamedValue(L"detectorState",
                json::JsonValue::CreateStringValue(DetectorStateName(item.State)));
            auto const& delivery = status.Delivery[index];
            object.SetNamedValue(L"activeCallbacks", json::JsonValue::CreateNumberValue(delivery.ActiveCallbacks));
            object.SetNamedValue(L"acceptedMessages", json::JsonValue::CreateNumberValue(delivery.AcceptedMessages));
            object.SetNamedValue(L"dequeuedMessages", json::JsonValue::CreateNumberValue(delivery.DequeuedMessages));
            object.SetNamedValue(L"callbackAttempts", json::JsonValue::CreateNumberValue(delivery.CallbackAttempts));
            object.SetNamedValue(L"callbackSuccesses", json::JsonValue::CreateNumberValue(delivery.CallbackSuccesses));
            object.SetNamedValue(L"callbackFailures", json::JsonValue::CreateNumberValue(delivery.CallbackFailures));
            object.SetNamedValue(L"lastCallbackResult", json::JsonValue::CreateNumberValue(delivery.LastCallbackResult));
            object.SetNamedValue(L"lastCallbackOptions", json::JsonValue::CreateNumberValue(delivery.LastCallbackOptions));
            diagnostics.Append(object);
        }
        response.SetNamedValue(L"diagnostics", diagnostics);
    }
}

_Use_decl_annotations_
HRESULT CMidi2LoopBe30ConfigurationManager::Initialize(
    GUID transportId,
    IMidiDeviceManager* midiDeviceManager,
    IMidiServiceConfigurationManager* midiServiceConfigurationManager)
{
    UNREFERENCED_PARAMETER(transportId);
    UNREFERENCED_PARAMETER(midiServiceConfigurationManager);
    if (midiDeviceManager != nullptr)
        RETURN_IF_FAILED(midiDeviceManager->QueryInterface(__uuidof(IMidiDeviceManager),
            reinterpret_cast<void**>(&m_MidiDeviceManager)));
    return S_OK;
}

_Use_decl_annotations_
HRESULT CMidi2LoopBe30ConfigurationManager::UpdateConfiguration(
    LPCWSTR configurationJsonSection,
    LPWSTR* response)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, response);
    *response = nullptr;
    auto responseObject = internal::BuildConfigurationResponseObject(false);
    try
    {
        json::JsonObject transportObject;
        if (configurationJsonSection == nullptr ||
            !json::JsonObject::TryParse(configurationJsonSection, transportObject))
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"Configuration JSON is invalid.");
        }
        else if (!internal::MidiTransportCommandHelper::TransportObjectContainsCommand(transportObject))
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"A transport command is required.");
        }
        else
        {
            RETURN_IF_FAILED(ProcessCommand(transportObject, responseObject));
        }
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        internal::SetConfigurationResponseObjectFailWithErrorCode(responseObject,
            static_cast<uint32_t>(hr), L"Unable to process the LoopBe30 command.");
    }
    RETURN_HR_IF(E_OUTOFMEMORY, !internal::JsonStringifyObjectToOutParam(responseObject, response));
    return S_OK;
}

HRESULT CMidi2LoopBe30ConfigurationManager::ProcessCommand(
    json::JsonObject const& transportObject,
    json::JsonObject& responseObject) noexcept
{
    try
    {
        auto command = internal::MidiTransportCommandHelper::ParseCommand(transportObject);
        auto engine = TransportState::Current().GetEngine();
        if (engine == nullptr)
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"The LoopBe30 transport is not running.");
            return S_OK;
        }

        if (command.Command() == MIDI_CONFIG_JSON_TRANSPORT_COMMAND_QUERY_CAPABILITIES)
        {
            std::map<std::wstring, bool> capabilities{};
            capabilities.emplace(MIDI_CONFIG_JSON_TRANSPORT_COMMAND_CAPABILITY_CUSTOMIZE_ENDPOINT, false);
            capabilities.emplace(MIDI_CONFIG_JSON_TRANSPORT_COMMAND_CAPABILITY_CUSTOMIZE_PORTS, false);
            capabilities.emplace(MIDI_CONFIG_JSON_TRANSPORT_COMMAND_CAPABILITY_RESTART_ENDPOINT, false);
            capabilities.emplace(MIDI_CONFIG_JSON_TRANSPORT_COMMAND_CAPABILITY_DISCONNECT_ENDPOINT, false);
            capabilities.emplace(MIDI_CONFIG_JSON_TRANSPORT_COMMAND_CAPABILITY_RECONNECT_ENDPOINT, false);
            capabilities.emplace(MIDI_CONFIG_JSON_TRANSPORT_COMMAND_CAPABILITY_MUTE_ENDPOINT, true);
            internal::SetConfigurationResponseObjectSuccess(responseObject);
            internal::SetConfigurationCommandResponseQueryCapabilities(responseObject, capabilities);
            return S_OK;
        }

        if (command.Command() == GetStatusCommand || command.Command() == GetTrialStatusCommand)
        {
            SetStatusResponse(responseObject, engine->GetStatus(), engine->GetTrialStatus(), engine->PortCount());
            return S_OK;
        }

        if (command.Command() == MIDI_CONFIG_JSON_TRANSPORT_COMMAND_MUTE_ENDPOINT ||
            command.Command() == MIDI_CONFIG_JSON_TRANSPORT_COMMAND_UNMUTE_ENDPOINT)
        {
            auto association = command.Arguments()->find(
                MIDI_CONFIG_JSON_TRANSPORT_COMMAND_COMMON_PARAMETER_ENDPOINT_ASSOCIATION_ID);
            if (association == command.Arguments()->end())
            {
                internal::SetConfigurationResponseObjectFail(responseObject, L"The associationId argument is required.");
                return S_OK;
            }
            return ChangePortMutedState(
                internal::StringToGuid(association->second),
                command.Command() == MIDI_CONFIG_JSON_TRANSPORT_COMMAND_MUTE_ENDPOINT,
                responseObject);
        }

        if (command.Command() == SetFeedbackDetectionEnabledCommand)
        {
            auto enabledArgument = command.Arguments()->find(EnabledArgument);
            if (enabledArgument == command.Arguments()->end() ||
                (enabledArgument->second != L"true" && enabledArgument->second != L"false"))
            {
                internal::SetConfigurationResponseObjectFail(responseObject,
                    L"The enabled argument must be true or false.");
                return S_OK;
            }
            std::scoped_lock lock(m_configurationMutex);
            const bool enabled = enabledArgument->second == L"true";
            const bool previous = engine->FeedbackDetectionEnabled();
            RETURN_IF_FAILED(engine->SetFeedbackDetectionEnabled(enabled));
            const auto saveResult = LoopBe30RegistrySettings::WriteFeedbackDetectionEnabled(enabled);
            if (FAILED(saveResult))
            {
                LOG_IF_FAILED(engine->SetFeedbackDetectionEnabled(previous));
                internal::SetConfigurationResponseObjectFailWithErrorCode(responseObject,
                    static_cast<uint32_t>(saveResult), L"Unable to save the feedback detector setting.");
                return S_OK;
            }
            SetStatusResponse(responseObject, engine->GetStatus(), engine->GetTrialStatus(), engine->PortCount());
            return S_OK;
        }

        internal::SetConfigurationResponseObjectFail(responseObject, L"Unknown LoopBe30 transport command.");
        return S_OK;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        internal::SetConfigurationResponseObjectFailWithErrorCode(responseObject,
            static_cast<uint32_t>(hr), L"Unable to process the LoopBe30 command.");
        return S_OK;
    }
}

HRESULT CMidi2LoopBe30ConfigurationManager::ChangePortMutedState(
    winrt::guid const& associationId,
    bool muted,
    json::JsonObject& responseObject) noexcept
{
    try
    {
        std::scoped_lock configurationLock(m_configurationMutex);
        auto endpointTable = TransportState::Current().GetEndpointTable();
        auto endpointManager = TransportState::Current().GetEndpointManager();
        auto engine = TransportState::Current().GetEngine();
        if (endpointTable == nullptr || endpointManager == nullptr || engine == nullptr)
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"The LoopBe30 transport is not running.");
            return S_OK;
        }
        auto device = endpointTable->GetDevice(associationId);
        if (device == nullptr || device->Definition == nullptr ||
            device->Definition->PortIndex >= engine->PortCount())
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"The LoopBe30 endpoint was not found.");
            return S_OK;
        }
        if (engine->TrialMuteLocked() && !muted)
        {
            internal::SetConfigurationResponseObjectFail(responseObject,
                L"Trial time has expired. Please reboot your Computer to test LoopBe30 again.");
            return S_OK;
        }

        auto definition = device->Definition;
        const auto portIndex = definition->PortIndex;
        const bool previousManualMute = engine->IsPortConfiguredMuted(portIndex);
        const auto previousStatus = engine->GetStatus();
        const auto previousMask = engine->ConfiguredMuteMask();

        const auto applyResult = engine->SetPortMuted(portIndex, muted);
        if (FAILED(applyResult))
        {
            internal::SetConfigurationResponseObjectFailWithErrorCode(responseObject,
                static_cast<uint32_t>(applyResult), L"Unable to change the LoopBe30 mute state.");
            return S_OK;
        }
        definition->ConfiguredMuted = muted;
        const auto propertyResult = endpointManager->UpdateEndpointMutedStateProperty(
            definition, engine->IsPortEffectivelyMuted(portIndex));
        if (FAILED(propertyResult))
        {
            engine->RestorePortConfiguredMuteState(portIndex, previousManualMute);
            engine->RestorePortSafetyMute(portIndex, previousStatus);
            definition->ConfiguredMuted = previousManualMute;
            LOG_IF_FAILED(endpointManager->UpdateEndpointMutedStateProperty(
                definition, (previousStatus.EffectiveMuteMask & (1u << portIndex)) != 0));
            internal::SetConfigurationResponseObjectFailWithErrorCode(responseObject,
                static_cast<uint32_t>(propertyResult), L"Unable to update the endpoint mute state.");
            return S_OK;
        }

        const auto saveResult = LoopBe30RegistrySettings::WriteMuteMask(engine->ConfiguredMuteMask());
        if (FAILED(saveResult))
        {
            engine->RestorePortConfiguredMuteState(portIndex, previousManualMute);
            engine->RestorePortSafetyMute(portIndex, previousStatus);
            definition->ConfiguredMuted = previousManualMute;
            LOG_IF_FAILED(LoopBe30RegistrySettings::WriteMuteMask(previousMask));
            LOG_IF_FAILED(endpointManager->UpdateEndpointMutedStateProperty(
                definition, (previousStatus.EffectiveMuteMask & (1u << portIndex)) != 0));
            internal::SetConfigurationResponseObjectFailWithErrorCode(responseObject,
                static_cast<uint32_t>(saveResult), L"Unable to save the LoopBe30 mute state.");
            return S_OK;
        }

        SetStatusResponse(responseObject, engine->GetStatus(), engine->GetTrialStatus(), engine->PortCount());
        responseObject.SetNamedValue(L"muted", json::JsonValue::CreateBooleanValue(
            engine->IsPortEffectivelyMuted(portIndex)));
        return S_OK;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        internal::SetConfigurationResponseObjectFailWithErrorCode(responseObject,
            static_cast<uint32_t>(hr), L"Unable to change the LoopBe30 mute state.");
        return S_OK;
    }
}

HRESULT CMidi2LoopBe30ConfigurationManager::Shutdown()
{
    m_MidiDeviceManager.reset();
    return S_OK;
}

