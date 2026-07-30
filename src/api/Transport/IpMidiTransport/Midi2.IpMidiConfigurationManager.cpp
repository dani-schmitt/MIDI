// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

namespace
{
    constexpr wchar_t GetLoopbackCommand[] = L"getLoopback";
    constexpr wchar_t SetLoopbackCommand[] = L"setLoopback";
    constexpr wchar_t GetMuteMaskCommand[] = L"getMuteMask";
    constexpr wchar_t GetTrialStatusCommand[] = L"getTrialStatus";
    constexpr wchar_t GetNetworkStatusCommand[] = L"getNetworkStatus";
    constexpr wchar_t EnabledArgument[] = L"enabled";
    constexpr wchar_t MutedProperty[] = L"muted";
    constexpr wchar_t MuteMaskProperty[] = L"muteMask";
    constexpr wchar_t IsTrialProperty[] = L"isTrial";
    constexpr wchar_t StateProperty[] = L"state";
    constexpr wchar_t RemainingSecondsProperty[] = L"remainingSeconds";
    constexpr wchar_t RebootRequiredProperty[] = L"rebootRequired";
    constexpr wchar_t SafetyMuteMaskProperty[] = L"safetyMuteMask";
    constexpr wchar_t EffectiveMuteMaskProperty[] = L"effectiveMuteMask";
    constexpr wchar_t OverflowMaskProperty[] = L"overflowMask";
    constexpr wchar_t ReceiveFailureMaskProperty[] = L"receiveFailureMask";
    constexpr wchar_t SendFailureMaskProperty[] = L"sendFailureMask";
    constexpr wchar_t GenerationProperty[] = L"generation";
    constexpr wchar_t ActualPortsProperty[] = L"actualPorts";

    wchar_t const* TrialStateName(IpMidiTrialStateKind state) noexcept
    {
        switch (state)
        {
        case IpMidiTrialStateKind::Retail: return L"retail";
        case IpMidiTrialStateKind::NotStarted: return L"notStarted";
        case IpMidiTrialStateKind::Active: return L"active";
        case IpMidiTrialStateKind::Expired: return L"expired";
        case IpMidiTrialStateKind::Faulted: return L"faulted";
        default: return L"faulted";
        }
    }

    void SetLoopbackResponse(json::JsonObject& responseObject, bool enabled)
    {
        internal::SetConfigurationResponseObjectSuccess(responseObject);
        responseObject.SetNamedValue(EnabledArgument, json::JsonValue::CreateBooleanValue(enabled));
    }

    void SetMuteResponse(json::JsonObject& responseObject, bool muted, uint32_t muteMask)
    {
        internal::SetConfigurationResponseObjectSuccess(responseObject);
        responseObject.SetNamedValue(MutedProperty, json::JsonValue::CreateBooleanValue(muted));
        responseObject.SetNamedValue(MuteMaskProperty, json::JsonValue::CreateNumberValue(muteMask));
    }

    void SetMuteMaskResponse(json::JsonObject& responseObject, uint32_t muteMask)
    {
        internal::SetConfigurationResponseObjectSuccess(responseObject);
        responseObject.SetNamedValue(MuteMaskProperty, json::JsonValue::CreateNumberValue(muteMask));
    }

    void SetTrialStatusResponse(json::JsonObject& responseObject, IpMidiTrialStatus const& status)
    {
        internal::SetConfigurationResponseObjectSuccess(responseObject);
        responseObject.SetNamedValue(IsTrialProperty, json::JsonValue::CreateBooleanValue(status.IsTrial));
        responseObject.SetNamedValue(StateProperty, json::JsonValue::CreateStringValue(TrialStateName(status.State)));
        responseObject.SetNamedValue(RemainingSecondsProperty,
            json::JsonValue::CreateNumberValue(status.RemainingSeconds));
        responseObject.SetNamedValue(RebootRequiredProperty,
            json::JsonValue::CreateBooleanValue(status.RebootRequired));
    }

