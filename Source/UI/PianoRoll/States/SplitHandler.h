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
  Note* getBoundaryLeftNote() const;   // ← 通过帧数实时查找
  Note* getBoundaryRightNote() const;  // ← 通过帧数实时查找
  bool isNearBoundary() const { return isNearBoundary_; }
  
  void clearGuide();

private:
  float splitGuideX = -1.0f;
  Note *splitGuideNote = nullptr;
  
  // Boundary merge state - store frame numbers instead of pointers!
  float boundaryHighlightX = -1.0f;
  int boundaryLeftStartFrame = -1;   // ← 左音符起始帧（唯一标识）
  int boundaryLeftEndFrame = -1;     // ← 左音符结束帧
  int boundaryRightStartFrame = -1;  // ← 右音符起始帧
  int boundaryRightEndFrame = -1;    // ← 右音符结束帧
  bool isNearBoundary_ = false;
};
