#pragma once

#include "UndoableAction.h"
#include "F0FrameEdit.h"
#include <vector>
#include <functional>
#include <limits>

/**
 * Represents a pitch offset change for a note.
 */
struct PitchOffsetEdit
{
    int noteIndex = -1;
    float oldOffset = 0.0f;
    float newOffset = 0.0f;
};

/**
 * Action for changing multiple F0 values (hand-drawing).
 */
class F0EditAction : public UndoableAction
{
public:
    F0EditAction(std::vector<float>* f0Array,
                 std::vector<float>* deltaPitchArray,
                 std::vector<bool>* voicedMask,
                 std::vector<F0FrameEdit> edits,
                 std::vector<PitchOffsetEdit> pitchOffsetEdits = {},
                 std::function<void(int, int)> onF0Changed = nullptr)
        : f0Array(f0Array), deltaPitchArray(deltaPitchArray), voicedMask(voicedMask),
          edits(std::move(edits)), pitchOffsetEdits(std::move(pitchOffsetEdits)),
          onF0Changed(onF0Changed) {}

    void undo() override
    {
        if (!f0Array) return;
        int minIdx = std::numeric_limits<int>::max();
        int maxIdx = std::numeric_limits<int>::min();
        for (const auto& e : edits)
        {
            if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size())) {
                (*f0Array)[e.idx] = e.oldF0;
                minIdx = std::min(minIdx, e.idx);
                maxIdx = std::max(maxIdx, e.idx);
            }
            if (deltaPitchArray && e.idx >= 0 && e.idx < static_cast<int>(deltaPitchArray->size()))
                (*deltaPitchArray)[e.idx] = e.oldDelta;
            if (voicedMask && e.idx >= 0 && e.idx < static_cast<int>(voicedMask->size()))
                (*voicedMask)[e.idx] = e.oldVoiced;
        }
        
        // Restore pitch offsets to old values
        if (onPitchOffsetChanged)
        {
            for (const auto& pe : pitchOffsetEdits)
            {
                onPitchOffsetChanged(pe.noteIndex, pe.oldOffset);
            }
        }
        
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    void redo() override
    {
        if (!f0Array) return;
        int minIdx = std::numeric_limits<int>::max();
        int maxIdx = std::numeric_limits<int>::min();
        for (const auto& e : edits)
        {
            if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size())) {
                (*f0Array)[e.idx] = e.newF0;
                minIdx = std::min(minIdx, e.idx);
                maxIdx = std::max(maxIdx, e.idx);
            }
            if (deltaPitchArray && e.idx >= 0 && e.idx < static_cast<int>(deltaPitchArray->size()))
                (*deltaPitchArray)[e.idx] = e.newDelta;
            if (voicedMask && e.idx >= 0 && e.idx < static_cast<int>(voicedMask->size()))
                (*voicedMask)[e.idx] = e.newVoiced;
        }
        
        // Restore pitch offsets to new values
        if (onPitchOffsetChanged)
        {
            for (const auto& pe : pitchOffsetEdits)
            {
                onPitchOffsetChanged(pe.noteIndex, pe.newOffset);
            }
        }
        
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    juce::String getName() const override { return "Edit Pitch Curve"; }
    
    void setOnPitchOffsetChanged(std::function<void(int, float)> callback)
    {
        onPitchOffsetChanged = std::move(callback);
    }

private:
    std::vector<float>* f0Array;
    std::vector<float>* deltaPitchArray;
    std::vector<bool>* voicedMask;
    std::vector<F0FrameEdit> edits;
    std::vector<PitchOffsetEdit> pitchOffsetEdits;
    std::function<void(int, int)> onF0Changed;
    std::function<void(int, float)> onPitchOffsetChanged;
};
