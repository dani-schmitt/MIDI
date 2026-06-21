// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

class MidiIpMidiDevice
{
public:
    std::shared_ptr<MidiIpMidiDeviceDefinition> Definition;

    void Shutdown()
    {
        Definition.reset();
    }

    ~MidiIpMidiDevice()
    {
        Shutdown();
    }
};
