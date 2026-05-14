#include "PitchToolHandles.h"
#include <algorithm>
#include <limits>

PitchToolHandles::PitchToolHandles() {
  // Initialize (currently empty, but reserve space)
  handles.reserve(20);  // Typical max handles for multi-note selection
}

void PitchToolHandles::updateHandles(const std::vector<Note*>& selectedNotes,
                                     const CoordinateMapper& mapper) {
  handles.clear();

  if (selectedNotes.empty())
    return;

  // Compute bounding box in musical coordinates
  int minStartFrame = std::numeric_limits<int>::max();
  int maxEndFrame = std::numeric_limits<int>::min();
  float minMidi = std::numeric_limits<float>::max();
  float maxMidi = std::numeric_limits<float>::min();

  for (const auto* note : selectedNotes) {
    if (!note) continue;
    minStartFrame = std::min(minStartFrame, note->getStartFrame());
    maxEndFrame = std::max(maxEndFrame, note->getEndFrame());
    minMidi = std::min(minMidi, note->getAdjustedMidiNote());
    maxMidi = std::max(maxMidi, note->getAdjustedMidiNote());
  }

  // Convert to WORLD coordinates (not screen - mouse events are in world space)
  // Start/End times
  float startSec = mapper.framesToSeconds(minStartFrame);
  float endSec = mapper.framesToSeconds(maxEndFrame);
  
  float leftX = mapper.timeToX(startSec);
  float rightX = mapper.timeToX(endSec);

  // Pitch range
  // Note: High pitch = Low Y value (Top of screen)
  // Low pitch = High Y value (Bottom of screen)
  // We want the visual box.
  // Top Y corresponds to the highest MIDI note.
  float topY = mapper.midiToY(maxMidi);
  
  // Bottom Y corresponds to the lowest MIDI note + 1 semitone height (since note has height)
  // midiToY(minMidi) gives the top of the lowest note.
  // midiToY(minMidi) + pixelsPerSemitone gives the bottom of the lowest note.
  float bottomY = mapper.midiToY(minMidi) + mapper.getPixelsPerSemitone();

  // Ensure valid dimensions
  if (rightX < leftX) std::swap(leftX, rightX);
  if (bottomY < topY) std::swap(topY, bottomY);

  float centerX = (leftX + rightX) * 0.5f;
  float centerY = (topY + bottomY) * 0.5f;

  // Add Handles
  // 1. Tilt Left: Left edge, vertically centered
  addHandle(HandleType::TiltLeft, leftX, centerY);

  // 2. Tilt Right: Right edge, vertically centered
  addHandle(HandleType::TiltRight, rightX, centerY);

  // 3. Reduce Variance: Top edge, horizontally centered
  addHandle(HandleType::ReduceVariance, centerX, topY);

  // 4. Smooth Left: Top-Left corner
  addHandle(HandleType::SmoothLeft, leftX, topY);

  // 5. Smooth Right: Top-Right corner
  addHandle(HandleType::SmoothRight, rightX, topY);
  
  // 6. High Pass Flatten: Bottom-Right corner (右下角)
  addHandle(HandleType::HighPassFlatten, rightX, bottomY);
}

void PitchToolHandles::draw(juce::Graphics& g) const {
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    const auto& handle = handles[i];
    bool isHovered = (i == hoveredHandleIndex);

    g.setColour(handle.color);
    
    auto drawBounds = handle.bounds;
    
    // Draw filled circle
    g.fillEllipse(drawBounds);
    
    // Draw outline for better visibility against note backgrounds
    g.setColour(handle.color.darker(0.3f));
    g.drawEllipse(drawBounds, 1.0f);
  }
}

int PitchToolHandles::hitTest(float worldX, float worldY, float tolerance) const {
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    auto center = handles[i].bounds.getCentre();
    float distance = center.getDistanceFrom(juce::Point<float>(worldX, worldY));
    
    if (distance <= tolerance) {
      return i;
    }
  }
  return -1;
}

void PitchToolHandles::addHandle(HandleType type, float worldX, float worldY, Note* note) {
  Handle h;
  h.type = type;
  h.note = note;
  h.color = getColorForType(type);
  
  // Center the handle bounds on the coordinate (now in world space)
  float halfSize = HANDLE_SIZE * 0.5f;
  h.bounds = juce::Rectangle<float>(worldX - halfSize, worldY - halfSize, 
                                   HANDLE_SIZE, HANDLE_SIZE);
  
  handles.push_back(h);
}

juce::Colour PitchToolHandles::getColorForType(PitchToolHandles::HandleType type) const {
  switch (type) {
    case HandleType::TiltLeft:
      return juce::Colours::cyan;
    case HandleType::TiltRight:
      return juce::Colours::cyan;
    case HandleType::ReduceVariance:
      return juce::Colours::yellow;
    case HandleType::SmoothLeft:
      return juce::Colours::orange;
    case HandleType::SmoothRight:
      return juce::Colours::orange;
    case HandleType::HighPassFlatten:  // 新增高通滤波手柄颜色
      return juce::Colours::green;
    case HandleType::None:
      return juce::Colours::white;
  }
  return juce::Colours::white;
}
