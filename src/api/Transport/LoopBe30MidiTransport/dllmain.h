// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================


#pragma once

class CMidi2LoopBe30TransportModule : public ATL::CAtlDllModuleT< CMidi2LoopBe30TransportModule >
{
public :
    DECLARE_LIBID(LIBID_Midi2LoopBe30TransportLib)

    // the guid here is the lib guid from the IDL file, not the interface guid
    DECLARE_REGISTRY_APPID_RESOURCEID(IDR_MIDI2LOOPBE30TRANSPORT, "{10c903c1-ec01-41cf-bef9-7c7bba40a541}")
};

extern class CMidi2LoopBe30TransportModule _AtlModule;

