// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License
// ============================================================================
// This is part of the Windows MIDI Services App API and should be used
// in your Windows application via an official binary distribution.
// Further information: https://aka.ms/midi
// ============================================================================

#pragma once

// the IDs here aren't the full Ids, just the values we start with
// The full Id comes back from the swdevicecreate callback

#define TRANSPORT_LAYER_GUID __uuidof(Midi2IpMidiTransport);

#define TRANSPORT_MANUFACTURER L"Microsoft"
#define TRANSPORT_CODE L"IPMIDI"

#define IP_MIDI_ENDPOINT_NAME L"01. Ethernet MIDI"
#define IP_MIDI_ENDPOINT_UNIQUE_ID L"01_ETHERNET_MIDI"

inline constexpr GUID IP_MIDI_ENDPOINT_ASSOCIATION_ID =
{ 0x9f4f32b2, 0x1ee3, 0x48d5, { 0x8c, 0x98, 0x3b, 0x93, 0x4a, 0x17, 0xff, 0xd0 } };

#define MIDI_IP_MIDI_INSTANCE_ID_PREFIX L"MIDIU_IPMIDI_"


// TODO: Names should be moved to .rc for localization

#define TRANSPORT_PARENT_ID L"MIDIU_IPMIDI_TRANSPORT"
#define TRANSPORT_PARENT_DEVICE_NAME L"ipMIDI Devices"
#define IP_MIDI_PARENT_ROOT L"HTREE\\ROOT\\0"


#define TRANSPORT_ENUMERATOR L"MIDISRV"
