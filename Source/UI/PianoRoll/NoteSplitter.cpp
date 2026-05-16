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
        
        // For first note (left part) - use saved snapshot to avoid dangling pointer
        {
            const int startFrame = firstNote.getStartFrame();
            const int endFrame = firstNote.getEndFrame();
            
            if (!audioData.f0.empty() && endFrame > startFrame) {
                // Calculate average F0 in MIDI cents for this segment
                float sumF0Midi = 0.0f;
                int validCount = 0;
                
                for (int i = startFrame; i < endFrame && i < static_cast<int>(audioData.f0.size()); ++i) {
                    // Only count voiced frames with valid F0 (skip non-voiced regions where F0 may be 0 or interpolated)
                    if (i < static_cast<int>(audioData.voicedMask.size()) && 
                        audioData.voicedMask[i] && audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                        sumF0Midi += freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                        validCount++;
                    }
                }
                
                if (validCount > 0) {
                    const float avgF0Midi = sumF0Midi / static_cast<float>(validCount);
                    
                    // Find and update the first note in project using startFrame and endFrame
                    for (size_t i = 0; i < project->getNotes().size(); ++i) {
                        auto& n = project->getNotes()[i];
                        if (n.getStartFrame() == startFrame && n.getEndFrame() == endFrame) {
                            n.setMidiNote(avgF0Midi);
                            n.setPitchOffset(0.0f);
                            break;
                        }
                    }
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
                    // Only count voiced frames with valid F0 (skip non-voiced regions where F0 may be 0 or interpolated)
                    if (i < static_cast<int>(audioData.voicedMask.size()) && 
                        audioData.voicedMask[i] && audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                        sumF0Midi += freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                        validCount++;
                    }
                }
                
                if (validCount > 0) {
                    const float avgF0Midi = sumF0Midi / static_cast<float>(validCount);
                    
                    // Find and update the second note in project using index-based access
                    for (size_t i = 0; i < project->getNotes().size(); ++i) {
                        auto& n = project->getNotes()[i];
                        if (n.getStartFrame() == startFrame && n.getEndFrame() == endFrame) {
                            n.setMidiNote(avgF0Midi);
                            n.setPitchOffset(0.0f);
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

Note* NoteSplitter::findNoteBoundaryAt(float x, float y, float& boundaryX) {
    if (!project || !coordMapper)
        return nullptr;

    juce::ignoreUnused(y);  // Y coordinate is no longer used for boundary detection
    
    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    
    // Get all notes and sort by start frame
    auto& notes = project->getNotes();
    std::vector<Note*> sortedNotes;
    for (auto& n : notes) {
        if (!n.isRest()) {
            sortedNotes.push_back(&n);
        }
    }
    std::sort(sortedNotes.begin(), sortedNotes.end(),
              [](const Note* a, const Note* b) {
                  return a->getStartFrame() < b->getStartFrame();
              });
    
    // Check all adjacent note pairs for boundaries near the mouse X position
    for (size_t i = 0; i < sortedNotes.size() - 1; ++i) {
        Note* leftNote = sortedNotes[i];
        Note* rightNote = sortedNotes[i + 1];
        
        // Check if notes are adjacent (within 1 frame tolerance)
        if (std::abs(leftNote->getEndFrame() - rightNote->getStartFrame()) <= 1) {
            float leftEndX = framesToSeconds(leftNote->getEndFrame()) * pixelsPerSecond;
            float rightStartX = framesToSeconds(rightNote->getStartFrame()) * pixelsPerSecond;
            
            // Check if mouse is within 8 pixels of the boundary
            float distToLeft = std::abs(x - leftEndX);
            float distToRight = std::abs(x - rightStartX);
            
            if (distToLeft < 8.0f || distToRight < 8.0f) {
                boundaryX = (leftEndX + rightStartX) / 2.0f;
                return leftNote;  // Return the left note for merging
            }
        }
    }
    
    return nullptr;
}

bool NoteSplitter::mergeNotes(Note* leftNote, Note* rightNote) {
    if (!leftNote || !rightNote || !project)
        return false;
    
    // Verify notes are adjacent
    if (leftNote->getEndFrame() != rightNote->getStartFrame())
        return false;
    
    // Store original notes for undo
    Note leftNoteSnapshot = *leftNote;
    Note rightNoteSnapshot = *rightNote;
    
    // Create merged note
    Note mergedNote;
    mergedNote.setStartFrame(leftNote->getStartFrame());
    mergedNote.setEndFrame(rightNote->getEndFrame());
    mergedNote.setSrcStartFrame(leftNote->getSrcStartFrame());
    mergedNote.setSrcEndFrame(rightNote->getSrcEndFrame());
    mergedNote.setMidiNote(leftNote->getMidiNote());  // Will be recalculated based on average F0
    mergedNote.setLyric(leftNote->getLyric());
    mergedNote.setPitchOffset(0.0f);
    
    // Merge clip waveforms if available
    if (leftNote->hasClipWaveform() && rightNote->hasClipWaveform()) {
        const auto& leftClip = leftNote->getClipWaveform();
        const auto& rightClip = rightNote->getClipWaveform();
        std::vector<float> mergedClip;
        mergedClip.reserve(leftClip.size() + rightClip.size());
        mergedClip.insert(mergedClip.end(), leftClip.begin(), leftClip.end());
        mergedClip.insert(mergedClip.end(), rightClip.begin(), rightClip.end());
        mergedNote.setClipWaveform(std::move(mergedClip));
    }
    
    // Merge source clip waveforms if available
    if (leftNote->hasSrcClipWaveform() && rightNote->hasSrcClipWaveform()) {
        const auto& leftSrcClip = leftNote->getSrcClipWaveform();
        const auto& rightSrcClip = rightNote->getSrcClipWaveform();
        std::vector<float> mergedSrcClip;
        mergedSrcClip.reserve(leftSrcClip.size() + rightSrcClip.size());
        mergedSrcClip.insert(mergedSrcClip.end(), leftSrcClip.begin(), leftSrcClip.end());
        mergedSrcClip.insert(mergedSrcClip.end(), rightSrcClip.begin(), rightSrcClip.end());
        mergedNote.setSrcClipWaveform(std::move(mergedSrcClip));
    }
    
    // Merge clip mel if available
    if (leftNote->hasClipMel() && rightNote->hasClipMel()) {
        const auto& leftMel = leftNote->getClipMel();
        const auto& rightMel = rightNote->getClipMel();
        std::vector<std::vector<float>> mergedMel;
        mergedMel.reserve(leftMel.size() + rightMel.size());
        mergedMel.insert(mergedMel.end(), leftMel.begin(), leftMel.end());
        mergedMel.insert(mergedMel.end(), rightMel.begin(), rightMel.end());
        mergedNote.setClipMel(std::move(mergedMel));
    }
    
    // Merge originalDeltaPitch if available
    if (leftNote->hasOriginalDeltaPitch() && rightNote->hasOriginalDeltaPitch()) {
        const auto& leftDelta = leftNote->getOriginalDeltaPitch();
        const auto& rightDelta = rightNote->getOriginalDeltaPitch();
        std::vector<float> mergedDelta;
        mergedDelta.reserve(leftDelta.size() + rightDelta.size());
        mergedDelta.insert(mergedDelta.end(), leftDelta.begin(), leftDelta.end());
        mergedDelta.insert(mergedDelta.end(), rightDelta.begin(), rightDelta.end());
        mergedNote.setOriginalDeltaPitch(std::move(mergedDelta));
    }
    
    // CRITICAL: Save start frames BEFORE removing notes to avoid issues with pointer invalidation
    // after vector reallocation during removeNoteByStartFrame
    const int leftStartFrame = leftNote->getStartFrame();
    const int rightStartFrame = rightNote->getStartFrame();
    
    // Remove both notes from project
    project->removeNoteByStartFrame(leftStartFrame);
    project->removeNoteByStartFrame(rightStartFrame);
    
    // Add merged note
    project->addNote(mergedNote);
    
    // CRITICAL: After merging, recalculate midiNote based on average F0
    {
        auto& audioData = project->getAudioData();
        
        // Find merged note using startFrame
        for (auto& n : project->getNotes()) {
            if (n.getStartFrame() == mergedNote.getStartFrame()) {
                const int startFrame = n.getStartFrame();
                const int endFrame = n.getEndFrame();
                
                if (!audioData.f0.empty() && endFrame > startFrame) {
                    float sumF0Midi = 0.0f;
                    int validCount = 0;
                    
                    for (int i = startFrame; i < endFrame && i < static_cast<int>(audioData.f0.size()); ++i) {
                        // Only count voiced frames with valid F0 (skip non-voiced regions where F0 may be 0 or interpolated)
                        if (i < static_cast<int>(audioData.voicedMask.size()) && 
                            audioData.voicedMask[i] && audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                            sumF0Midi += freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                            validCount++;
                        }
                    }
                    
                    if (validCount > 0) {
                        const float avgF0Midi = sumF0Midi / static_cast<float>(validCount);
                        n.setMidiNote(avgF0Midi);
                        n.setPitchOffset(0.0f);
                    }
                }
                break;
            }
        }
    }
    
    // CRITICAL: After merging, rebuild basePitch and deltaPitch
    {
        auto& audioData = project->getAudioData();
        const int totalFrames = audioData.getNumFrames();
        
        // Step 1: Build note segments for base pitch generation
        std::vector<BasePitchCurve::NoteSegment> segments;
        const auto& notes = project->getNotes();
        segments.reserve(notes.size());
        for (const auto& n : notes) {
            if (n.isRest()) continue;
            
            BasePitchCurve::NoteSegment seg;
            seg.startFrame = n.getStartFrame();
            seg.endFrame = n.getEndFrame();
            seg.midiNote = n.getMidiNote() + n.getPitchOffset()
                         - (n.getTiltLeft() + n.getTiltRight()) / 2.0f;
            segments.push_back(seg);
        }
        
        std::sort(segments.begin(), segments.end(),
                  [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
        
        // Step 2: Regenerate basePitch from notes
        if (!segments.empty()) {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }
        
        // Step 3: Recalculate deltaPitch from f0 and new basePitch
        audioData.deltaPitch.resize(static_cast<size_t>(totalFrames));
        for (int i = 0; i < totalFrames; ++i) {
            const float baseMidi = audioData.basePitch[static_cast<size_t>(i)];
            // CRITICAL: Skip non-voiced regions to avoid computing deltaPitch from F0=0
            if (i < static_cast<int>(audioData.voicedMask.size()) && 
                audioData.voicedMask[static_cast<size_t>(i)] && 
                audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                const float f0Midi = freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                audioData.deltaPitch[static_cast<size_t>(i)] = f0Midi - baseMidi;
            } else {
                // For non-voiced regions, keep deltaPitch at 0 to avoid extreme negative values
                audioData.deltaPitch[static_cast<size_t>(i)] = 0.0f;
            }
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
            
            n.setOriginalDeltaPitch(noteDelta);
            n.setDeltaPitch(noteDelta);
        }
        
        // CRITICAL: Recompose F0 from basePitch and deltaPitch (apply UV mask to hide non-voiced regions)
        PitchCurveProcessor::composeF0InPlace(*project, /*applyUvMask=*/true);
    }
    
    // Add to undo manager
    if (undoManager) {
        auto action = std::make_unique<NoteMergeAction>(
            project, leftNoteSnapshot, rightNoteSnapshot, mergedNote,
            [this]() {
                // Callback for UI refresh
            });
        undoManager->addAction(std::move(action));
    }
    
    if (onNoteSplit)
        onNoteSplit();
    
    return true;
}