    void SetNetworkStatusResponse(
        json::JsonObject& responseObject,
        IpMidiNetworkStatus const& status,
        uint8_t actualPorts)
    {
        internal::SetConfigurationResponseObjectSuccess(responseObject);
        responseObject.SetNamedValue(ActualPortsProperty, json::JsonValue::CreateNumberValue(actualPorts));
        responseObject.SetNamedValue(SafetyMuteMaskProperty, json::JsonValue::CreateNumberValue(status.SafetyMuteMask));
        responseObject.SetNamedValue(EffectiveMuteMaskProperty, json::JsonValue::CreateNumberValue(status.EffectiveMuteMask));
        responseObject.SetNamedValue(OverflowMaskProperty, json::JsonValue::CreateNumberValue(status.OverflowMask));
        responseObject.SetNamedValue(ReceiveFailureMaskProperty, json::JsonValue::CreateNumberValue(status.ReceiveFailureMask));
        responseObject.SetNamedValue(SendFailureMaskProperty, json::JsonValue::CreateNumberValue(status.SendFailureMask));
        responseObject.SetNamedValue(GenerationProperty, json::JsonValue::CreateNumberValue(status.Generation));
    }
}

_Use_decl_annotations_
HRESULT
CMidi2IpMidiConfigurationManager::Initialize(
    GUID transportId,
    IMidiDeviceManager* midiDeviceManager,
    IMidiServiceConfigurationManager* midiServiceConfigurationManagerInterface)
{
    UNREFERENCED_PARAMETER(transportId);
    UNREFERENCED_PARAMETER(midiServiceConfigurationManagerInterface);

    RETURN_HR_IF_NULL(E_INVALIDARG, midiDeviceManager);
    RETURN_IF_FAILED(midiDeviceManager->QueryInterface(__uuidof(IMidiDeviceManager), (void**)&m_MidiDeviceManager));

    return S_OK;
}

_Use_decl_annotations_
HRESULT
CMidi2IpMidiConfigurationManager::UpdateConfiguration(
    LPCWSTR configurationJsonSection,
    LPWSTR* response)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, response);
    *response = nullptr;

    auto responseObject = internal::BuildConfigurationResponseObject(false);
    if (configurationJsonSection == nullptr || *configurationJsonSection == L'\0')
    {
        internal::SetConfigurationResponseObjectFail(responseObject, L"Configuration JSON is required.");
        RETURN_HR_IF(E_OUTOFMEMORY, !internal::JsonStringifyObjectToOutParam(responseObject, response));
        return S_OK;
    }

    try
    {
        json::JsonObject transportObject;
        if (!json::JsonObject::TryParse(configurationJsonSection, transportObject))
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
        internal::SetConfigurationResponseObjectFailWithErrorCode(
            responseObject, static_cast<uint32_t>(hr), L"Unable to process the ipMIDI configuration command.");
    }

    RETURN_HR_IF(E_OUTOFMEMORY, !internal::JsonStringifyObjectToOutParam(responseObject, response));
    return S_OK;
}

