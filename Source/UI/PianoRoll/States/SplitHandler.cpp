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
    boundaryLeftNote = boundaryNote;
    
    // Find the right note (the one that starts where left note ends)
    auto& notes = owner_.project->getNotes();
    for (auto& n : notes) {
      if (!n.isRest() && n.getStartFrame() == boundaryNote->getEndFrame()) {
        boundaryRightNote = &n;
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
    boundaryLeftNote = nullptr;
    boundaryRightNote = nullptr;
    
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
  if (isNearBoundary_ && boundaryLeftNote && boundaryRightNote) {
    owner_.noteSplitter->mergeNotes(boundaryLeftNote, boundaryRightNote);
    
    // Clear the highlight after merging
    isNearBoundary_ = false;
    boundaryHighlightX = -1.0f;
    boundaryLeftNote = nullptr;
    boundaryRightNote = nullptr;
    
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
  
  if (isNearBoundary_) {
    isNearBoundary_ = false;
    boundaryHighlightX = -1.0f;
    boundaryLeftNote = nullptr;
    boundaryRightNote = nullptr;
    needsRepaint = true;
  }
  
  if (needsRepaint) {
    owner_.repaint();
  }
}
