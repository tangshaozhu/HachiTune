/**
 * Example usage of VolumeEnvelopeCompensator in the context of audio processing
 * This demonstrates how to use the compensator to maintain consistent volume
 * after model inference with pc_nsf_hifigan
 */

#include "VolumeEnvelopeCompensator.h"
#include <vector>

namespace VolumeCompensationExample {
    
    /**
     * Process audio with volume compensation
     * @param originalAudio The original audio waveform before processing
     * @param processedAudio The audio after model inference (may have volume changes)
     * @return Audio with compensated volume to match original envelope
     */
    std::vector<float> processWithVolumeCompensation(
        const std::vector<float>& originalAudio,
        const std::vector<float>& processedAudio)
    {
        // Apply volume envelope compensation to maintain consistent loudness
        // Uses window length of 1024 and smoothing window of 5 as specified
        return VolumeEnvelopeCompensator::compensateVolume(
            originalAudio,
            processedAudio,
            1024,  // window length for RMS calculation
            5      // smoothing window size
        );
    }
    
    /**
     * Alternative method if you want more control over the process
     */
    std::vector<float> processWithDetailedControl(
        const std::vector<float>& originalAudio,
        const std::vector<float>& processedAudio)
    {
        // Step 1: Calculate original audio envelope
        auto originalEnvelope = VolumeEnvelopeCompensator::calculateRMSEnvelope(originalAudio, 1024);
        
        // Step 2: Calculate processed audio envelope  
        auto processedEnvelope = VolumeEnvelopeCompensator::calculateRMSEnvelope(processedAudio, 1024);
        
        // Step 3: Interpolate envelopes to match processed audio length
        int targetLength = static_cast<int>(processedAudio.size());
        auto interpolatedOriginal = VolumeEnvelopeCompensator::interpolateEnvelope(originalEnvelope, targetLength);
        auto interpolatedProcessed = VolumeEnvelopeCompensator::interpolateEnvelope(processedEnvelope, targetLength);
        
        // Step 4: Smooth the envelopes
        auto smoothedOriginal = VolumeEnvelopeCompensator::smoothEnvelope(interpolatedOriginal, 5);
        auto smoothedProcessed = VolumeEnvelopeCompensator::smoothEnvelope(interpolatedProcessed, 5);
        
        // Step 5: Calculate gain curve (original / processed ratio)
        auto gainCurve = VolumeEnvelopeCompensator::computeGainCurve(smoothedOriginal, smoothedProcessed);
        
        // Step 6: Apply gain curve to processed audio
        std::vector<float> compensatedAudio(processedAudio.size());
        for (size_t i = 0; i < processedAudio.size(); ++i) {
            compensatedAudio[i] = processedAudio[i] * gainCurve[i];
        }
        
        return compensatedAudio;
    }
}