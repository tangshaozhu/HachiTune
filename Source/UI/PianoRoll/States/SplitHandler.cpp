#include "SplitHandler.h"
#include "../../PianoRollComponent.h"
#include "../NoteSplitter.h"
#include "../../../Utils/Constants.h"
#include "../../../Undo/NoteActions.h"
#include "../../../Utils/PitchCurveProcessor.h"

SplitHandler::SplitHandler(PianoRollComponent &owner)
    : InteractionHandler(owner) {}

bool SplitHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                             float worldY) {
  juce::ignoreUnused(e, worldY);

  // Debug: Track mouseDown entry
  static int mouseDownCount = 0;
  if (mouseDownCount < 5) {
    DBG("MOUSEDOWN #" << mouseDownCount++ 
        << ": worldX=" << worldX
        << ", isNearBoundary_=" << (isNearBoundary_ ? "true" : "false"));
  }

  // Check if we're near a note boundary for merging (don't rely on isNearBoundary_ from mouseMove)
  float boundaryX = -1.0f;
  Note* boundaryNote = owner_.noteSplitter->findNoteBoundaryAt(worldX, worldY, boundaryX);
  
  if (boundaryNote && !isDraggingBoundary) {
    DBG("MOUSEDOWN: Found boundary at X=" << boundaryX << ", leftNote=" << boundaryNote->getStartFrame() << "-" << boundaryNote->getEndFrame());
    
    Note* leftNote = boundaryNote;
    
    // Find the right note
    auto& notes = owner_.project->getNotes();
    Note* rightNote = nullptr;
    for (auto& n : notes) {
      if (!n.isRest() && n.getStartFrame() == leftNote->getEndFrame()) {
        rightNote = &n;
        break;
      }
    }
    
    if (rightNote) {
      // Calculate boundary X in world coordinates
      float pixelsPerSecond = owner_.getPixelsPerSecond();
      float leftEndX = framesToSeconds(leftNote->getEndFrame()) * pixelsPerSecond;
      float rightStartX = framesToSeconds(rightNote->getStartFrame()) * pixelsPerSecond;
      float calcBoundaryX = (leftEndX + rightStartX) / 2.0f;
      
      DBG("MOUSEDOWN: worldX=" << worldX << ", calcBoundaryX=" << calcBoundaryX 
          << ", diff=" << std::abs(worldX - calcBoundaryX));
      
      // Check if mouse is close enough to the boundary to start dragging
      if (std::abs(worldX - calcBoundaryX) < 8.0f) {
        DBG("MOUSEDOWN: Starting drag!");
        isDraggingBoundary = true;
        dragBoundaryX = calcBoundaryX;
        dragInitialFrame = leftNote->getEndFrame(); // This is the initial boundary frame
        dragLeftNote = leftNote;
        dragRightNote = rightNote;
        originalLeftEndFrame = leftNote->getEndFrame();
        originalRightStartFrame = rightNote->getStartFrame();
        
        // Calculate min/max drag limits based on minimum note duration
        // Minimum note duration is typically around 10 frames (adjust as needed)
        const int minNoteDuration = 10; // Minimum note duration in frames
        
        // Left note can't go beyond right note's start minus min duration
        dragMinFrame = leftNote->getStartFrame() + minNoteDuration;
        
        // Right note can't go beyond left note's end plus min duration
        dragMaxFrame = rightNote->getEndFrame() - minNoteDuration;
        
        // Ensure min/max constraints don't violate each other
        dragMinFrame = std::max(dragMinFrame, leftNote->getStartFrame() + minNoteDuration);
        dragMaxFrame = std::min(dragMaxFrame, rightNote->getEndFrame() - minNoteDuration);
        dragMinFrame = std::min(dragMinFrame, dragMaxFrame);
        
        // Convert to world coordinates
        dragMinX = framesToSeconds(dragMinFrame) * pixelsPerSecond;
        dragMaxX = framesToSeconds(dragMaxFrame) * pixelsPerSecond;
        
        // Trigger repaint immediately so first render sees the dragging state
        owner_.repaint();
        
        return true;
      }
    }
  }
  
  // If not near boundary or not starting drag, proceed with normal split
  if (!isDraggingBoundary && !isNearBoundary_) {
    Note *note = owner_.noteSplitter->findNoteAt(worldX, worldY);
    if (note) {
      owner_.noteSplitter->splitNoteAtX(note, worldX);
      return true;
    }
  }
  
  return false;
}

