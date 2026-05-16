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
  constexpr double SMOOTH_WINDOW_SEC = 0.04;  // 40ms total window
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
                                           int smoothLeftFrames,
                                           int smoothRightFrames,
                                           float highPassCutoff,
                                           const AdjacentNoteContext& adjacentContext) {
  if (originalDelta.empty()) {
    return {};
  }

  // Start with the original pristine curve
  std::vector<float> result = originalDelta;

  // 1. Apply tilt transformations (combined left + right)
  // TiltLeft: pivot at right (1.0), negative amount
  if (std::abs(tiltLeft) > 0.001f) {
    result = tiltDeltaPitch(result, 1.0f, -tiltLeft);
  }
  
  // TiltRight: pivot at left (0.0), positive amount
  if (std::abs(tiltRight) > 0.001f) {
    result = tiltDeltaPitch(result, 0.0f, tiltRight);
  }

  // 2. Apply variance scaling
  if (std::abs(varianceScale - 1.0f) > 0.001f) {
    result = reduceVariance(result, varianceScale);
  }

  // 3. Apply high-pass flattening if needed
  if (std::abs(highPassCutoff) > 0.001f) {
    // Convert delta pitch back to F0 for processing
    std::vector<float> f0Curve(result.size());
    const float basePitch = 0.0f;  // We're working with delta from base
    for (size_t i = 0; i < result.size(); ++i) {
        f0Curve[i] = std::pow(2.0f, result[i] / 12.0f) * midiToFreq(basePitch);
    }
    
    // Apply high-pass filter
    auto filteredF0 = highPassFlatten(f0Curve, highPassCutoff);
    
    // Convert back to delta pitch
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = 12.0f * std::log2(std::max(filteredF0[i], 1e-6f) / midiToFreq(basePitch));
    }
  }

  // 4. Apply boundary smoothing AFTER variance scaling and high-pass filtering
  if (smoothLeftFrames > 0) {
    const float leftTarget = adjacentContext.hasLeft ? adjacentContext.leftBoundaryDelta : 0.0f;
    result = smoothBoundary(result, 0, smoothLeftFrames, leftTarget);
  }
  
  if (smoothRightFrames > 0) {
    const float rightTarget = adjacentContext.hasRight ? adjacentContext.rightBoundaryDelta : 0.0f;
    result = smoothBoundary(result, 1, smoothRightFrames, rightTarget);
  }

  return result;
}

// 新增：一阶高通滤波平直化算法，模拟Autotune的行为
std::vector<float> highPassFlatten(const std::vector<float>& f0Curve, float cutoffRatio) {
    if (f0Curve.empty()) {
        return f0Curve;
    }
    
    if (cutoffRatio <= 0.0f) {
        return f0Curve;  // 不滤波，返回原曲线
    }
    
    if (cutoffRatio >= 1.0f) {
        // 完全滤波，返回均值
        const float mean = std::accumulate(f0Curve.begin(), f0Curve.end(), 0.0f) / static_cast<float>(f0Curve.size());
        return std::vector<float>(f0Curve.size(), mean);
    }
    
    // 计算原始曲线的均值作为基准
    const float originalMean = std::accumulate(f0Curve.begin(), f0Curve.end(), 0.0f) / static_cast<float>(f0Curve.size());
    
    // 将曲线转换为围绕0的偏差值
    std::vector<float> centeredCurve = f0Curve;
    for (auto& val : centeredCurve) {
        val -= originalMean;
    }
    
    // 对中心化的曲线应用一阶高通滤波
    std::vector<float> result = centeredCurve;
    const size_t n = centeredCurve.size();
    
    // 使用一阶高通滤波器，保留原始起点值
    // 高通滤波器形式: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
    const float alpha = 1.0f - cutoffRatio;
    
    // 从第二点开始应用滤波器
    for (size_t i = 1; i < n; ++i) {
        // 应用一阶高通滤波
        result[i] = alpha * (result[i-1] + centeredCurve[i] - centeredCurve[i-1]);
    }
    
    // 将基准均值加回去
    for (auto& val : result) {
        val += originalMean;
    }
    
    // 如果需要，可以对结果进行后处理以改善右端点对齐
    if (cutoffRatio > 0.5f) {
        // 对最后几个点进行轻微平滑以避免右端点跳跃
        const size_t smoothLength = std::min(static_cast<size_t>(n / 10), static_cast<size_t>(10));
        if (smoothLength > 1 && n > smoothLength * 2) {
            // 使用线性插值平滑最后几个点
            const float target = std::accumulate(result.end() - smoothLength, result.end(), 0.0f) / static_cast<float>(smoothLength);
            for (size_t i = n - smoothLength; i < n; ++i) {
                const float t = static_cast<float>(i - (n - smoothLength)) / static_cast<float>(smoothLength);
                result[i] = result[i] * (1.0f - t) + target * t;
            }
        }
    }
    
    return result;
}

} // namespace PitchToolOperations