HRESULT CMidi2IpMidiConfigurationManager::ProcessCommand(
    json::JsonObject const& transportObject,
    json::JsonObject& responseObject) noexcept
{
    try
    {
        auto command = internal::MidiTransportCommandHelper::ParseCommand(transportObject);
        auto networkEngine = TransportState::Current().GetNetworkEngine();
        if (networkEngine == nullptr)
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"The ipMIDI network engine is not running.");
            return S_OK;
        }

        if (command.Command() == GetLoopbackCommand)
        {
            SetLoopbackResponse(responseObject, networkEngine->LoopbackEnabled());
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

        if (command.Command() == GetTrialStatusCommand)
        {
            SetTrialStatusResponse(responseObject, networkEngine->GetTrialStatus());
            return S_OK;
        }

        if (command.Command() == GetMuteMaskCommand)
        {
            SetMuteMaskResponse(responseObject, networkEngine->EffectiveMuteMask());
            return S_OK;
        }

        if (command.Command() == GetNetworkStatusCommand)
        {
            SetNetworkStatusResponse(
                responseObject,
                networkEngine->GetNetworkStatus(),
                networkEngine->PortCount());
            return S_OK;
        }

        if (command.Command() == MIDI_CONFIG_JSON_TRANSPORT_COMMAND_MUTE_ENDPOINT ||
            command.Command() == MIDI_CONFIG_JSON_TRANSPORT_COMMAND_UNMUTE_ENDPOINT)
        {
            const auto associationArgument = command.Arguments()->find(
                MIDI_CONFIG_JSON_TRANSPORT_COMMAND_COMMON_PARAMETER_ENDPOINT_ASSOCIATION_ID);
            if (associationArgument == command.Arguments()->end())
            {
                internal::SetConfigurationResponseObjectFail(responseObject, L"The associationId argument is required.");
                return S_OK;
            }

            const auto associationId = internal::StringToGuid(associationArgument->second);
            return ChangePortMutedState(
                associationId,
                command.Command() == MIDI_CONFIG_JSON_TRANSPORT_COMMAND_MUTE_ENDPOINT,
                responseObject);
        }

        if (command.Command() != SetLoopbackCommand)
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"Unknown ipMIDI transport command.");
            return S_OK;
        }

        const auto enabledArgument = command.Arguments()->find(EnabledArgument);
        if (enabledArgument == command.Arguments()->end() ||
            (enabledArgument->second != L"true" && enabledArgument->second != L"false"))
        {
            internal::SetConfigurationResponseObjectFail(
                responseObject, L"The enabled argument must be either true or false.");
            return S_OK;
        }

        const bool enabled = enabledArgument->second == L"true";
        const bool previousValue = networkEngine->LoopbackEnabled();
        const auto applyResult = networkEngine->SetLoopbackEnabled(enabled);
        if (FAILED(applyResult))
        {
            internal::SetConfigurationResponseObjectFailWithErrorCode(
                responseObject, static_cast<uint32_t>(applyResult), L"Unable to change multicast loopback.");
            return S_OK;
        }

        const auto writeResult = IpMidiRegistrySettings::WriteLoopback(enabled);
        if (FAILED(writeResult))
        {
            const auto rollbackResult = networkEngine->SetLoopbackEnabled(previousValue);
            if (FAILED(rollbackResult))
            {
                TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                    TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                    TraceLoggingWideString(L"Unable to roll back ipMIDI loopback after registry failure", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                    TraceLoggingHResult(rollbackResult, MIDI_TRACE_EVENT_HRESULT_FIELD));
            }
            internal::SetConfigurationResponseObjectFailWithErrorCode(
                responseObject, static_cast<uint32_t>(writeResult), L"Unable to save the multicast loopback setting.");
            return S_OK;
        }

        SetLoopbackResponse(responseObject, enabled);
        return S_OK;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        internal::SetConfigurationResponseObjectFailWithErrorCode(
            responseObject, static_cast<uint32_t>(hr), L"Unable to process the ipMIDI configuration command.");
        return S_OK;
    }
}

