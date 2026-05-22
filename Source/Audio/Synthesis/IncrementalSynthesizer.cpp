#include "IncrementalSynthesizer.h"
#include "../../Utils/Localization.h"
#include "../../Utils/SpikeSuppressor.h"
#include "../../Utils/VolumeEnvelopeCompensator.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

IncrementalSynthesizer::IncrementalSynthesizer() = default;

IncrementalSynthesizer::~IncrementalSynthesizer() { cancel(); }

void IncrementalSynthesizer::cancel() {
  if (cancelFlag)
    cancelFlag->store(true);
}

// ---------------------------------------------------------------------------
// computeSynthesisRange: find voiced segments overlapping dirty range,
// expand to include complete segments + padding.
// ---------------------------------------------------------------------------
std::pair<int, int>
IncrementalSynthesizer::computeSynthesisRange(int dirtyStart, int dirtyEnd) {
  if (!project)
    return {dirtyStart, dirtyEnd};

  auto &audioData = project->getAudioData();
  auto &voicedMask = audioData.voicedMask;
  
  const int totalFrames = static_cast<int>(voicedMask.size());
  if (totalFrames == 0)
    return {dirtyStart, dirtyEnd};

  dirtyStart = std::max(0, dirtyStart);
  dirtyEnd = std::min(totalFrames, dirtyEnd);

  auto isVoiced = [&](int idx) -> bool {
    return idx >= 0 && idx < totalFrames && static_cast<bool>(voicedMask[idx]);
  };

  // Expand backward: stop immediately at unvoiced region (no gap bridging)
  int start = dirtyStart;
  while (start > 0 && isVoiced(start - 1)) {
    --start;
  }
  // CRITICAL: Stop at first unvoiced frame - no gap tolerance for synthesis triggering

  // Expand forward: stop immediately at unvoiced region (no gap bridging)
  int end = dirtyEnd;
  while (end < totalFrames && isVoiced(end)) {
    ++end;
  }
  // CRITICAL: Stop at first unvoiced frame - no gap tolerance for synthesis triggering
  
  return {start, end};
}

// ---------------------------------------------------------------------------
// generateBlendMask: per-sample blend factor from voicedMask.
// 1.0 = synthesized, 0.0 = original, smooth ramps at transitions.
// ---------------------------------------------------------------------------
std::vector<float>
IncrementalSynthesizer::generateBlendMask(int startFrame, int endFrame,
                                          int hopSize) {
  auto &voicedMask = project->getAudioData().voicedMask;
  const int totalFrames = static_cast<int>(voicedMask.size());
  const int numFrames = endFrame - startFrame;
  const int numSamples = numFrames * hopSize;

  // Step 1: stability-first frame mask.
  // Default to synthesized audio in the whole region to avoid internal
  // orig/synth combing artifacts at note junctions.
  std::vector<float> frameMask(numFrames, 1.0f);

  // Keep original audio only for long unvoiced runs (e.g. clear breaths/silence),
  // not for short UV gaps between notes.
  constexpr int kKeepOriginalUnvoicedFrames = 24;
  if (numFrames > 0 && totalFrames > 0) {
    int i = 0;
    while (i < numFrames) {
      const int gf = startFrame + i;
      const bool voiced =
          gf >= 0 && gf < totalFrames && static_cast<bool>(voicedMask[gf]);
      if (voiced) {
        ++i;
        continue;
      }

      const int runStart = i;
      while (i < numFrames) {
        const int g = startFrame + i;
        const bool v =
            g >= 0 && g < totalFrames && static_cast<bool>(voicedMask[g]);
        if (v)
          break;
        ++i;
      }
      const int runEnd = i;
      const int runLen = runEnd - runStart;
      if (runLen >= kKeepOriginalUnvoicedFrames) {
        for (int k = runStart; k < runEnd; ++k)
          frameMask[k] = 0.0f;
      }
    }
  }

  // Step 2: expand to per-sample (sample-and-hold)
  std::vector<float> mask(numSamples, 0.0f);
  for (int i = 0; i < numFrames; ++i) {
    int ss = i * hopSize;
    int se = std::min(ss + hopSize, numSamples);
    for (int s = ss; s < se; ++s)
      mask[s] = frameMask[i];
  }

  // Step 3: smooth transitions with linear ramp at frame boundaries
  constexpr int kMinRampSamples = 512;
  const int kRampSamples = std::max(kMinRampSamples, hopSize * 2);
  for (int i = 0; i < numFrames - 1; ++i) {
    if (frameMask[i] == frameMask[i + 1])
      continue;
    // Transition at frame boundary
    int center = (i + 1) * hopSize;
    int rampStart = std::max(0, center - kRampSamples / 2);
    int rampEnd = std::min(numSamples, center + kRampSamples / 2);
    float fromVal = frameMask[i];
    float toVal = frameMask[i + 1];
    for (int s = rampStart; s < rampEnd; ++s) {
      float t = static_cast<float>(s - rampStart) /
                static_cast<float>(rampEnd - rampStart);
      mask[s] = fromVal + (toVal - fromVal) * t;
    }
  }

  return mask;
}

