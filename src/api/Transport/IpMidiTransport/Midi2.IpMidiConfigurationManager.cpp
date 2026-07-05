// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

namespace
{
    constexpr wchar_t GetLoopbackCommand[] = L"getLoopback";
    constexpr wchar_t SetLoopbackCommand[] = L"setLoopback";
    constexpr wchar_t EnabledArgument[] = L"enabled";

    void SetLoopbackResponse(json::JsonObject& responseObject, bool enabled)
    {
        internal::SetConfigurationResponseObjectSuccess(responseObject);
        responseObject.SetNamedValue(EnabledArgument, json::JsonValue::CreateBooleanValue(enabled));
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

HRESULT
CMidi2IpMidiConfigurationManager::Shutdown()
{
    m_MidiDeviceManager.reset();

    return S_OK;
}
