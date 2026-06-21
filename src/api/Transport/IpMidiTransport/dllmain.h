// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================


#pragma once

class CMidi2IpMidiTransportModule : public ATL::CAtlDllModuleT< CMidi2IpMidiTransportModule >
{
public :
    DECLARE_LIBID(LIBID_Midi2IpMidiTransportLib)

    // the guid here is the lib guid from the IDL file, not the interface guid
    DECLARE_REGISTRY_APPID_RESOURCEID(IDR_MIDI2IPMIDITRANSPORT, "{fa5869a3-b673-49ed-8991-e44641d23c6c}")
};

extern class CMidi2IpMidiTransportModule _AtlModule;
