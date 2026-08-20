// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

#include <array>
#include <cstddef>
#include <span>

template <typename T, size_t Capacity>
class LoopBe30FixedQueue
{
public:
    static_assert(Capacity != 0);

    bool TryPushBatchOrClear(std::span<T const> batch) noexcept
    {
        if (batch.size() > Capacity - m_count)
        {
            Clear();
            return false;
        }
        for (auto const& value : batch)
        {
            m_values[(m_begin + m_count) % Capacity] = value;
            ++m_count;
        }
        return true;
    }

    bool TryPop(T& value) noexcept
    {
        if (m_count == 0) return false;
        value = m_values[m_begin];
        m_begin = (m_begin + 1) % Capacity;
        --m_count;
        return true;
    }

    void Clear() noexcept
    {
        m_begin = 0;
        m_count = 0;
    }

    bool Empty() const noexcept { return m_count == 0; }
    size_t Count() const noexcept { return m_count; }
    static constexpr size_t MaximumSize() noexcept { return Capacity; }

private:
    std::array<T, Capacity> m_values{};
    size_t m_begin{};
    size_t m_count{};
};
