// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

class CMidi2IpMidiConfigurationManager :
    public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        IMidiTransportConfigurationManager>
{
public:
    STDMETHOD(Initialize(_In_ GUID, _In_ IMidiDeviceManager*, _In_ IMidiServiceConfigurationManager*));
    STDMETHOD(UpdateConfiguration(_In_ LPCWSTR, _Out_ LPWSTR*));
    STDMETHOD(Shutdown)();

private:
    wil::com_ptr_nothrow<IMidiDeviceManager> m_MidiDeviceManager;
};
