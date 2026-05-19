#pragma once

#include <vector>

namespace PitchToolOperations {

/**
 * Applies a linear tilt around a pivot position.
 *
 * The value at the pivot stays unchanged, and the contour is shifted
 * linearly across the note. The furthest end from the pivot reaches
 * the full `amount` in semitones.
 */
std::vector<float> tiltDeltaPitch(const std::vector<float>& deltaPitch,
                                  float pivotPosition,
                                  float amount);

/**
 * Cubic Bezier curve tilt: uses standard cubic Bezier formula with control points on boundary vertical lines
 * 
 * Control points configuration (matching bezier_tilt_demo.py):
 * - P0 = (t=0, Y=0)           : Left endpoint (fixed at origin)
 * - P1 = (t=0, Y=leftOffset)  : Left control point (X fixed at 0, Y adjustable)
 * - P2 = (t=1, Y=rightOffset) : Right control point (X fixed at 1, Y adjustable)
 * - P3 = (t=1, Y=0)           : Right endpoint (fixed at origin)
 * 
 * Behavior:
 * - Left offset only (leftOffset≠0, rightOffset=0): Curve starts at leftOffset and smoothly returns to 0 at right end ↘️
 * - Right offset only (leftOffset=0, rightOffset≠0): Curve starts at 0 and smoothly rises to rightOffset at right end ↗️
 * - Both offsets: Creates arch or S-shape depending on signs
 * 
 * This ensures C1 continuity at boundaries (offset=0 and first derivative=0 at both ends),
 * preventing pitch discontinuities between adjacent notes.
 * 
 * @param deltaPitch The original delta pitch curve
 * @param leftOffset Y-axis offset of left control point P1 (at t=0) in semitones
 * @param rightOffset Y-axis offset of right control point P2 (at t=1) in semitones
 * @return Transformed delta pitch curve with smooth Bezier tilt applied
 */
std::vector<float> splineTiltDeltaPitch(const std::vector<float>& deltaPitch,
                                        float leftOffset,
                                        float rightOffset);

/**
 * Scales deviations from the base MIDI note (zero).
 *
 * `factor = 0` flattens to zero (base MIDI note) and `factor = 1` keeps
 * the original contour unchanged.
 */
std::vector<float> reduceVariance(const std::vector<float>& deltaPitch,
                                  float factor);

/**
 * Smooths one boundary to connect with adjacent pitch context.
 *
 * For left side, fades from `targetPitch` to the note boundary.
 * For right side, fades from the note boundary to `targetPitch`.
 * Cosine interpolation is used to avoid abrupt slope changes.
 */
std::vector<float> smoothBoundary(const std::vector<float>& deltaPitch,
                                  int side,
                                  int transitionFrames,
                                  float targetPitch);

/**
 * Computes the arithmetic mean of a pitch contour.
 * Returns 0 when the input is empty.
 */
float computeMean(const std::vector<float>& deltaPitch);

/**
 * Context for adjacent notes (for boundary smoothing).
 * Stores boundary delta pitch values from temporally adjacent notes.
 */
struct AdjacentNoteContext
{
  bool hasLeft = false;           // True if a previous note exists
  bool hasRight = false;          // True if a next note exists
  float leftBoundaryDelta = 0.0f;  // Last delta value of previous note
  float rightBoundaryDelta = 0.0f; // First delta value of next note
};

/**
 * Applies all transformation parameters non-destructively.
 * 
 * This function chains multiple transformations in order:
 * 1. Variance scaling
 * 2. Tilt (left and right combined)
 * 3. High-pass flattening
 * 
 * @param originalDelta The pristine deltaPitch curve from analysis (never modified)
 * @param tiltLeft Tilt amount at left edge in semitones
 * @param tiltRight Tilt amount at right edge in semitones
 * @param varianceScale Variance scaling factor (1.0=unchanged, 0.0=flat, >1.0=amplify, <0.0=invert)
 * @param highPassCutoff High-pass filter cutoff ratio (0.0=no effect, 1.0=fully flat)
 * @param adjacentContext Context for adjacent notes
 * @return Transformed deltaPitch curve
 */
std::vector<float> applyAllTransformations(const std::vector<float>& originalDelta,
                                           float tiltLeft,
                                           float tiltRight,
                                           float varianceScale,
                                           float highPassCutoff = 0.0f,
                                           const AdjacentNoteContext& adjacentContext = {});

/**
 * High-pass filtering flattening algorithm.
 * 
 * Treats the F0 curve as a time-domain signal and applies high-pass filtering
 * to gradually remove low-frequency trends while preserving high-frequency details.
 * 
 * @param f0Curve The original F0 curve (frequency values over time)
 * @param cutoffRatio Controls the degree of flattening (0.0=no effect, 1.0=fully flat)
 * @return Flattened F0 curve with details preserved
 */
std::vector<float> highPassFlatten(const std::vector<float>& f0Curve, float cutoffRatio);

} // namespace PitchToolOperations