#include "SplitHandler.h"
#include "../../PianoRollComponent.h"
#include "../NoteSplitter.h"

SplitHandler::SplitHandler(PianoRollComponent &owner)
    : InteractionHandler(owner) {}

bool SplitHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                             float worldY) {
  juce::ignoreUnused(e);

  // If near a boundary, don't split (will be handled by double-click merge)
  if (isNearBoundary_) {
    return false;
  }

  Note *note = owner_.noteSplitter->findNoteAt(worldX, worldY);
  if (note) {
    owner_.noteSplitter->splitNoteAtX(note, worldX);
    return true;
  }
  return false;
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
    
    Note *note = owner_.noteSplitter->findNoteAt(worldX, worldY);
    if (note) {
      splitGuideX = worldX;
      splitGuideNote = note;
    } else {
      splitGuideX = -1.0f;
      splitGuideNote = nullptr;
    }
  }
  
  owner_.repaint();
}

void SplitHandler::mouseDoubleClick(const juce::MouseEvent &e, float worldX,
                                    float worldY) {
  juce::ignoreUnused(e, worldX, worldY);
  
  // Only merge if we're near a boundary
  if (isNearBoundary_) {
    // Get notes through safe getter methods that find by frame numbers
    Note* leftNote = getBoundaryLeftNote();
    Note* rightNote = getBoundaryRightNote();
    
    if (leftNote && rightNote) {
      owner_.noteSplitter->mergeNotes(leftNote, rightNote);
    }
    
    // Clear the highlight after merging
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
      return const_cast<Note*>(&n);  // Temporary pointer, only valid for current call
    }
  }
  return nullptr;
}
