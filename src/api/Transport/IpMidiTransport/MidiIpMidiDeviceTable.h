// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================


#pragma once


class MidiIpMidiDeviceTable
{
private:
    // unline GUID, winrt::guid has built-in comparison so it can be used as a key in std::map
    std::map<winrt::guid, std::shared_ptr<MidiIpMidiDevice>> m_devices;


public:

    std::shared_ptr<MidiIpMidiDevice> GetDevice(_In_ winrt::guid const& associationId);
    std::shared_ptr<MidiIpMidiDevice> GetDeviceById(_In_ std::wstring const& endpointDeviceId);


    void SetDevice(_In_ winrt::guid const& associationId, _In_ std::shared_ptr<MidiIpMidiDevice> device);
    void RemoveDevice(_In_ winrt::guid const& associationId);

    bool IsUniqueIdentifierInUse(_In_ std::wstring const& uniqueIdentifier);

};
