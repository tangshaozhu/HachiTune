#pragma once

#include "InteractionHandler.h"
#include <vector>

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
  bool mouseDrag(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseUp(const juce::MouseEvent &e, float worldX,
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
  void adjustBoundaryData(Note& leftNote, Note& rightNote, int oldBoundaryFrame, int newBoundaryFrame);
  void adjustOriginalDeltaPitch(Note& leftNote, Note& rightNote, int oldBoundaryFrame, int newBoundaryFrame);
  void adjustClipWaveform(Note& leftNote, Note& rightNote, int frameDelta);
  void adjustF0Values(Note& leftNote, Note& rightNote, int frameDelta);

  float splitGuideX = -1.0f;
  Note *splitGuideNote = nullptr;
  
  // Boundary merge state - store frame numbers instead of pointers!
  float boundaryHighlightX = -1.0f;
  int boundaryLeftStartFrame = -1;   // ← 左音符起始帧（唯一标识）
  int boundaryLeftEndFrame = -1;     // ← 左音符结束帧
  int boundaryRightStartFrame = -1;  // ← 右音符起始帧
  int boundaryRightEndFrame = -1;    // ← 右音符结束帧
  bool isNearBoundary_ = false;
  
  // Boundary dragging state
  bool isDraggingBoundary = false;
  float dragBoundaryX = -1.0f;
  int dragInitialFrame = -1;
  int dragMinFrame = -1;
  int dragMaxFrame = -1;
  float dragMinX = -1.0f;
  float dragMaxX = -1.0f;
  Note* dragLeftNote = nullptr;
  Note* dragRightNote = nullptr;
  int originalLeftEndFrame = -1;
  int originalRightStartFrame = -1;
  
public:
  // Accessors for boundary dragging state
  bool getIsDraggingBoundary() const { return isDraggingBoundary; }
  float getDragBoundaryX() const { return dragBoundaryX; }
  Note* getDragLeftNote() const { return dragLeftNote; }
  Note* getDragRightNote() const { return dragRightNote; }
};