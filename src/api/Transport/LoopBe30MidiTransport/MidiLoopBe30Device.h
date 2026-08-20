// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

class MidiLoopBe30Device
{
public:
    std::shared_ptr<MidiLoopBe30DeviceDefinition> Definition;

    void Shutdown()
    {
        Definition.reset();
    }

    ~MidiLoopBe30Device()
    {
        Shutdown();
    }
};

