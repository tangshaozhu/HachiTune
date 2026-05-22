#pragma once

#include "../JuceHeader.h"
#include <cmath>
#include <vector>

namespace SpikeSuppressor
{
constexpr float kSpikeRatio = 4.0f;
constexpr float kSpikeFloor = 1e-4f;

inline void suppress(float *audio, size_t n)
{
    if (n < 3)
        return;

    std::vector<bool> spike(n, false);
    for (size_t i = 1; i + 1 < n; ++i)
    {
        const float curr = audio[i];
        const float absCurr = std::abs(curr);
        if (absCurr < kSpikeFloor)
            continue;

        const float prev = audio[i - 1];
        const float next = audio[i + 1];

        const bool signChangePrev = (curr * prev) < 0.0f;
        const bool signChangeNext = (curr * next) < 0.0f;
        if (!signChangePrev && !signChangeNext)
            continue;

        const float neighborMax = std::max(std::abs(prev), std::abs(next));
        const float ratio = (neighborMax > 1e-10f)
                                ? absCurr / neighborMax
                                : 0.0f;
        if (ratio <= kSpikeRatio)
            continue;

        spike[i] = true;
        DBG("spike marked @[" + juce::String(static_cast<int>(i)) +
            "] val=" + juce::String(curr, 6) +
            " prev=" + juce::String(prev, 6) +
            " next=" + juce::String(next, 6) +
            " ratio=" + juce::String(ratio, 1));
    }

    for (size_t i = 0; i < n;)
    {
        if (!spike[i])
        {
            ++i;
            continue;
        }
        size_t runStart = i;
        size_t runEnd = i;
        while (runEnd + 1 < n && spike[runEnd + 1])
            ++runEnd;

        const size_t left = (runStart > 0) ? runStart - 1 : runStart;
        const size_t right = (runEnd + 1 < n) ? runEnd + 1 : runEnd;
        const float leftVal = audio[left];
        const float rightVal = audio[right];
        const float runLen = static_cast<float>(right - left);

        DBG("spike fix @[" + juce::String(static_cast<int>(runStart)) +
            (runEnd > runStart ? ".." + juce::String(static_cast<int>(runEnd)) : juce::String()) +
            "] val=" + juce::String(audio[runStart], 6) +
            (runEnd > runStart ? "," + juce::String(audio[runEnd], 6) : juce::String()) +
            " -> interp=[" + juce::String(leftVal, 6) + "," + juce::String(rightVal, 6) + "]");

        for (size_t j = runStart; j <= runEnd; ++j)
        {
            const float t = (runLen > 0.0f)
                                ? static_cast<float>(j - left) / runLen
                                : 0.5f;
            audio[j] = leftVal + t * (rightVal - leftVal);
        }

        i = runEnd + 1;
    }
}
} // namespace SpikeSuppressor