bool SplitHandler::mouseDrag(const juce::MouseEvent &e, float worldX,
                            float worldY) {
  juce::ignoreUnused(e, worldY);
  
  // Debug: Track first mouseDrag
  static int dragCount = 0;
  if (dragCount < 3) {
    DBG("MOUSEDRAG #" << dragCount++ 
        << ": isDraggingBoundary=" << (isDraggingBoundary ? "true" : "false")
        << ", dragBoundaryX=" << dragBoundaryX
        << ", dragLeftNote=" << (dragLeftNote ? "valid" : "nullptr")
        << ", dragRightNote=" << (dragRightNote ? "valid" : "nullptr"));
  }
  
  if (!isDraggingBoundary || !dragLeftNote || !dragRightNote) {
    return false;
  }
  
  // Clamp the drag position to min/max limits
  float clampedX = juce::jlimit(dragMinX, dragMaxX, worldX);
  dragBoundaryX = clampedX;
  
  owner_.repaint();
  
  return true;
}

bool SplitHandler::mouseUp(const juce::MouseEvent &e, float worldX,
                          float worldY) {
  juce::ignoreUnused(e, worldY, worldX);
  
  if (!isDraggingBoundary || !dragLeftNote || !dragRightNote) {
    isDraggingBoundary = false;
    return false;
  }
  
  // Calculate the final boundary frame from the drag position
  float pixelsPerSecond = owner_.getPixelsPerSecond();
  double time = dragBoundaryX / pixelsPerSecond;
  int newBoundaryFrame = static_cast<int>(time * SAMPLE_RATE / HOP_SIZE);
  
  // Create an undo action for the boundary change (EXACT same pattern as NoteMerge)
  if (owner_.undoManager && dragLeftNote && dragRightNote) {
    // Step 0: Ensure both notes have clip waveforms before adjustment
    auto& audioData = owner_.project->getAudioData();
    if (audioData.waveform.getNumSamples() > 0) {
      const float* src = audioData.waveform.getReadPointer(0);
      const int totalSamples = audioData.waveform.getNumSamples();
      
      for (auto* note : {dragLeftNote, dragRightNote}) {
        if (!note->hasClipWaveform()) {
          int startSample = note->getStartFrame() * HOP_SIZE;
          int endSample = note->getEndFrame() * HOP_SIZE;
          startSample = std::max(0, std::min(startSample, totalSamples));
          endSample = std::max(startSample, std::min(endSample, totalSamples));
          std::vector<float> clip;
          clip.reserve(static_cast<size_t>(endSample - startSample));
          for (int i = startSample; i < endSample; ++i) {
            clip.push_back(src[i]);
          }
          note->setClipWaveform(std::move(clip));
        }
      }
    }
    
    // Step 1: Capture snapshots BEFORE any changes
    Note originalLeftNote = *dragLeftNote;
    Note originalRightNote = *dragRightNote;
    
    // Step 2: Create adjusted note objects (don't modify existing notes yet)
    Note adjustedLeftNote = originalLeftNote;
    Note adjustedRightNote = originalRightNote;
    
    // Apply new boundaries to the adjusted copies
    adjustedLeftNote.setEndFrame(newBoundaryFrame);
    adjustedRightNote.setStartFrame(newBoundaryFrame);
    
    // CRITICAL: Resize originalDeltaPitch vectors to match new duration
    // This ensures the snapshot has correct size for rebuildPitchData
    int leftNewDuration = newBoundaryFrame - adjustedLeftNote.getStartFrame();
    int rightNewDuration = adjustedRightNote.getEndFrame() - newBoundaryFrame;
    
    std::vector<float> leftDelta = adjustedLeftNote.getOriginalDeltaPitch();
    std::vector<float> rightDelta = adjustedRightNote.getOriginalDeltaPitch();
    leftDelta.resize(static_cast<size_t>(leftNewDuration), 0.0f);
    rightDelta.resize(static_cast<size_t>(rightNewDuration), 0.0f);
    adjustedLeftNote.setOriginalDeltaPitch(std::move(leftDelta));
    adjustedRightNote.setOriginalDeltaPitch(std::move(rightDelta));
    
    // Transfer other data between notes when boundary changes (clipWaveform, f0Values)
    adjustBoundaryData(adjustedLeftNote, adjustedRightNote, 
                      originalLeftNote.getEndFrame(), newBoundaryFrame);
    
    // Step 3: Actually apply the changes by removing old notes and adding new ones (same as mergeNotes)
    auto& notes = owner_.project->getNotes();
    
    // Find and remove original notes
    bool leftRemoved = false;
    bool rightRemoved = false;
    
    for (auto it = notes.begin(); it != notes.end(); ) {
      if (!leftRemoved && it->getStartFrame() == originalLeftNote.getStartFrame()) {
        it = notes.erase(it);
        leftRemoved = true;
      } else if (!rightRemoved && it->getStartFrame() == originalRightNote.getStartFrame()) {
        it = notes.erase(it);
        rightRemoved = true;
      } else {
        ++it;
      }
      
      if (leftRemoved && rightRemoved) break;
    }
    
    // Add adjusted notes
    if (leftRemoved && rightRemoved) {
      owner_.project->addNote(adjustedLeftNote);
      owner_.project->addNote(adjustedRightNote);
      
      // Mark notes as synth dirty
      for (auto& n : owner_.project->getNotes()) {
        if (n.getStartFrame() == adjustedLeftNote.getStartFrame() ||
            n.getStartFrame() == adjustedRightNote.getStartFrame()) {
          n.markSynthDirty();
        }
      }
      
      // CRITICAL: Rebuild basePitch and deltaPitch immediately after applying changes (same as NoteMerge)
      // This ensures audioData arrays are consistent with the new note boundaries
      auto& audioData = owner_.project->getAudioData();
      const int totalFrames = audioData.getNumFrames();
      
      // Step 1: Build note segments for base pitch generation
      std::vector<BasePitchCurve::NoteSegment> segments;
      const auto& notes = owner_.project->getNotes();
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
        if (i < static_cast<int>(audioData.voicedMask.size()) && 
            audioData.voicedMask[static_cast<size_t>(i)] && 
            audioData.f0[static_cast<size_t>(i)] > 0.0f) {
          const float f0Midi = freqToMidi(audioData.f0[static_cast<size_t>(i)]);
          audioData.deltaPitch[static_cast<size_t>(i)] = f0Midi - baseMidi;
        } else {
          audioData.deltaPitch[static_cast<size_t>(i)] = 0.0f;
        }
      }
      
      // Step 4: Update cached baseF0
      audioData.baseF0.resize(static_cast<size_t>(totalFrames));
      for (int i = 0; i < totalFrames; ++i) {
        audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);
      }
      
      // Step 5: Sync global deltaPitch back to each note's deltaPitch vectors
      for (auto& n : owner_.project->getNotes()) {
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
      
      // Step 6: Recompose F0 from basePitch and deltaPitch
      PitchCurveProcessor::composeF0InPlace(*owner_.project, /*applyUvMask=*/true);
    }
    
    // Step 4: Create undo action (same as NoteMergeAction call in NoteSplitter.cpp)
    auto action = std::make_unique<NoteBoundaryAdjustAction>(
        owner_.project,
        originalLeftNote, originalRightNote,
        adjustedLeftNote, adjustedRightNote,
        [this]() {
            // Callback for UI refresh (same as NoteMerge)
            owner_.invalidateBasePitchCache();
            
            // Ensure audio data arrays are still large enough after undo/redo
            auto& audioData = owner_.project->getAudioData();
            int maxFrame = 0;
            for (const auto& note : owner_.project->getNotes()) {
                maxFrame = std::max(maxFrame, note.getEndFrame());
            }
            
            if (maxFrame >= static_cast<int>(audioData.f0.size())) {
                size_t newSize = static_cast<size_t>(maxFrame + 100);
                
                if (audioData.f0.size() < newSize) audioData.f0.resize(newSize, 0.0f);
                if (audioData.baseF0.size() < newSize) audioData.baseF0.resize(newSize, 0.0f);
                if (audioData.basePitch.size() < newSize) audioData.basePitch.resize(newSize, 0.0f);
                if (audioData.deltaPitch.size() < newSize) audioData.deltaPitch.resize(newSize, 0.0f);
                if (audioData.voicedMask.size() < newSize) audioData.voicedMask.resize(newSize, false);
                if (audioData.vadMask.size() < newSize) audioData.vadMask.resize(newSize, false);
            }
            owner_.repaint();
        }
    );
    
    owner_.undoManager->addAction(std::move(action));
  }
  
  isDraggingBoundary = false;
  dragLeftNote = nullptr;
  dragRightNote = nullptr;
  
  // CRITICAL: Immediately refresh UI to show updated note boundaries
  owner_.repaint();
  
  return true;
}