// ---------------------------------------------------------------------------
// synthesizeRegion: Voiced-Only Blend approach.
// ---------------------------------------------------------------------------
void IncrementalSynthesizer::synthesizeRegion(ProgressCallback onProgress,
                                              CompleteCallback onComplete) {
  if (!project || !vocoder) {
    if (onComplete)
      onComplete(false);
    return;
  }

  auto &audioData = project->getAudioData();
  if (audioData.melSpectrogram.empty() || audioData.f0.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!vocoder->isLoaded()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!project->hasDirtyNotes() && !project->hasF0DirtyRange()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  auto [dirtyStart, dirtyEnd] = project->getDirtyFrameRange();
  if (dirtyStart < 0 || dirtyEnd < 0) {
    if (onComplete)
      onComplete(false);
    return;
  }

  // Compute synthesis range (voiced segments + padding)
  auto [startFrame, endFrame] = computeSynthesisRange(dirtyStart, dirtyEnd);
  startFrame = std::max(0, startFrame);
  endFrame =
      std::min(static_cast<int>(audioData.melSpectrogram.size()), endFrame);

  if (startFrame >= endFrame) {
    if (onComplete)
      onComplete(false);
    return;
  }

  // Generate blend mask before async call (voicedMask is stable here)
  int hopSize = vocoder->getHopSize();
  std::vector<float> blendMask = generateBlendMask(startFrame, endFrame, hopSize);

  // Early exit: if blend mask is all-zero, nothing to synthesize
  bool hasVoiced = std::any_of(blendMask.begin(), blendMask.end(),
                               [](float v) { return v > 0.0f; });
  if (!hasVoiced) {
    project->clearAllDirty();
    if (onComplete)
      onComplete(true);
    return;
  }

  // Copy original waveform segment for blending
  const auto &origWaveform = audioData.originalWaveform.getNumSamples() > 0
                                 ? audioData.originalWaveform
                                 : audioData.waveform;
  int startSample = startFrame * hopSize;
  int numSynthSamples = (endFrame - startFrame) * hopSize;
  int totalOrigSamples = origWaveform.getNumSamples();

  std::vector<float> originalSegment(numSynthSamples, 0.0f);
  {
    const float *origPtr = origWaveform.getReadPointer(0);
    int copyLen = std::min(numSynthSamples,
                           std::max(0, totalOrigSamples - startSample));
    if (copyLen > 0 && startSample >= 0)
      std::copy(origPtr + startSample, origPtr + startSample + copyLen,
                originalSegment.begin());
  }

  // Extract mel + adjusted F0
  std::vector<std::vector<float>> melRange(
      audioData.melSpectrogram.begin() + startFrame,
      audioData.melSpectrogram.begin() + endFrame);
  std::vector<float> adjustedF0Range =
      project->getAdjustedF0ForRange(startFrame, endFrame);

  if (melRange.empty() || adjustedF0Range.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (onProgress)
    onProgress(TR("progress.synthesizing"));

  // Cancel previous job
  if (cancelFlag)
    cancelFlag->store(true);
  cancelFlag = std::make_shared<std::atomic<bool>>(false);
  uint64_t currentJobId = ++jobId;
  isBusy = true;

  int capturedStartFrame = startFrame;
  int capturedEndFrame = endFrame;
  auto capturedCancelFlag = cancelFlag;
  auto capturedProject = project;


  vocoder->inferAsync(
      std::move(melRange), std::move(adjustedF0Range),
      [this, capturedCancelFlag, capturedProject, capturedStartFrame,
       capturedEndFrame, hopSize, currentJobId, onComplete,
       blendMask = std::move(blendMask),
       originalSegment = std::move(originalSegment)](
          std::vector<float> synthesizedAudio) {
        if (currentJobId != jobId.load())
          return;
        if (capturedCancelFlag->load()) {
          isBusy = false;
          if (onComplete)
            onComplete(false);
          return;
        }
        if (synthesizedAudio.empty()) {
          isBusy = false;
          if (onComplete)
            onComplete(false);
          return;
        }

        std::thread([this, capturedCancelFlag, capturedProject,
                     capturedStartFrame, capturedEndFrame, hopSize,
                     currentJobId, onComplete, blendMask, originalSegment,
                     synthesizedAudio = std::move(synthesizedAudio)]() mutable {
          juce::ScopedNoDenormals noDenormals;

          if (currentJobId != jobId.load())
            return;
          if (capturedCancelFlag->load()) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          auto &audioData = capturedProject->getAudioData();
          int totalSamples = audioData.waveform.getNumSamples();
          int startSample = capturedStartFrame * hopSize;
          int expectedSamples =
              (capturedEndFrame - capturedStartFrame) * hopSize;

          if (expectedSamples <= 0) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          // Resize synthesized audio to match expected
          synthesizedAudio.resize(static_cast<size_t>(expectedSamples), 0.0f);

          int samplesToWrite =
              std::min(expectedSamples, totalSamples - startSample);
          if (samplesToWrite <= 0) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          // Apply volume compensation to maintain consistent loudness after model inference
        // This addresses the issue where pc_nsf_hifigan changes the volume characteristics
        if (!originalSegment.empty() && !synthesizedAudio.empty()) {
            // Only apply compensation if we have both original and synthesized audio
            std::vector<float> compensatedSynthesizedAudio = 
                VolumeEnvelopeCompensator::compensateVolume(
                    originalSegment, 
                    synthesizedAudio
                );
            
            // Replace synthesizedAudio with compensated version
            synthesizedAudio = std::move(compensatedSynthesizedAudio);
        }

          // Build blended target from model/original.
          std::vector<float> targetSegment(samplesToWrite, 0.0f);
          for (int i = 0; i < samplesToWrite; ++i) {
            const float b =
                (i < static_cast<int>(blendMask.size())) ? blendMask[i] : 0.0f;
            const float synth = synthesizedAudio[static_cast<size_t>(i)];
            const float orig = originalSegment[static_cast<size_t>(i)];
            targetSegment[static_cast<size_t>(i)] =
                b * synth + (1.0f - b) * orig;
          }

          // Apply per-note gain on top of the blended target.
          std::vector<float> sampleGain(static_cast<size_t>(samplesToWrite),
                                        1.0f);
          for (const auto &note : capturedProject->getNotes()) {
            if (note.isRest())
              continue;
            if (std::abs(note.getVolumeDb()) < 0.001f)
              continue;

            const int noteStart = note.getStartFrame();
            const int noteEnd = note.getEndFrame();
            const int overlapStart = std::max(capturedStartFrame, noteStart);
            const int overlapEnd = std::min(capturedEndFrame, noteEnd);
            if (overlapEnd <= overlapStart)
              continue;

            const int localStart = (overlapStart - capturedStartFrame) * hopSize;
            const int localEnd = (overlapEnd - capturedStartFrame) * hopSize;
            if (localStart >= samplesToWrite)
              continue;

            const float gain =
                juce::Decibels::decibelsToGain(note.getVolumeDb(), -60.0f);
            const int clampedStart = std::max(0, localStart);
            const int clampedEnd = std::min(samplesToWrite, localEnd);
            for (int i = clampedStart; i < clampedEnd; ++i) {
              sampleGain[static_cast<size_t>(i)] *= gain;
            }
          }
          for (int i = 0; i < samplesToWrite; ++i) {
            targetSegment[static_cast<size_t>(i)] *=
                sampleGain[static_cast<size_t>(i)];
          }

          // Distribute synthesized audio into per-note synthWaveforms.
          // Each note gets the slice of targetSegment corresponding to its
          // output frame range [startFrame, endFrame), PLUS margin samples on
          // each side so that composeGlobalWaveform() can crossfade with real
          // audio instead of held-value extrapolation at note boundaries.
          constexpr int kSynthMarginSamples = 256; // margin each side

          for (auto &note : capturedProject->getNotes()) {
            if (note.isRest())
              continue;

            const int noteStart = note.getStartFrame();
            const int noteEnd = note.getEndFrame();
            const int overlapStart = std::max(capturedStartFrame, noteStart);
            const int overlapEnd = std::min(capturedEndFrame, noteEnd);
            if (overlapEnd <= overlapStart)
              continue;

            // Only update notes that overlap the synthesis range and are dirty
            // (or have no synthWaveform yet)
            if (!note.isDirty() && !note.isSynthDirty() && note.hasSynthWaveform())
              continue;

            // Full note range in samples (the "body")
            const int noteStartSample = noteStart * hopSize;
            const int noteEndSample = noteEnd * hopSize;
            const int noteSamples = noteEndSample - noteStartSample;
            if (noteSamples <= 0)
              continue;

            // Compute margin: how far we can extend into targetSegment
            // beyond the note's body on each side.
            const int targetStartSample = capturedStartFrame * hopSize;
            const int targetEndSample = targetStartSample + samplesToWrite;

            // Left margin: extend before noteStartSample
            const int leftMarginAvail = noteStartSample - targetStartSample;
            const int leftMargin = std::max(0, std::min(kSynthMarginSamples, leftMarginAvail));

            // Right margin: extend after noteEndSample
            const int rightMarginAvail = targetEndSample - noteEndSample;
            const int rightMargin = std::max(0, std::min(kSynthMarginSamples, rightMarginAvail));

            // Total synth vector: [preroll | body | postroll]
            const int totalSynthLen = leftMargin + noteSamples + rightMargin;
            std::vector<float> noteSynth(static_cast<size_t>(totalSynthLen), 0.0f);

            // Copy from targetSegment: the extended region
            // [noteStartSample - leftMargin, noteEndSample + rightMargin) in global coords
            // maps to targetSegment[(noteStartSample - leftMargin - targetStartSample) ..]
            const int extGlobalStart = noteStartSample - leftMargin;
            const int extLocalSrc = extGlobalStart - targetStartSample;

            // The overlap between [extGlobalStart, noteEndSample+rightMargin) and
            // [capturedStartFrame*hopSize, capturedStartFrame*hopSize + samplesToWrite)
            // determines what we can actually copy from targetSegment.
            const int copyStart = std::max(0, extLocalSrc);
            const int copyEnd = std::min(samplesToWrite,
                extLocalSrc + totalSynthLen);
            const int dstOffset = copyStart - extLocalSrc;

            for (int i = copyStart; i < copyEnd; ++i) {
              const int dstIdx = dstOffset + (i - copyStart);
              if (dstIdx >= 0 && dstIdx < totalSynthLen) {
                noteSynth[static_cast<size_t>(dstIdx)] =
                    targetSegment[static_cast<size_t>(i)];
              }
            }

            // For parts of the note body outside the synthesis range, use srcClipWaveform
            if (note.hasSrcClipWaveform()) {
              const auto &srcClip = note.getSrcClipWaveform();
              const int srcFrames = note.getSrcEndFrame() - note.getSrcStartFrame();
              const int dstFrames = note.getEndFrame() - note.getStartFrame();
              const int srcSamples = static_cast<int>(srcClip.size());

              for (int i = 0; i < noteSamples; ++i) {
                const int globalSample = noteStartSample + i;
                const int globalFrame = (globalSample / hopSize);
                // Skip samples already covered by synthesis
                if (globalFrame >= overlapStart && globalFrame < overlapEnd)
                  continue;

                // Map destination sample to source sample (handle stretch)
                float srcPos;
                if (dstFrames > 0 && srcFrames > 0) {
                  srcPos = static_cast<float>(i) * static_cast<float>(srcSamples) /
                           static_cast<float>(noteSamples);
                } else {
                  srcPos = static_cast<float>(i);
                }
                int srcIdx = static_cast<int>(srcPos);
                if (srcIdx >= 0 && srcIdx < srcSamples) {
                  // Body samples start at offset leftMargin in noteSynth
                  noteSynth[static_cast<size_t>(leftMargin + i)] =
                      srcClip[static_cast<size_t>(srcIdx)];
                }
              }
            }


            note.setSynthWaveform(std::move(noteSynth), leftMargin);
            
            // VALIDATION CHECK: If the generated synthWaveform contains significant
            // zero regions, discard it to avoid phase discontinuities in composeGlobalWaveform.
            // It's better to use original audio than corrupted synth audio.
            if (noteSamples > 0 && leftMargin >= 0 && (leftMargin + noteSamples) <= static_cast<int>(noteSynth.size())) {
              int zeroCount = 0;
              const int checkStart = leftMargin;  // Only check the body, not margins
              const int checkEnd = leftMargin + noteSamples;
              
              for (int i = checkStart; i < checkEnd; ++i) {
                if (std::abs(noteSynth[static_cast<size_t>(i)]) < 1e-6f) {
                  zeroCount++;
                }
              }
              
              // If more than 5% of the body is zero, discard the synthWaveform
              const float zeroRatio = static_cast<float>(zeroCount) / static_cast<float>(noteSamples);
              if (zeroRatio > 0.05f) {
                note.clearSynthWaveform();  // This will make composeGlobalWaveform skip it
              }
            }
          }

          // Compose the global waveform from per-note synthWaveforms
          capturedProject->composeGlobalWaveform();

          // Suppress zero-crossing spikes in the dirty region only
          {
            auto &waveform = audioData.waveform;
            const int chCount = waveform.getNumChannels();
            const int startSmp = std::max(0, startSample - 1);
            const int endSmp = std::min(waveform.getNumSamples(),
                                        startSample + samplesToWrite + 1);
            const size_t len = static_cast<size_t>(endSmp - startSmp);
            if (len >= 3)
            {
              for (int ch = 0; ch < chCount; ++ch)
              {
                float *data = waveform.getWritePointer(ch) + startSmp;
                SpikeSuppressor::suppress(data, len);
              }
            }
          }

          isBusy = false;
          juce::MessageManager::callAsync(
              [capturedProject, onComplete]() {
                capturedProject->clearAllDirty();
                if (onComplete) onComplete(true);
              });
        }).detach();
      });
}