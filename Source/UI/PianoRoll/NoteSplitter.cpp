#include "NoteSplitter.h"
#include "../../Utils/Constants.h"
#include "../../Utils/BasePitchCurve.h"
#include <algorithm>

Note* NoteSplitter::findNoteAt(float x, float y) {
    if (!project || !coordMapper)
        return nullptr;

    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();

    for (auto& note : project->getNotes()) {
        if (note.isRest())
            continue;

        float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
        float noteW = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
        float noteY = coordMapper->midiToY(note.getAdjustedMidiNote());
        float noteH = pixelsPerSemitone;

        if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH) {
            return &note;
        }
    }

    return nullptr;
}

bool NoteSplitter::splitNoteAtFrame(Note* note, int splitFrame) {
    if (!note || !project)
        return false;

    int startFrame = note->getStartFrame();
    int endFrame = note->getEndFrame();

    // Ensure split point is within note bounds (with margin)
    if (splitFrame <= startFrame + 5 || splitFrame >= endFrame - 5)
        return false;

    // Store original note data for undo
    Note originalNote = *note;

    // Ensure clip waveform exists before splitting
    if (!note->hasClipWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.waveform.getNumSamples() > 0) {
            int startSample = startFrame * HOP_SIZE;
            int endSample = endFrame * HOP_SIZE;
            startSample = std::max(0, std::min(startSample, audioData.waveform.getNumSamples()));
            endSample = std::max(startSample, std::min(endSample, audioData.waveform.getNumSamples()));
            std::vector<float> clip;
            clip.reserve(static_cast<size_t>(endSample - startSample));
            const float* src = audioData.waveform.getReadPointer(0);
            for (int i = startSample; i < endSample; ++i)
                clip.push_back(src[i]);
            note->setClipWaveform(std::move(clip));
        }
    }

    // Ensure source clip waveform exists before splitting
    if (!note->hasSrcClipWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.originalWaveform.getNumSamples() > 0) {
            int srcStart = note->getSrcStartFrame() * HOP_SIZE;
            int srcEnd = note->getSrcEndFrame() * HOP_SIZE;
            srcStart = std::max(0, std::min(srcStart, audioData.originalWaveform.getNumSamples()));
            srcEnd = std::max(srcStart, std::min(srcEnd, audioData.originalWaveform.getNumSamples()));
            std::vector<float> srcClip;
            srcClip.reserve(static_cast<size_t>(srcEnd - srcStart));
            const float* origSrc = audioData.originalWaveform.getReadPointer(0);
            for (int i = srcStart; i < srcEnd; ++i)
                srcClip.push_back(origSrc[i]);
            note->setSrcClipWaveform(std::move(srcClip));
        }
    }

    // Ensure clip mel exists before splitting
    if (!note->hasClipMel()) {
        auto& audioData = project->getAudioData();
        if (!audioData.melSpectrogram.empty()) {
            int melSize = static_cast<int>(audioData.melSpectrogram.size());
            int melStart = std::max(0, std::min(startFrame, melSize));
            int melEnd = std::max(melStart, std::min(endFrame, melSize));
            if (melEnd > melStart) {
                std::vector<std::vector<float>> melClip(
                    audioData.melSpectrogram.begin() + melStart,
                    audioData.melSpectrogram.begin() + melEnd);
                note->setClipMel(std::move(melClip));
            }
        }
    }

    // Create the second note (right part)
    Note secondNote;
    secondNote.setStartFrame(splitFrame);
    secondNote.setEndFrame(endFrame);
    secondNote.setSrcStartFrame(splitFrame);
    secondNote.setSrcEndFrame(endFrame);
    secondNote.setMidiNote(note->getMidiNote());
    secondNote.setLyric(note->getLyric());
    secondNote.setPitchOffset(0.0f);

    // Split clip waveform if available
    if (note->hasClipWaveform()) {
        const auto& clip = note->getClipWaveform();
        int splitOffset = (splitFrame - startFrame) * HOP_SIZE;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(clip.size())));
        std::vector<float> leftClip(clip.begin(), clip.begin() + splitOffset);
        std::vector<float> rightClip(clip.begin() + splitOffset, clip.end());
        note->setClipWaveform(std::move(leftClip));
        secondNote.setClipWaveform(std::move(rightClip));
    }

    // Split source clip waveform if available
    if (note->hasSrcClipWaveform()) {
        const auto& srcClip = note->getSrcClipWaveform();
        int splitOffset = (splitFrame - startFrame) * HOP_SIZE;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(srcClip.size())));
        std::vector<float> leftSrcClip(srcClip.begin(), srcClip.begin() + splitOffset);
        std::vector<float> rightSrcClip(srcClip.begin() + splitOffset, srcClip.end());
        note->setSrcClipWaveform(std::move(leftSrcClip));
        secondNote.setSrcClipWaveform(std::move(rightSrcClip));
    }

    // Split clip mel if available
    if (note->hasClipMel()) {
        const auto& mel = note->getClipMel();
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(mel.size())));
        std::vector<std::vector<float>> leftMel(mel.begin(), mel.begin() + splitOffset);
        std::vector<std::vector<float>> rightMel(mel.begin() + splitOffset, mel.end());
        note->setClipMel(std::move(leftMel));
        secondNote.setClipMel(std::move(rightMel));
    }

    // Split originalDeltaPitch if available (pristine curve for non-destructive stretch)
    if (note->hasOriginalDeltaPitch()) {
        const auto& origDelta = note->getOriginalDeltaPitch();
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(origDelta.size())));
        std::vector<float> leftDelta(origDelta.begin(), origDelta.begin() + splitOffset);
        std::vector<float> rightDelta(origDelta.begin() + splitOffset, origDelta.end());
        note->setOriginalDeltaPitch(std::move(leftDelta));
        secondNote.setOriginalDeltaPitch(std::move(rightDelta));
    } else {
        // Fallback: extract from global deltaPitch if originalDeltaPitch is missing
        auto& audioData = project->getAudioData();
        const int totalFrames = static_cast<int>(audioData.deltaPitch.size());
        if (totalFrames > 0) {
            int leftLen = splitFrame - startFrame;
            int rightLen = endFrame - splitFrame;
            if (leftLen > 0) {
                std::vector<float> leftDelta(static_cast<size_t>(leftLen));
                for (int i = 0; i < leftLen; ++i) {
                    int gIdx = startFrame + i;
                    if (gIdx >= 0 && gIdx < totalFrames)
                        leftDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(gIdx)];
                }
                note->setOriginalDeltaPitch(std::move(leftDelta));
            }
            if (rightLen > 0) {
                std::vector<float> rightDelta(static_cast<size_t>(rightLen));
                for (int i = 0; i < rightLen; ++i) {
                    int gIdx = splitFrame + i;
                    if (gIdx >= 0 && gIdx < totalFrames)
                        rightDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(gIdx)];
                }
                secondNote.setOriginalDeltaPitch(std::move(rightDelta));
            }
        }
    }

    // Modify the first note (left part)
    note->setEndFrame(splitFrame);
    note->setSrcEndFrame(splitFrame);

    // Save first note BEFORE addNote (addNote may invalidate note pointer due to vector reallocation)
    Note firstNote = *note;

    // Add the second note to project
    project->addNote(secondNote);

    // CRITICAL: After splitting, rebuild basePitch/deltaPitch from notes
    // Keep pitchOffset at 0 for both new notes, but adjust basePitch to match F0
    {
        auto& audioData = project->getAudioData();
        const int totalFrames = audioData.getNumFrames();
        
        // Calculate average F0 MIDI value for each note segment and set as midiNote
        // This makes basePitch track the actual F0 without needing pitchOffset
        
        // For first note (left part) - note is the original pointer, already modified in project
        {
            const int startFrame = note->getStartFrame();
            const int endFrame = note->getEndFrame();
            
            if (!audioData.f0.empty() && endFrame > startFrame) {
                // Calculate average F0 in MIDI cents for this segment
                float sumF0Midi = 0.0f;
                int validCount = 0;
                
                for (int i = startFrame; i < endFrame && i < static_cast<int>(audioData.f0.size()); ++i) {
                    if (audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                        sumF0Midi += freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                        validCount++;
                    }
                }
                
                if (validCount > 0) {
                    const float avgF0Midi = sumF0Midi / static_cast<float>(validCount);
                    
                    // Update the note's midiNote to match average F0
                    note->setMidiNote(avgF0Midi);
                    note->setPitchOffset(0.0f);  // Explicitly reset to 0
                }
            }
        }
        
        // For second note (right part) - find it in project by startFrame
        {
            const int startFrame = secondNote.getStartFrame();
            const int endFrame = secondNote.getEndFrame();
            
            if (!audioData.f0.empty() && endFrame > startFrame) {
                // Calculate average F0 in MIDI cents for this segment
                float sumF0Midi = 0.0f;
                int validCount = 0;
                
                for (int i = startFrame; i < endFrame && i < static_cast<int>(audioData.f0.size()); ++i) {
                    if (audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                        sumF0Midi += freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                        validCount++;
                    }
                }
                
                if (validCount > 0) {
                    const float avgF0Midi = sumF0Midi / static_cast<float>(validCount);
                    
                    // Find and update the second note in project
                    for (auto& n : project->getNotes()) {
                        if (n.getStartFrame() == startFrame) {
                            n.setMidiNote(avgF0Midi);
                            n.setPitchOffset(0.0f);  // Explicitly reset to 0
                            break;
                        }
                    }
                }
            }
        }
        
        // Now rebuild basePitch and deltaPitch from notes (same as DrawHandler)
        // Step 1: Build note segments for base pitch generation
        std::vector<BasePitchCurve::NoteSegment> segments;
        const auto& notes = project->getNotes();
        segments.reserve(notes.size());
        for (const auto& n : notes) {
            if (n.isRest()) continue;
            
            BasePitchCurve::NoteSegment seg;
            seg.startFrame = n.getStartFrame();
            seg.endFrame = n.getEndFrame();
            // Base pitch uses adjusted midiNote directly (pitchOffset is 0)
            seg.midiNote = n.getMidiNote() + n.getPitchOffset()
                         - (n.getTiltLeft() + n.getTiltRight()) / 2.0f;
            segments.push_back(seg);
        }
        
        // Sort segments by start frame for stable generation
        std::sort(segments.begin(), segments.end(),
                  [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
        
        // Step 2: Regenerate basePitch from notes
        if (!segments.empty()) {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }
        
        // Step 3: Recalculate deltaPitch = f0 - basePitch (keep f0 unchanged!)
        audioData.deltaPitch.resize(static_cast<size_t>(totalFrames));
        for (int i = 0; i < totalFrames; ++i) {
            const float baseMidi = audioData.basePitch[static_cast<size_t>(i)];
            const float f0Midi = freqToMidi(audioData.f0[static_cast<size_t>(i)]);
            audioData.deltaPitch[static_cast<size_t>(i)] = f0Midi - baseMidi;
        }
        
        // Step 4: Update cached baseF0
        audioData.baseF0.resize(static_cast<size_t>(totalFrames));
        for (int i = 0; i < totalFrames; ++i) {
            audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);
        }
        
        // Step 5: Sync global deltaPitch back to each note's deltaPitch vectors
        for (auto& n : project->getNotes()) {
            if (n.isRest()) continue;
            
            const int startFrame = n.getStartFrame();
            const int endFrame = n.getEndFrame();
            const int numFrames = endFrame - startFrame;
            
            if (numFrames <= 0) continue;
            
            std::vector<float> noteDelta(static_cast<size_t>(numFrames));
            for (int i = 0; i < numFrames; ++i) {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames) {
                    noteDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
                }
            }
            
            // Set both originalDeltaPitch and deltaPitch
            n.setOriginalDeltaPitch(noteDelta);
            n.setDeltaPitch(noteDelta);
        }
    }

    // Add to undo manager
    if (undoManager) {
        const int startFrame = std::min(originalNote.getStartFrame(), secondNote.getStartFrame());
        const int endFrame = std::max(originalNote.getEndFrame(), secondNote.getEndFrame());
        
        auto action = std::make_unique<NoteSplitAction>(
            project, originalNote, firstNote, secondNote,
            [this, startFrame, endFrame]() {
                // Set F0 dirty range for the entire note region
                project->setF0DirtyRange(startFrame, endFrame);
            });
        undoManager->addAction(std::move(action));
    }
    
    if (onNoteSplit)
        onNoteSplit();

    return true;
}

bool NoteSplitter::splitNoteAtX(Note* note, float x) {
    if (!note || !coordMapper)
        return false;

    // Convert X coordinate to frame
    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    double time = x / pixelsPerSecond;
    int frame = static_cast<int>(time * SAMPLE_RATE / HOP_SIZE);

    return splitNoteAtFrame(note, frame);
}