void SplitHandler::mouseMove(const juce::MouseEvent &e, float worldX,
                             float worldY) {
  juce::ignoreUnused(e);

  if (!owner_.project) {
    clearGuide();
    return;
  }

  // First check if we're near a note boundary for merging
  float boundaryX = -1.0f;
  Note* boundaryNote = owner_.noteSplitter->findNoteBoundaryAt(worldX, worldY, boundaryX);
  
  if (boundaryNote) {
    // We're near a boundary - show merge highlight
    isNearBoundary_ = true;
    boundaryHighlightX = boundaryX;
    
    // Store frame numbers instead of pointers to avoid dangling pointer issues
    boundaryLeftStartFrame = boundaryNote->getStartFrame();
    boundaryLeftEndFrame = boundaryNote->getEndFrame();
    
    // Find the right note and store its frame numbers
    auto& notes = owner_.project->getNotes();
    boundaryRightStartFrame = -1;
    boundaryRightEndFrame = -1;
    for (const auto& n : notes) {
      if (!n.isRest() && n.getStartFrame() == boundaryNote->getEndFrame()) {
        boundaryRightStartFrame = n.getStartFrame();
        boundaryRightEndFrame = n.getEndFrame();
        break;
      }
    }
    
    // Clear the split guide when showing merge highlight
    splitGuideX = -1.0f;
    splitGuideNote = nullptr;
  } else {
    // Not near boundary - show normal split guide
    isNearBoundary_ = false;
    boundaryHighlightX = -1.0f;
    boundaryLeftStartFrame = -1;
    boundaryLeftEndFrame = -1;
    boundaryRightStartFrame = -1;
    boundaryRightEndFrame = -1;
    
    // Show split guide line at mouse position
    splitGuideX = worldX;
    splitGuideNote = owner_.noteSplitter->findNoteAt(worldX, worldY);
  }
  
  owner_.repaint();
}

