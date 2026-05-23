#include "PitchToolOperations.h"
#include "../Models/Note.h"
#include "Constants.h"  // 添加这一行以引入midiToFreq函数
#include <algorithm>
#include <cmath>
#include <numeric>

namespace PitchToolOperations {

std::vector<float> tiltDeltaPitch(const std::vector<float>& deltaPitch,
                                  float pivotPosition,
                                  float amount) {
  if (deltaPitch.empty()) {
    return {};
  }

  std::vector<float> result(deltaPitch);
  if (deltaPitch.size() == 1) {
    return result;
  }

  const float clampedPivot = std::clamp(pivotPosition, 0.0f, 1.0f);
  const float maxDistance = std::max(clampedPivot, 1.0f - clampedPivot);
  if (maxDistance <= 0.0f) {
    return result;
  }

  const float invLastIndex = 1.0f / static_cast<float>(deltaPitch.size() - 1);
  for (size_t i = 0; i < deltaPitch.size(); ++i) {
    const float normalizedPosition = static_cast<float>(i) * invLastIndex;
    // Normalize by furthest edge distance so `amount` means a full-end shift.
    const float normalizedDistance =
        (normalizedPosition - clampedPivot) / maxDistance;
    result[i] = deltaPitch[i] + normalizedDistance * amount;
  }

  return result;
}

// Cubic Bezier curve tilt: uses standard cubic Bezier formula with control points on boundary vertical lines
// P0 = (t=0, Y=0), P1 = (t=0, Y=leftOffset), P2 = (t=1, Y=rightOffset), P3 = (t=1, Y=0)
// 
// This matches the implementation in bezier_tilt_demo.py exactly.
// IMPORTANT: Uses uniform X-axis sampling (not uniform parameter t) to match full_bezier.png behavior.
// For each frame position, we numerically solve for t such that B_x(t) = target_x, then compute B_y(t).
//
// Behavior:
// - Left offset only: Curve starts at leftOffset and smoothly returns to 0 at right end ↘️
// - Right offset only: Curve starts at 0 and smoothly rises to rightOffset at right end ↗️
// - Both offsets: Creates arch or S-shape depending on signs
std::vector<float> splineTiltDeltaPitch(const std::vector<float>& deltaPitch,
                                        float leftOffset,   // Y-axis offset of left control point P1 (at t=0)
                                        float rightOffset) {  // Y-axis offset of right control point P2 (at t=1)
  if (deltaPitch.empty()) {
    return {};
  }

  const size_t n = deltaPitch.size();
  std::vector<float> result(n);
  
  if (n == 1) {
    // For single frame, apply both offsets equally
    result[0] = deltaPitch[0] + leftOffset + rightOffset;
    return result;
  }

  // Control points for cubic Bezier
  const float P0_x = 0.0f, P0_y = 0.0f;
  const float P1_x = 0.0f, P1_y = leftOffset;
  const float P2_x = 1.0f, P2_y = rightOffset;
  const float P3_x = 1.0f, P3_y = 0.0f;

  // Precompute a lookup table: t -> B_x(t) with high resolution
  constexpr int LUT_SIZE = 1000;
  std::vector<float> lut_t(LUT_SIZE);
  std::vector<float> lut_Bx(LUT_SIZE);
  
  for (int i = 0; i < LUT_SIZE; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(LUT_SIZE - 1);
    lut_t[i] = t;
    // B_x(t) = (1-t)³*P0_x + 3*(1-t)²*t*P1_x + 3*(1-t)*t²*P2_x + t³*P3_x
    const float oneMinusT = 1.0f - t;
    lut_Bx[i] = oneMinusT * oneMinusT * oneMinusT * P0_x +
                3.0f * oneMinusT * oneMinusT * t * P1_x +
                3.0f * oneMinusT * t * t * P2_x +
                t * t * t * P3_x;
  }

  // For each frame, find t such that B_x(t) = target_x using binary search on LUT
  const float invLastIndex = 1.0f / static_cast<float>(n - 1);
  
  for (size_t i = 0; i < n; ++i) {
    const float target_x = static_cast<float>(i) * invLastIndex;  // Uniform X position [0, 1]
    
    // Binary search in LUT to find t where B_x(t) ≈ target_x
    int left = 0, right = LUT_SIZE - 1;
    while (left < right - 1) {
      const int mid = (left + right) / 2;
      if (lut_Bx[mid] < target_x) {
        left = mid;
      } else {
        right = mid;
      }
    }
    
    // Linear interpolation between left and right for more accurate t
    const float t_left = lut_t[left];
    const float t_right = lut_t[right];
    const float Bx_left = lut_Bx[left];
    const float Bx_right = lut_Bx[right];
    
    float t;
    if (std::abs(Bx_right - Bx_left) > 1e-6f) {
      t = t_left + (target_x - Bx_left) / (Bx_right - Bx_left) * (t_right - t_left);
    } else {
      t = (t_left + t_right) * 0.5f;
    }
    
    // Now compute B_y(t) using the found t value
    const float oneMinusT = 1.0f - t;
    const float bezierOffset = oneMinusT * oneMinusT * oneMinusT * P0_y +
                               3.0f * oneMinusT * oneMinusT * t * P1_y +
                               3.0f * oneMinusT * t * t * P2_y +
                               t * t * t * P3_y;
    
    result[i] = deltaPitch[i] + bezierOffset;
  }

  return result;
}

std::vector<float> reduceVariance(const std::vector<float>& deltaPitch,
                                  float factor) {
  if (deltaPitch.empty()) {
    return {};
  }

  // When factor is close to 1, no change needed
  if (std::abs(factor - 1.0f) < 0.001f) {
    return deltaPitch;
  }

  std::vector<float> result(deltaPitch.size());
  const size_t n = deltaPitch.size();
  
  // Boundary width based on SMOOTH_WINDOW (0.04s = ~3.45 frames at 44100Hz/512 hop)
  // Convert time window to frame count for consistent behavior across different sample rates
  constexpr double SMOOTH_WINDOW_SEC = 0.06;  // 60ms total window
  constexpr int HOP_SIZE = 512;
  constexpr int SAMPLE_RATE = 44100;
  const int smoothWindowFrames = static_cast<int>(std::round(
      SMOOTH_WINDOW_SEC * SAMPLE_RATE / HOP_SIZE));  // ≈ 3-4 frames
  
  // Use smoothWindowFrames as boundary width (proportional to transition smoothness)
  const size_t boundaryWidth = std::max(static_cast<size_t>(1), 
                                        static_cast<size_t>(smoothWindowFrames));
  
  for (size_t i = 0; i < n; ++i) {
    // Calculate distance from nearest boundary (0.0 to 1.0)
    float boundaryDist = 0.0f;
    if (i < boundaryWidth) {
      // Left boundary region
      boundaryDist = static_cast<float>(i) / static_cast<float>(boundaryWidth);
    } else if (i >= n - boundaryWidth) {
      // Right boundary region
      boundaryDist = static_cast<float>(n - 1 - i) / static_cast<float>(boundaryWidth);
    } else {
      // Middle region - fully apply factor
      boundaryDist = 1.0f;
    }
    
    // Use cosine interpolation for smooth transition
    // At boundary (dist=0): weight=1.0 (no scaling)
    // At middle (dist=1.0): weight=factor
    const float smoothWeight = 0.5f * (1.0f - std::cos(boundaryDist * 3.14159265f));
    const float effectiveFactor = 1.0f + (factor - 1.0f) * smoothWeight;
    
    result[i] = deltaPitch[i] * effectiveFactor;
  }

  return result;
}

std::vector<float> smoothBoundary(const std::vector<float>& deltaPitch,
                                  int side,
                                  int transitionFrames,
                                  float targetPitch) {
  if (deltaPitch.empty()) {
    return {};
  }

  std::vector<float> result(deltaPitch);
  if (transitionFrames <= 0 || (side != 0 && side != 1)) {
    return result;
  }

  const int clampedFrames = std::max(
      1, std::min(transitionFrames, static_cast<int>(deltaPitch.size())));

  // Gaussian kernel: sigma = transitionFrames / 2.0
  // Weight at boundary = 1.0 (full target), weight at edge of transition = ~0.14
  const float sigma = static_cast<float>(clampedFrames) / 2.0f;
  const float invTwoSigmaSq = 1.0f / (2.0f * sigma * sigma);

  if (side == 0) {
    // Left boundary: blend FROM targetPitch TO note's internal curve
    for (int i = 0; i < clampedFrames; ++i) {
      // Distance from boundary (frame 0)
      const float dist = static_cast<float>(i);
      // Gaussian weight: 1.0 at boundary, decreasing toward interior
      const float gaussWeight = std::exp(-dist * dist * invTwoSigmaSq);
      // Blend: high gaussWeight = more targetPitch, low = more original
      result[static_cast<size_t>(i)] =
          targetPitch * gaussWeight + deltaPitch[static_cast<size_t>(i)] * (1.0f - gaussWeight);
    }
  } else {
    // Right boundary: blend FROM note's internal curve TO targetPitch
    const size_t startIndex = deltaPitch.size() - static_cast<size_t>(clampedFrames);
    for (int i = 0; i < clampedFrames; ++i) {
      // Distance from boundary (last frame)
      const float dist = static_cast<float>(clampedFrames - 1 - i);
      // Gaussian weight: 1.0 at boundary, decreasing toward interior
      const float gaussWeight = std::exp(-dist * dist * invTwoSigmaSq);
      const size_t index = startIndex + static_cast<size_t>(i);
      // Blend: high gaussWeight = more targetPitch, low = more original
      result[index] =
          targetPitch * gaussWeight + deltaPitch[index] * (1.0f - gaussWeight);
    }
  }

  return result;
}

float computeMean(const std::vector<float>& deltaPitch) {
  if (deltaPitch.empty()) {
    return 0.0f;
  }

  const float sum =
      std::accumulate(deltaPitch.begin(), deltaPitch.end(), 0.0f);
  return sum / static_cast<float>(deltaPitch.size());
}

std::vector<float> applyAllTransformations(const std::vector<float>& originalDelta,
                                           float tiltLeft,
                                           float tiltRight,
                                           float varianceScale,
                                           float highPassCutoff,
                                           float bezierTiltLeft,
                                           float bezierTiltRight,
                                           const AdjacentNoteContext& /*adjacentContext*/) {
  if (originalDelta.empty()) {
    return {};
  }

  // Start with the original pristine curve
  std::vector<float> result = originalDelta;

  // 1. Apply linear tilt transformations (independent control)
  // TiltLeft: pivot at right edge (1.0), creates slope from left to right
  if (std::abs(tiltLeft) > 0.001f) {
    result = tiltDeltaPitch(result, 1.0f, -tiltLeft);
  }
  
  // TiltRight: pivot at left edge (0.0), creates slope from right to left
  if (std::abs(tiltRight) > 0.001f) {
    result = tiltDeltaPitch(result, 0.0f, tiltRight);
  }
  
  // 2. Apply Bezier curve tilt for smooth curvature (separate parameters)
  // Control points: P0=(0,0), P1=(0,bezierTiltLeft), P2=(1,bezierTiltRight), P3=(1,0)
  if (std::abs(bezierTiltLeft) > 0.001f || std::abs(bezierTiltRight) > 0.001f) {
    result = splineTiltDeltaPitch(result, bezierTiltLeft, bezierTiltRight);
  }

  // 3. Apply variance scaling
  if (std::abs(varianceScale - 1.0f) > 0.001f) {
    result = reduceVariance(result, varianceScale);
  }

  // 4. Apply high-pass flattening if needed
  if (std::abs(highPassCutoff) > 0.001f) {
    result = highPassFlatten(result, highPassCutoff);
  }

  return result;
}

// 优化：一阶高通滤波 + 边缘40ms余弦窗混合算法，模拟Autotune的行为
std::vector<float> highPassFlatten(const std::vector<float>& deltapitch, float cutoffRatio) {
    if (deltapitch.empty()) {
        return deltapitch;
    }
    
    const size_t n = deltapitch.size();
    
    if (cutoffRatio <= 0.0f) {
        return deltapitch;
    }

    // Identify voiced segments: consecutive frames where |deltaPitch| > threshold.
    // Each segment is filtered independently so that non-voiced gaps
    // (e.g. breathy consonants) between them don't pollute the IIR state.
    constexpr float kVoicedThreshold = 1e-6f;
    struct VoicedSeg { int start; int end; };
    std::vector<VoicedSeg> segments;
    {
        int segStart = -1;
        for (size_t i = 0; i < n; ++i)
        {
            if (std::abs(deltapitch[i]) > kVoicedThreshold)
            {
                if (segStart < 0)
                    segStart = static_cast<int>(i);
            }
            else
            {
                if (segStart >= 0)
                {
                    segments.push_back({segStart, static_cast<int>(i) - 1});
                    segStart = -1;
                }
            }
        }
        if (segStart >= 0)
            segments.push_back({segStart, static_cast<int>(n) - 1});
    }

    if (segments.empty())
        return deltapitch;

    const float alpha = 1.0f - cutoffRatio;
    constexpr double SMOOTH_WINDOW_SEC = 0.06;
    constexpr int HOP_SIZE = 512;
    constexpr int SAMPLE_RATE = 44100;
    const int smoothWindowFrames = std::max(2, static_cast<int>(std::round(
        SMOOTH_WINDOW_SEC * SAMPLE_RATE / HOP_SIZE)));

    std::vector<float> result = deltapitch;

    for (const auto &seg : segments)
    {
        const int segLen = seg.end - seg.start + 1;

        // Apply IIR high-pass filter within this voiced segment only
        std::vector<float> filtered(static_cast<size_t>(segLen));
        filtered[0] = deltapitch[static_cast<size_t>(seg.start)];
        for (int i = 1; i < segLen; ++i)
        {
            const int srcIdx = seg.start + i;
            filtered[static_cast<size_t>(i)] =
                alpha * (filtered[static_cast<size_t>(i - 1)] +
                         deltapitch[static_cast<size_t>(srcIdx)] -
                         deltapitch[static_cast<size_t>(srcIdx - 1)]);
        }

        // Apply cosine window blending at segment boundaries
        for (int i = 0; i < segLen; ++i)
        {
            const int distToLeft = i;
            const int distToRight = segLen - 1 - i;
            const int distToNearest = std::min(distToLeft, distToRight);

            float blendWeight = 1.0f;
            if (distToNearest < smoothWindowFrames)
            {
                const float nd = static_cast<float>(distToNearest) /
                                 static_cast<float>(smoothWindowFrames);
                blendWeight = 0.5f * (1.0f - std::cos(nd * 3.14159265f));
            }

            const int g = seg.start + i;
            result[static_cast<size_t>(g)] =
                deltapitch[static_cast<size_t>(g)] * (1.0f - blendWeight) +
                filtered[static_cast<size_t>(i)] * blendWeight;
        }
    }

    return result;
}

} // namespace PitchToolOperations