// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

namespace LoopBe30RegistrySettings
{
    uint8_t ReadWantedPorts() noexcept;
    uint32_t ReadMuteMask() noexcept;
    bool ReadFeedbackDetectionEnabled() noexcept;
    HRESULT WriteActualPorts(_In_ uint8_t actualPorts) noexcept;
    HRESULT WriteMuteMask(_In_ uint32_t muteMask) noexcept;
    HRESULT WriteFeedbackDetectionEnabled(_In_ bool enabled) noexcept;
}