void SplitHandler::mouseDoubleClick(const juce::MouseEvent &e, float worldX,
                                    float worldY) {
  juce::ignoreUnused(e);
  
  if (!owner_.project || !owner_.noteSplitter)
    return;
  
  // Check if double-clicking on a note boundary
  float boundaryX = -1.0f;
  Note* boundaryNote = owner_.noteSplitter->findNoteBoundaryAt(worldX, worldY, boundaryX);
  
  if (boundaryNote) {
    // Find the adjacent right note
    auto& notes = owner_.project->getNotes();
    Note* rightNote = nullptr;
    for (auto& n : notes) {
      if (!n.isRest() && n.getStartFrame() == boundaryNote->getEndFrame()) {
        rightNote = &n;
        break;
      }
    }
    
    if (rightNote) {
      // Merge the two notes
      owner_.noteSplitter->mergeNotes(boundaryNote, rightNote);
      clearGuide();
      owner_.repaint();
      return;
    }
  }
  
  // If not near boundary, proceed with normal split
  Note* note = owner_.noteSplitter->findNoteAt(worldX, worldY);
  if (note) {
    owner_.noteSplitter->splitNoteAtX(note, worldX);
    clearGuide();
    owner_.repaint();
  }
}

void SplitHandler::cancel() { clearGuide(); }

