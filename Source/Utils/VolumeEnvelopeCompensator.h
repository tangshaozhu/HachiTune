#pragma once

#include "../JuceHeader.h"
#include <vector>
#include <cmath>

/**
 * Volume envelope compensator to maintain consistent volume after audio processing.
 * Addresses volume changes caused by pc_nsf_hifigan model inference by:
 * 1. Calculating RMS envelope of original waveform
 * 2. Calculating RMS envelope of processed waveform
 * 3. Computing gain ratio and applying inverse compensation
 */
class VolumeEnvelopeCompensator
{
public:
    /**
     * Type of envelope to calculate
     */
    enum class EnvelopeType {
        RMS,
        PEAK
    };
    
    /**
     * Calculate RMS envelope of audio signal using a sliding window
     * @param audio Input audio samples
     * @param windowLength Length of RMS calculation window (default 1024)
     * @return RMS envelope values
     */
    static std::vector<float> calculateRMSEnvelope(const std::vector<float>& audio, int windowLength = 2048);
    
    /**
     * Calculate peak envelope of audio signal using a sliding window
     * @param audio Input audio samples
     * @param windowLength Length of peak calculation window
     * @return Peak envelope values
     */
    static std::vector<float> calculatePeakEnvelope(const std::vector<float>& audio, int windowLength);
    
    /**
     * Interpolate envelope to match target length
     * @param envelope Original envelope
     * @param targetLength Target length for interpolation
     * @return Interpolated envelope
     */
    static std::vector<float> interpolateEnvelope(const std::vector<float>& envelope, int targetLength);
    
    /**
     * Smooth envelope using moving average
     * @param envelope Input envelope
     * @param smoothWindowSize Size of smoothing window
     * @return Smoothed envelope
     */
    static std::vector<float> smoothEnvelope(const std::vector<float>& envelope, int smoothWindowSize = 5);
    
    /**
     * Compensate processed audio using original and processed envelopes
     * @param originalAudio Original audio before processing
     * @param processedAudio Audio after processing (to be compensated)
     * @param windowLength Window length for RMS calculation
     * @param smoothWindowSize Size of smoothing window
     * @param envelopeType Type of envelope to use for compensation
     * @return Compensated audio with volume envelope preserved
     */
    static std::vector<float> compensateVolume(
        const std::vector<float>& originalAudio,
        const std::vector<float>& processedAudio,
        int windowLength = 2048,
        int smoothWindowSize = 10,
        EnvelopeType envelopeType = EnvelopeType::PEAK
    );
    
    /**
     * Compute gain curve based on ratio of original to processed envelopes
     * @param originalEnvelope Original audio envelope
     * @param processedEnvelope Processed audio envelope
     * @param minGain Minimum gain value to prevent division by zero
     * @return Gain curve to apply to processed audio
     */
    static std::vector<float> computeGainCurve(
        const std::vector<float>& originalEnvelope,
        const std::vector<float>& processedEnvelope,
        float minGain = 1e-6f
    );
    
    /**
     * Apply soft limiting to prevent clipping while preserving dynamics
     * @param audio Audio to limit
     * @param threshold Threshold above which to apply soft limiting (default 0.9)
     * @param softKnee Soft knee width for smooth transition (default 0.05)
     * @return Limited audio
     */
    static std::vector<float> applySoftLimiting(
        const std::vector<float>& audio,
        float threshold = 0.9f,
        float softKnee = 0.05f
    );
    
private:
    /**
     * Safe division function to prevent division by very small numbers
     */
    static float safeDivide(float numerator, float denominator, float minDenominator = 1e-6f);
    
    /**
     * Linear interpolation between two points
     */
    static float lerp(float a, float b, float t);
    
    /**
     * Apply soft limiting to a single sample
     */
    static float applySoftLimitingToOneSample(float sample, float threshold, float softKnee);
};