// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

namespace IpMidiRegistrySettings
{
    uint8_t ReadWantedPorts() noexcept;
    bool ReadLoopback() noexcept;
    HRESULT WriteActualPorts(_In_ uint8_t actualPorts) noexcept;
    HRESULT WriteLoopback(_In_ bool enabled) noexcept;
}
