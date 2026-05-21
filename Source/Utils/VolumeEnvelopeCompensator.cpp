#include "VolumeEnvelopeCompensator.h"
#include "../JuceHeader.h"
#include <stdexcept>
#include <cmath>

std::vector<float> VolumeEnvelopeCompensator::calculateRMSEnvelope(const std::vector<float>& audio, int windowLength)
{
    if (audio.empty() || windowLength <= 0) {
        return {};
    }
    
    std::vector<float> envelope;
    int numSamples = static_cast<int>(audio.size());
    
    // Calculate number of envelope points
    int hopSize = windowLength / 2; // 50% overlap
    int numFrames = (numSamples + hopSize - 1) / hopSize; // Ceiling division
    
    envelope.reserve(numFrames);
    
    for (int i = 0; i < numFrames; ++i) {
        int startPos = i * hopSize;
        int endPos = std::min(startPos + windowLength, numSamples);
        
        // Calculate RMS for this window
        float sumSquares = 0.0f;
        int actualWindowLength = endPos - startPos;
        
        for (int j = startPos; j < endPos; ++j) {
            sumSquares += audio[j] * audio[j];
        }
        
        float rms = std::sqrt(sumSquares / actualWindowLength);
        envelope.push_back(rms);
    }
    
    return envelope;
}

std::vector<float> VolumeEnvelopeCompensator::calculatePeakEnvelope(const std::vector<float>& audio, int windowLength)
{
    if (audio.empty() || windowLength <= 0) {
        return {};
    }
    
    std::vector<float> envelope;
    int numSamples = static_cast<int>(audio.size());
    
    // Calculate number of envelope points
    int hopSize = windowLength / 2; // 50% overlap
    int numFrames = (numSamples + hopSize - 1) / hopSize; // Ceiling division
    
    envelope.reserve(numFrames);
    
    for (int i = 0; i < numFrames; ++i) {
        int startPos = i * hopSize;
        int endPos = std::min(startPos + windowLength, numSamples);
        
        // Find peak absolute value for this window
        float maxAbsValue = 0.0f;
        
        for (int j = startPos; j < endPos; ++j) {
            float absValue = std::abs(audio[j]);
            if (absValue > maxAbsValue) {
                maxAbsValue = absValue;
            }
        }
        
        envelope.push_back(maxAbsValue);
    }
    
    return envelope;
}

std::vector<float> VolumeEnvelopeCompensator::interpolateEnvelope(const std::vector<float>& envelope, int targetLength)
{
    if (envelope.empty() || targetLength <= 0) {
        return std::vector<float>(targetLength, 0.0f);
    }
    
    if (envelope.size() == 1) {
        return std::vector<float>(targetLength, envelope[0]);
    }
    
    std::vector<float> interpolated(targetLength);
    
    // Use cubic interpolation (Catmull-Rom spline) for smooth transitions
    for (int i = 0; i < targetLength; ++i) {
        float pos = i * (static_cast<float>(envelope.size() - 1) / (targetLength - 1));
        int idx = static_cast<int>(pos);
        float fract = pos - idx;
        
        // Clamp indices to valid range
        int idx0 = std::max(0, idx - 1);
        int idx1 = idx;
        int idx2 = std::min(idx + 1, static_cast<int>(envelope.size() - 1));
        int idx3 = std::min(idx + 2, static_cast<int>(envelope.size() - 1));
        
        // Get the four points for cubic interpolation
        float p0 = envelope[idx0];
        float p1 = envelope[idx1];
        float p2 = envelope[idx2];
        float p3 = envelope[idx3];
        
        // Cubic Hermite spline (Catmull-Rom spline)
        float c0 = p1;
        float c1 = 0.5f * (p2 - p0);
        float c2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        float c3 = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
        
        interpolated[i] = ((c3 * fract + c2) * fract + c1) * fract + c0;
    }
    
    return interpolated;
}

std::vector<float> VolumeEnvelopeCompensator::smoothEnvelope(const std::vector<float>& envelope, int smoothWindowSize)
{
    if (envelope.empty() || smoothWindowSize <= 0) {
        return envelope;
    }
    
    if (smoothWindowSize == 1) {
        return envelope;
    }
    
    std::vector<float> smoothed(envelope.size());
    float prevValue = envelope[0];
    float alpha = 0.3f;
    
    for (size_t i = 0; i < envelope.size(); ++i) {
        smoothed[i] = alpha * envelope[i] + (1.0f - alpha) * prevValue;
        prevValue = smoothed[i];
    }
    
    return smoothed;
}