void SplitHandler::clearGuide() {
  bool needsRepaint = false;
  
  if (splitGuideX >= 0) {
    splitGuideX = -1.0f;
    splitGuideNote = nullptr;
    needsRepaint = true;
  }
  
  // Always clear boundary state to prevent dangling pointers
  if (isNearBoundary_ || boundaryLeftStartFrame >= 0 || boundaryRightStartFrame >= 0) {
    isNearBoundary_ = false;
    boundaryHighlightX = -1.0f;
    boundaryLeftStartFrame = -1;
    boundaryLeftEndFrame = -1;
    boundaryRightStartFrame = -1;
    boundaryRightEndFrame = -1;
    needsRepaint = true;
  }
  
  if (needsRepaint) {
    owner_.repaint();
  }
}

// Safe getter methods that find notes by frame numbers (not stored pointers)
Note* SplitHandler::getBoundaryLeftNote() const {
  if (!owner_.project || boundaryLeftStartFrame < 0) {
    return nullptr;
  }
  
  auto& notes = owner_.project->getNotes();
  for (const auto& n : notes) {
    if (!n.isRest() && 
        n.getStartFrame() == boundaryLeftStartFrame && 
        n.getEndFrame() == boundaryLeftEndFrame) {
      return const_cast<Note*>(&n);  // Temporary pointer, only valid for current call
    }
  }
  return nullptr;
}

Note* SplitHandler::getBoundaryRightNote() const {
  if (!owner_.project || boundaryRightStartFrame < 0) {
    return nullptr;
  }
  
  auto& notes = owner_.project->getNotes();
  for (const auto& n : notes) {
    if (!n.isRest() &&
        n.getStartFrame() == boundaryRightStartFrame &&
        n.getEndFrame() == boundaryRightEndFrame) {
      return const_cast<Note*>(&n);  // Temporary pointer only valid for current call
    }
  }
  return nullptr;
}

void SplitHandler::adjustBoundaryData(Note& leftNote, Note& rightNote, int oldBoundaryFrame, int newBoundaryFrame) {
    // Reference: NoteSplitter::mergeNotes - we should NOT manually adjust deltaPitch
    // Instead, we should let the rebuildPitchData() in NoteBoundaryAdjustAction handle it
    // which recalculates deltaPitch from f0 and basePitch
    
    int frameDelta = newBoundaryFrame - oldBoundaryFrame;
    
    // Adjust clipWaveform
    adjustClipWaveform(leftNote, rightNote, frameDelta);
    
    // Adjust f0Values
    adjustF0Values(leftNote, rightNote, frameDelta);
}