HRESULT CMidi2IpMidiConfigurationManager::ChangePortMutedState(
    winrt::guid const& associationId,
    bool muted,
    json::JsonObject& responseObject) noexcept
{
    try
    {
        std::scoped_lock configurationLock(m_configurationMutex);
        auto endpointTable = TransportState::Current().GetEndpointTable();
        auto endpointManager = TransportState::Current().GetEndpointManager();
        auto networkEngine = TransportState::Current().GetNetworkEngine();
        if (endpointTable == nullptr || endpointManager == nullptr || networkEngine == nullptr)
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"The ipMIDI transport is not running.");
            return S_OK;
        }

        auto device = endpointTable->GetDevice(associationId);
        if (device == nullptr || device->Definition == nullptr ||
            device->Definition->PortIndex >= networkEngine->PortCount())
        {
            internal::SetConfigurationResponseObjectFail(responseObject, L"The ipMIDI endpoint was not found.");
            return S_OK;
        }

        auto definition = device->Definition;
        const auto portIndex = definition->PortIndex;
        if (networkEngine->TrialMuteLocked())
        {
            internal::SetConfigurationResponseObjectFail(
                responseObject,
                L"Trial time has expired. Please reboot your Computer to test ipMIDI again.");
            return S_OK;
        }

        const bool previousMuted = networkEngine->IsPortConfiguredMuted(portIndex);
        const auto previousMask = networkEngine->ConfiguredMuteMask();
        const auto previousNetworkStatus = networkEngine->GetNetworkStatus();
        const auto portBit = 1u << portIndex;
        const bool safetyMuted = (previousNetworkStatus.SafetyMuteMask & portBit) != 0;
        const bool previousEffectiveMuted = (previousNetworkStatus.EffectiveMuteMask & portBit) != 0;
        if (previousMuted == muted && (muted || !safetyMuted))
        {
            SetMuteResponse(responseObject, muted, networkEngine->EffectiveMuteMask());
            return S_OK;
        }

        const auto networkResult = networkEngine->SetPortMuted(portIndex, muted);
        if (FAILED(networkResult))
        {
            internal::SetConfigurationResponseObjectFailWithErrorCode(
                responseObject, static_cast<uint32_t>(networkResult), L"Unable to change the ipMIDI network state.");
            return S_OK;
        }

        if (networkEngine->TrialMuteLocked())
        {
            networkEngine->RestorePortConfiguredMuteState(portIndex, previousMuted);
            internal::SetConfigurationResponseObjectFail(
                responseObject,
                L"Trial time has expired. Please reboot your Computer to test ipMIDI again.");
            return S_OK;
        }

        definition->ConfiguredMuted = muted;
        const auto propertyResult = endpointManager->UpdateEndpointMutedStateProperty(definition, muted);
        if (FAILED(propertyResult))
        {
            definition->ConfiguredMuted = previousMuted;
            const auto rollbackResult = networkEngine->SetPortMuted(portIndex, previousMuted);
            networkEngine->RestorePortSafetyMute(portIndex, previousNetworkStatus);
            if (FAILED(rollbackResult) && networkEngine->TrialMuteLocked())
            {
                networkEngine->RestorePortConfiguredMuteState(portIndex, previousMuted);
            }
            LOG_IF_FAILED(rollbackResult);
            LOG_IF_FAILED(endpointManager->UpdateEndpointMutedStateProperty(
                definition, networkEngine->TrialMuteLocked() ? true : previousEffectiveMuted));
            internal::SetConfigurationResponseObjectFailWithErrorCode(
                responseObject, static_cast<uint32_t>(propertyResult), L"Unable to update the MIDI endpoint mute state.");
            return S_OK;
        }

        if (networkEngine->TrialMuteLocked())
        {
            networkEngine->RestorePortConfiguredMuteState(portIndex, previousMuted);
            definition->ConfiguredMuted = previousMuted;
            LOG_IF_FAILED(endpointManager->UpdateEndpointMutedStateProperty(definition, true));
            internal::SetConfigurationResponseObjectFail(
                responseObject,
                L"Trial time has expired. Please reboot your Computer to test ipMIDI again.");
            return S_OK;
        }

        const auto newMask = networkEngine->ConfiguredMuteMask();
        const auto registryResult = IpMidiRegistrySettings::WriteMuteMask(newMask);
        if (FAILED(registryResult))
        {
            const auto rollbackResult = networkEngine->SetPortMuted(portIndex, previousMuted);
            networkEngine->RestorePortSafetyMute(portIndex, previousNetworkStatus);
            if (FAILED(rollbackResult) && networkEngine->TrialMuteLocked())
            {
                networkEngine->RestorePortConfiguredMuteState(portIndex, previousMuted);
            }
            definition->ConfiguredMuted = previousMuted;
            const auto propertyRollbackResult = endpointManager->UpdateEndpointMutedStateProperty(
                definition, networkEngine->TrialMuteLocked() ? true : previousEffectiveMuted);
            LOG_IF_FAILED(rollbackResult);
            LOG_IF_FAILED(propertyRollbackResult);
            internal::SetConfigurationResponseObjectFailWithErrorCode(
                responseObject, static_cast<uint32_t>(registryResult), L"Unable to save the ipMIDI mute state.");
            return S_OK;
        }

        if (networkEngine->TrialMuteLocked())
        {
            networkEngine->RestorePortConfiguredMuteState(portIndex, previousMuted);
            definition->ConfiguredMuted = previousMuted;
            LOG_IF_FAILED(IpMidiRegistrySettings::WriteMuteMask(previousMask));
            LOG_IF_FAILED(endpointManager->UpdateEndpointMutedStateProperty(definition, true));
            internal::SetConfigurationResponseObjectFail(
                responseObject,
                L"Trial time has expired. Please reboot your Computer to test ipMIDI again.");
            return S_OK;
        }

        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI mute configuration applied", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingUInt8(static_cast<uint8_t>(portIndex + 1), "port number"),
            TraceLoggingBool(muted, "muted"),
            TraceLoggingUInt32(newMask, "mute mask"));
        SetMuteResponse(responseObject, muted, networkEngine->EffectiveMuteMask());
        return S_OK;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        internal::SetConfigurationResponseObjectFailWithErrorCode(
            responseObject, static_cast<uint32_t>(hr), L"Unable to change the ipMIDI mute state.");
        return S_OK;
    }
}

HRESULT
CMidi2IpMidiConfigurationManager::Shutdown()
{
    m_MidiDeviceManager.reset();

    return S_OK;
}