std::vector<float> VolumeEnvelopeCompensator::compensateVolume(
    const std::vector<float>& originalAudio,
    const std::vector<float>& processedAudio,
    int windowLength,
    int smoothWindowSize,
    EnvelopeType envelopeType)
{
    juce::ScopedNoDenormals noDenormals;

    if (originalAudio.empty() || processedAudio.empty()) {
        return processedAudio;
    }
    
    // Calculate envelopes for both original and processed audio based on selected type
    auto originalEnvelope = (envelopeType == EnvelopeType::RMS) ? 
        calculateRMSEnvelope(originalAudio, windowLength) : 
        calculatePeakEnvelope(originalAudio, windowLength);
    auto processedEnvelope = (envelopeType == EnvelopeType::RMS) ? 
        calculateRMSEnvelope(processedAudio, windowLength) : 
        calculatePeakEnvelope(processedAudio, windowLength);
    
    // Interpolate envelopes to match the length of processed audio
    int targetLength = static_cast<int>(processedAudio.size());
    auto interpolatedOriginalEnv = interpolateEnvelope(originalEnvelope, targetLength);
    auto interpolatedProcessedEnv = interpolateEnvelope(processedEnvelope, targetLength);
    
    // Compute gain curve (ratio of original to processed, with safety factor)
    auto gainCurve = computeGainCurve(interpolatedOriginalEnv, interpolatedProcessedEnv);
    auto compensatedGainCurve = smoothEnvelope(gainCurve, smoothWindowSize);
    
    // Apply gain curve to processed audio
    std::vector<float> compensatedAudio(processedAudio.size());
    
    for (size_t i = 0; i < processedAudio.size(); ++i) {
        compensatedAudio[i] = processedAudio[i] * compensatedGainCurve[i];
    }
    
    // Apply soft limiting to prevent clipping while preserving dynamics
    return applySoftLimiting(compensatedAudio, 0.9f, 0.05f); 
}

std::vector<float> VolumeEnvelopeCompensator::computeGainCurve(
    const std::vector<float>& originalEnvelope,
    const std::vector<float>& processedEnvelope,
    float minGain)
{
    if (originalEnvelope.size() != processedEnvelope.size()) {
        throw std::invalid_argument("Envelope sizes must match");
    }
    
    std::vector<float> gainCurve(originalEnvelope.size());
    
    for (size_t i = 0; i < originalEnvelope.size(); ++i) {
        // Calculate gain as ratio of original to processed (inverse compensation)
        float gain = safeDivide(originalEnvelope[i], processedEnvelope[i], minGain);
        gainCurve[i] = gain;
    }
    
    return gainCurve;
}

std::vector<float> VolumeEnvelopeCompensator::applySoftLimiting(
    const std::vector<float>& audio,
    float threshold,
    float softKnee)
{
    std::vector<float> limitedAudio = audio;
    
    for (size_t i = 0; i < limitedAudio.size(); ++i) {
        limitedAudio[i] = applySoftLimitingToOneSample(limitedAudio[i], threshold, softKnee);
    }
    
    return limitedAudio;
}

float VolumeEnvelopeCompensator::applySoftLimitingToOneSample(float sample, float threshold, float softKnee)
{
    // Apply soft limiting using arctangent function for smooth transition
    // This prevents harsh clipping while maintaining dynamics
    
    if (sample > threshold + softKnee) {
        // Above upper knee - compress aggressively
        return threshold + softKnee * std::atan((sample - threshold) / softKnee);
    } else if (sample < -(threshold + softKnee)) {
        // Below lower knee - compress aggressively
        return -(threshold + softKnee * std::atan(-(sample + threshold) / softKnee));
    } else if (sample > threshold - softKnee && sample <= threshold + softKnee) {
        // Upper knee region - gradual compression
        float normalized = (sample - threshold) / softKnee;
        float kneeEffect = softKnee * std::atan(normalized) * 0.5f;
        return threshold + kneeEffect;
    } else if (sample < -threshold + softKnee && sample >= -(threshold + softKnee)) {
        // Lower knee region - gradual compression
        float normalized = (sample + threshold) / softKnee;
        float kneeEffect = softKnee * std::atan(-normalized) * 0.5f;
        return -threshold - kneeEffect;
    }
    // Within linear region - no change
    return sample;
}

float VolumeEnvelopeCompensator::safeDivide(float numerator, float denominator, float minDenominator)
{
    // Check for denormalized or invalid floating point values
    if (!juce::approximatelyEqual(denominator, denominator)) { // Check for NaN
        return 1.0f;  // Return neutral gain (no change) if denominator is NaN
    }
    
    // If denominator is very small, return a reasonable gain value
    // For volume compensation, when processed envelope is tiny, we want to avoid extreme amplification
    if (std::abs(denominator) < minDenominator) {
        // When processed signal is very quiet, return a gain based on the expected relationship
        // If original is also small, return 1.0 (no change)
        // If original is large but processed is small, return a reasonable upper bound
        if (std::abs(numerator) < minDenominator) {
            return 1.0f;  // Both very small, return neutral gain
        } else {
            // Original has energy but processed is nearly silent - return a reasonable max gain
            // This could happen if the model suppresses parts of the signal
            float sign = (numerator >= 0.0f) ? 1.0f : -1.0f;
            return sign * std::min(10.0f, std::abs(numerator) / minDenominator);  // Limit max gain to 10x
        }
    }
    
    float result = numerator / denominator;
    
    // Limit extreme gain values to prevent clipping
    if (result > 10.0f) return 10.0f;
    if (result < -10.0f) return -10.0f;
    
    return result;
}

float VolumeEnvelopeCompensator::lerp(float a, float b, float t)
{
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}