void SplitHandler::adjustOriginalDeltaPitch(Note& leftNote, Note& rightNote, int oldBoundaryFrame, int newBoundaryFrame) {
    // CRITICAL: Resize originalDeltaPitch vectors to match new duration
    // DO NOT apply basePitch compensation - rebuildPitchData will recalculate from f0
    
    std::vector<float> leftDelta = leftNote.getOriginalDeltaPitch();
    std::vector<float> rightDelta = rightNote.getOriginalDeltaPitch();
    
    int leftNewDuration = leftNote.getEndFrame() - leftNote.getStartFrame();
    int rightNewDuration = rightNote.getEndFrame() - rightNote.getStartFrame();
    
    int frameDelta = newBoundaryFrame - oldBoundaryFrame;
    
    if (frameDelta > 0) {
        // Boundary moved right: left expands, right shrinks
        int leftOldSize = static_cast<int>(leftDelta.size());
        leftDelta.resize(static_cast<size_t>(leftNewDuration), 0.0f);
        
        // Transfer frames from right note to left note (copy directly without adjustment)
        for (int i = 0; i < frameDelta && i < static_cast<int>(rightDelta.size()); ++i) {
            int leftIdx = leftOldSize + i;
            if (leftIdx < leftNewDuration) {
                leftDelta[static_cast<size_t>(leftIdx)] = rightDelta[static_cast<size_t>(i)];
            }
        }
        
        // Shrink right note's delta pitch
        std::vector<float> newRightDelta;
        newRightDelta.reserve(static_cast<size_t>(rightNewDuration));
        for (int i = frameDelta; i < static_cast<int>(rightDelta.size()) && i - frameDelta < rightNewDuration; ++i) {
            newRightDelta.push_back(rightDelta[static_cast<size_t>(i)]);
        }
        newRightDelta.resize(static_cast<size_t>(rightNewDuration), 0.0f);
        
        leftNote.setOriginalDeltaPitch(std::move(leftDelta));
        rightNote.setOriginalDeltaPitch(std::move(newRightDelta));
        
    } else if (frameDelta < 0) {
        // Boundary moved left: left shrinks, right expands
        int absDelta = -frameDelta;
        
        // Save the tail of leftDelta BEFORE resizing (these frames will be transferred to right note)
        std::vector<float> leftTailFrames;
        int leftOldSize = static_cast<int>(leftDelta.size());
        int tailStart = leftNewDuration;
        int tailCount = std::min(absDelta, leftOldSize - tailStart);
        if (tailCount > 0 && tailStart < leftOldSize) {
            leftTailFrames.reserve(static_cast<size_t>(tailCount));
            for (int i = 0; i < tailCount; ++i) {
                leftTailFrames.push_back(leftDelta[static_cast<size_t>(tailStart + i)]);
            }
        }
        
        // Shrink left note's delta pitch
        leftDelta.resize(static_cast<size_t>(leftNewDuration), 0.0f);
        
        // Expand right note's delta pitch
        std::vector<float> newRightDelta(static_cast<size_t>(rightNewDuration), 0.0f);
        
        // Transfer saved tail frames from left note to right note (at the beginning)
        for (int i = 0; i < static_cast<int>(leftTailFrames.size()) && i < rightNewDuration; ++i) {
            newRightDelta[static_cast<size_t>(i)] = leftTailFrames[static_cast<size_t>(i)];
        }
        
        // Copy remaining frames from original right note (starting at index absDelta)
        for (int i = 0; i < static_cast<int>(rightDelta.size()) && (absDelta + i) < rightNewDuration; ++i) {
            newRightDelta[static_cast<size_t>(absDelta + i)] = rightDelta[static_cast<size_t>(i)];
        }
        
        leftNote.setOriginalDeltaPitch(std::move(leftDelta));
        rightNote.setOriginalDeltaPitch(std::move(newRightDelta));
    }
    // If frameDelta == 0, no adjustment needed
}

void SplitHandler::adjustClipWaveform(Note& leftNote, Note& rightNote, int frameDelta) {
    std::vector<float> leftClip = leftNote.getClipWaveform();
    std::vector<float> rightClip = rightNote.getClipWaveform();
    
    // Clip waveforms are stored as samples, not frames
    // Each frame corresponds to HOP_SIZE samples
    int leftNewDuration = leftNote.getEndFrame() - leftNote.getStartFrame();
    int rightNewDuration = rightNote.getEndFrame() - rightNote.getStartFrame();
    int leftNewNumSamples = leftNewDuration * HOP_SIZE;
    int rightNewNumSamples = rightNewDuration * HOP_SIZE;
    int frameDeltaSamples = frameDelta * HOP_SIZE;
    
    if (frameDelta > 0) {
        // Boundary moved right: left expands, right shrinks
        int leftOldSize = static_cast<int>(leftClip.size());
        leftClip.resize(static_cast<size_t>(leftNewNumSamples), 0.0f);
        
        for (int i = 0; i < frameDeltaSamples && i < static_cast<int>(rightClip.size()); ++i) {
            int leftIdx = leftOldSize + i;
            if (leftIdx < leftNewNumSamples) {
                leftClip[static_cast<size_t>(leftIdx)] = rightClip[static_cast<size_t>(i)];
            }
        }
        
        std::vector<float> newRightClip;
        newRightClip.reserve(static_cast<size_t>(rightNewNumSamples));
        for (int i = frameDeltaSamples; i < static_cast<int>(rightClip.size()) && i - frameDeltaSamples < rightNewNumSamples; ++i) {
            newRightClip.push_back(rightClip[static_cast<size_t>(i)]);
        }
        newRightClip.resize(static_cast<size_t>(rightNewNumSamples), 0.0f);
        
        leftNote.setClipWaveform(std::move(leftClip));
        rightNote.setClipWaveform(std::move(newRightClip));
        
    } else if (frameDelta < 0) {
        // Boundary moved left: left shrinks, right expands
        int absDeltaSamples = -frameDeltaSamples;
        
        // Save the tail of leftClip BEFORE resizing (these frames will be transferred to right note)
        std::vector<float> leftTailSamples;
        int leftOldSize = static_cast<int>(leftClip.size());
        int tailStart = leftNewNumSamples;
        int tailCount = std::min(absDeltaSamples, leftOldSize - tailStart);
        if (tailCount > 0 && tailStart < leftOldSize) {
            leftTailSamples.reserve(static_cast<size_t>(tailCount));
            for (int i = 0; i < tailCount; ++i) {
                leftTailSamples.push_back(leftClip[static_cast<size_t>(tailStart + i)]);
            }
        }
        
        // Shrink left note's clip waveform
        leftClip.resize(static_cast<size_t>(leftNewNumSamples), 0.0f);
        
        // Expand right note's clip waveform
        std::vector<float> newRightClip(static_cast<size_t>(rightNewNumSamples), 0.0f);
        
        // Transfer saved tail samples from left note to right note (at the beginning)
        for (int i = 0; i < static_cast<int>(leftTailSamples.size()) && i < rightNewNumSamples; ++i) {
            newRightClip[static_cast<size_t>(i)] = leftTailSamples[static_cast<size_t>(i)];
        }
        
        // Copy remaining frames from original right note (starting at index absDeltaSamples)
        for (int i = 0; i < static_cast<int>(rightClip.size()) && (absDeltaSamples + i) < rightNewNumSamples; ++i) {
            newRightClip[static_cast<size_t>(absDeltaSamples + i)] = rightClip[static_cast<size_t>(i)];
        }
        
        leftNote.setClipWaveform(std::move(leftClip));
        rightNote.setClipWaveform(std::move(newRightClip));
    }
}

