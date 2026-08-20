// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

class CMidi2LoopBe30ConfigurationManager :
    public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        IMidiTransportConfigurationManager>
{
public:
    STDMETHOD(Initialize(_In_ GUID, _In_ IMidiDeviceManager*, _In_ IMidiServiceConfigurationManager*));
    STDMETHOD(UpdateConfiguration(_In_ LPCWSTR, _Out_ LPWSTR*));
    STDMETHOD(Shutdown)();

private:
    HRESULT ProcessCommand(
        _In_ json::JsonObject const& transportObject,
        _Inout_ json::JsonObject& responseObject) noexcept;

    HRESULT ChangePortMutedState(
        _In_ winrt::guid const& associationId,
        _In_ bool muted,
        _Inout_ json::JsonObject& responseObject) noexcept;

    wil::com_ptr_nothrow<IMidiDeviceManager> m_MidiDeviceManager;
    std::mutex m_configurationMutex;
};

