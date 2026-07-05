// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

namespace IpMidiRegistrySettings
{
    uint8_t ReadWantedPorts() noexcept;
    HRESULT WriteActualPorts(_In_ uint8_t actualPorts) noexcept;
}