void SplitHandler::adjustF0Values(Note& leftNote, Note& rightNote, int frameDelta) {
    std::vector<float> leftF0 = leftNote.getF0Values();
    std::vector<float> rightF0 = rightNote.getF0Values();
    
    int leftNewDuration = leftNote.getEndFrame() - leftNote.getStartFrame();
    int rightNewDuration = rightNote.getEndFrame() - rightNote.getStartFrame();
    
    if (frameDelta > 0) {
        // Boundary moved right: left expands, right shrinks
        int leftOldSize = static_cast<int>(leftF0.size());
        leftF0.resize(static_cast<size_t>(leftNewDuration), 0.0f);
        
        for (int i = 0; i < frameDelta && i < static_cast<int>(rightF0.size()); ++i) {
            int leftIdx = leftOldSize + i;
            if (leftIdx < leftNewDuration) {
                leftF0[static_cast<size_t>(leftIdx)] = rightF0[static_cast<size_t>(i)];
            }
        }
        
        std::vector<float> newRightF0;
        newRightF0.reserve(static_cast<size_t>(rightNewDuration));
        for (int i = frameDelta; i < static_cast<int>(rightF0.size()) && i - frameDelta < rightNewDuration; ++i) {
            newRightF0.push_back(rightF0[static_cast<size_t>(i)]);
        }
        newRightF0.resize(static_cast<size_t>(rightNewDuration), 0.0f);
        
        leftNote.setF0Values(std::move(leftF0));
        rightNote.setF0Values(std::move(newRightF0));
        
    } else if (frameDelta < 0) {
        // Boundary moved left: left shrinks, right expands
        int absDelta = -frameDelta;
        
        leftF0.resize(static_cast<size_t>(leftNewDuration), 0.0f);
        
        std::vector<float> newRightF0(static_cast<size_t>(rightNewDuration), 0.0f);
        
        int copyCount = std::min(absDelta, static_cast<int>(leftF0.size()));
        for (int i = 0; i < copyCount; ++i) {
            int leftIdx = leftNewDuration + i;
            if (leftIdx < static_cast<int>(leftF0.size())) {
                newRightF0[static_cast<size_t>(i)] = leftF0[static_cast<size_t>(leftIdx)];
            }
        }
        
        for (int i = 0; i < static_cast<int>(rightF0.size()) && i + absDelta < rightNewDuration; ++i) {
            newRightF0[static_cast<size_t>(absDelta + i)] = rightF0[static_cast<size_t>(i)];
        }
        
        leftNote.setF0Values(std::move(leftF0));
        rightNote.setF0Values(std::move(newRightF0));
    }
}
