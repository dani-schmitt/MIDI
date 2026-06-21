// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

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
    UNREFERENCED_PARAMETER(configurationJsonSection);
    UNREFERENCED_PARAMETER(response);

    return E_NOTIMPL;
}

HRESULT
CMidi2IpMidiConfigurationManager::Shutdown()
{
    m_MidiDeviceManager.reset();

    return S_OK;
}
