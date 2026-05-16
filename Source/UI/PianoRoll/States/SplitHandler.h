#pragma once

#include "InteractionHandler.h"

class Note;
class NoteSplitter;

/**
 * Handles note splitting interactions in Split edit mode.
 * Manages the split guide line, boundary highlight, and executes note splits/merges.
 */
class SplitHandler : public InteractionHandler {
public:
  explicit SplitHandler(PianoRollComponent &owner);

  bool mouseDown(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  void mouseMove(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  void mouseDoubleClick(const juce::MouseEvent &e, float worldX,
                        float worldY) override;
  bool isActive() const override { return false; }
  void cancel() override;

  // Accessors for rendering
  float getSplitGuideX() const { return splitGuideX; }
  Note *getSplitGuideNote() const { return splitGuideNote; }
  
  // Boundary merge highlight accessors
  float getBoundaryHighlightX() const { return boundaryHighlightX; }
  Note* getBoundaryLeftNote() const { return boundaryLeftNote; }
  Note* getBoundaryRightNote() const { return boundaryRightNote; }
  bool isNearBoundary() const { return isNearBoundary_; }
  
  void clearGuide();

private:
  float splitGuideX = -1.0f;
  Note *splitGuideNote = nullptr;
  
  // Boundary merge state
  float boundaryHighlightX = -1.0f;
  Note* boundaryLeftNote = nullptr;
  Note* boundaryRightNote = nullptr;
  bool isNearBoundary_ = false;
};
