#include "PitchCurveProcessor.h"
#include "BasePitchCurve.h"
#include "CurveResampler.h"
#include "PitchToolOperations.h"
#include "../Utils/Constants.h"
#include <algorithm>
#include <cmath>

namespace
{
    inline float safeFreqToMidi(float freq)
    {
        if (freq <= 0.0f)
            return 0.0f;
        return freqToMidi(freq);
    }

    void ensureSizes(AudioData& audioData, int totalFrames)
    {
        if (totalFrames <= 0)
            return;

        if (audioData.basePitch.size() != static_cast<size_t>(totalFrames))
            audioData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        if (audioData.deltaPitch.size() != static_cast<size_t>(totalFrames))
            audioData.deltaPitch.assign(static_cast<size_t>(totalFrames), 0.0f);
    }

    PitchToolOperations::AdjacentNoteContext buildAdjacentContext(
        const std::vector<Note>& allNotes, const Note& targetNote)
    {
        PitchToolOperations::AdjacentNoteContext ctx;

        const Note* prevNote = nullptr;
        int prevNoteEndFrame = -1;
        for (const auto& candidate : allNotes) {
            if (&candidate == &targetNote || candidate.isRest())
                continue;
            const int candidateEnd = candidate.getEndFrame();
            if (candidateEnd <= targetNote.getStartFrame() && candidateEnd > prevNoteEndFrame) {
                prevNote = &candidate;
                prevNoteEndFrame = candidateEnd;
            }
        }

        const Note* nextNote = nullptr;
        int nextNoteStartFrame = std::numeric_limits<int>::max();
        for (const auto& candidate : allNotes) {
            if (&candidate == &targetNote || candidate.isRest())
                continue;
            const int candidateStart = candidate.getStartFrame();
            if (candidateStart >= targetNote.getEndFrame() && candidateStart < nextNoteStartFrame) {
                nextNote = &candidate;
                nextNoteStartFrame = candidateStart;
            }
        }

        if (prevNote) {
            const auto& prevDelta = prevNote->hasOriginalDeltaPitch()
                ? prevNote->getOriginalDeltaPitch()
                : prevNote->getDeltaPitch();
            if (!prevDelta.empty()) {
                ctx.hasLeft = true;
                ctx.leftBoundaryDelta = prevDelta.back();
            }
        }
        if (nextNote) {
            const auto& nextDelta = nextNote->hasOriginalDeltaPitch()
                ? nextNote->getOriginalDeltaPitch()
                : nextNote->getDeltaPitch();
            if (!nextDelta.empty()) {
                ctx.hasRight = true;
                ctx.rightBoundaryDelta = nextDelta.front();
            }
        }

        return ctx;
    }

    std::vector<BasePitchCurve::NoteSegment> collectNoteSegments(const std::vector<Note>& notes)
    {
        std::vector<BasePitchCurve::NoteSegment> segments;
        segments.reserve(notes.size());

        for (const auto& note : notes)
        {
            if (note.isRest())
                continue;

            BasePitchCurve::NoteSegment seg;
            seg.startFrame = note.getStartFrame();
            seg.endFrame = note.getEndFrame();
            // Base pitch already includes per-note offset
            seg.midiNote = note.getMidiNote() + note.getPitchOffset()
                         - (note.getTiltLeft() + note.getTiltRight()) / 2.0f;
            segments.push_back(seg);
        }

        // Ensure segments are sorted by start frame for stable generation
        std::sort(segments.begin(), segments.end(),
                  [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
        return segments;
    }
} // namespace

namespace PitchCurveProcessor
{
    std::vector<float> interpolateWithUvMask(const std::vector<float>& pitchHz,
                                             const std::vector<bool>& uvMask)
    {
        if (pitchHz.empty())
            return {};

        const int n = static_cast<int>(pitchHz.size());
        std::vector<float> dense(pitchHz);

        auto isVoicedFrame = [&](int i) -> bool {
            if (i < 0 || i >= n)
                return false;

            const bool hasPitch = pitchHz[static_cast<size_t>(i)] > 0.0f;
            if (!hasPitch)
                return false;

            // If uvMask is provided, respect it (out-of-range treated as unvoiced).
            // If uvMask is missing, fall back to pitch presence.
            if (!uvMask.empty())
                return i < static_cast<int>(uvMask.size()) && uvMask[static_cast<size_t>(i)];
            return true;
        };

        int nextVoiced = -1;
        auto findNext = [&](int idx) -> int {
            for (int i = idx; i < n; ++i)
            {
                if (isVoicedFrame(i))
                    return i;
            }
            return -1;
        };

        int lastVoiced = -1;
        nextVoiced = findNext(0);

        for (int i = 0; i < n; ++i)
        {
            const bool voiced = isVoicedFrame(i);
            if (voiced)
            {
                lastVoiced = i;
                if (i == nextVoiced)
                    nextVoiced = findNext(i + 1);
                continue;
            }

            // Update next voiced lazily
            if (nextVoiced != -1 && nextVoiced < i)
                nextVoiced = findNext(i + 1);

            float prevVal = (lastVoiced >= 0) ? dense[static_cast<size_t>(lastVoiced)] : 0.0f;
            float nextVal = (nextVoiced >= 0) ? dense[static_cast<size_t>(nextVoiced)] : 0.0f;

            if (prevVal <= 0.0f && nextVal <= 0.0f)
            {
                dense[static_cast<size_t>(i)] = 0.0f;
                continue;
            }

            if (prevVal <= 0.0f)
            {
                dense[static_cast<size_t>(i)] = nextVal;
                continue;
            }
            if (nextVal <= 0.0f)
            {
                dense[static_cast<size_t>(i)] = prevVal;
                continue;
            }

            const float t = (nextVoiced > i) ? static_cast<float>(i - lastVoiced) /
                                               static_cast<float>(nextVoiced - lastVoiced)
                                             : 0.0f;
            const float logA = std::log(prevVal);
            const float logB = std::log(nextVal);
            dense[static_cast<size_t>(i)] = std::exp(logA * (1.0f - t) + logB * t);
        }

        return dense;
    }

    void rebuildCurvesFromSource(Project& project,
                                 const std::vector<float>& sourcePitchHz)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = static_cast<int>(sourcePitchHz.size());
        ensureSizes(audioData, totalFrames);

        // Ensure delta is built from a dense F0 source that includes UV
        // head/tail fill (same behavior expectation as F0 interpolation).
        std::vector<float> denseSource = sourcePitchHz;
        if (!audioData.voicedMask.empty() &&
            audioData.voicedMask.size() == sourcePitchHz.size())
        {
            denseSource = interpolateWithUvMask(sourcePitchHz, audioData.voicedMask);
        }

        auto segments = collectNoteSegments(project.getNotes());
        if (!segments.empty())
        {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }

        if (audioData.basePitch.size() != static_cast<size_t>(totalFrames))
        {
            // Fallback: derive base from source pitch directly
            audioData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
            for (int i = 0; i < totalFrames; ++i)
                audioData.basePitch[static_cast<size_t>(i)] = safeFreqToMidi(denseSource[i]);
        }

        // Dense delta: midi(source) - base
        audioData.deltaPitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        for (int i = 0; i < totalFrames; ++i)
        {
            const float base = audioData.basePitch[static_cast<size_t>(i)];
            const float midi = safeFreqToMidi(denseSource[i]);
            audioData.deltaPitch[static_cast<size_t>(i)] = midi - base;
        }

        // Initialize originalDeltaPitch in each note from computed deltaPitch
        // This preserves the pristine pitch curve for non-destructive transformations
        for (auto& note : project.getNotes())
        {
            if (note.isRest()) continue;

            const int startFrame = note.getStartFrame();
            const int endFrame = note.getEndFrame();
            const int numFrames = endFrame - startFrame;

            if (numFrames <= 0) continue;

            std::vector<float> origDelta(static_cast<size_t>(numFrames));
            for (int i = 0; i < numFrames; ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    origDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
            }
            note.setOriginalDeltaPitch(std::move(origDelta));
        }

        // Cache base F0 (Hz) for backwards compatibility
        audioData.baseF0.resize(static_cast<size_t>(totalFrames));
        for (int i = 0; i < totalFrames; ++i)
            audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);

        composeF0InPlace(project, /*applyUvMask=*/false);
    }

    void rebuildBaseFromNotes(Project& project)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = audioData.getNumFrames();
        ensureSizes(audioData, totalFrames);

        auto segments = collectNoteSegments(project.getNotes());
        if (!segments.empty())
        {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }

        if (audioData.basePitch.size() != static_cast<size_t>(totalFrames))
        {
            audioData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        }

        // CRITICAL: Rebuild deltaPitch from Note objects NON-DESTRUCTIVELY
        // Clear global deltaPitch first
        audioData.deltaPitch.assign(static_cast<size_t>(totalFrames), 0.0f);

        for (auto& note : project.getNotes())
        {
            if (note.isRest()) continue;

            const int startFrame = note.getStartFrame();
            const int endFrame = note.getEndFrame();
            const int numFrames = endFrame - startFrame;

            if (numFrames <= 0) continue;

            const auto pristineDelta = note.getOriginalDeltaPitch();
            if (pristineDelta.empty()) continue;

            const auto context = buildAdjacentContext(project.getNotes(), note);

            // Apply transformations in the correct order
            const auto transformedDelta = applyAllTransformations(
                pristineDelta,
                note.getTiltLeft(),
                note.getTiltRight(),
                note.getVarianceScale(),
                note.getSmoothLeftFrames(),
                note.getSmoothRightFrames(),
                note.getHighPassCutoff(),
                context
            );

            for (int i = 0; i < numFrames; ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    audioData.deltaPitch[static_cast<size_t>(globalIdx)] = transformedDelta[static_cast<size_t>(i)];
            }
        }

        composeF0InPlace(project, /*applyUvMask=*/true);
    }

    void composeF0InPlace(Project& project, bool applyUvMask, float globalPitchOffset)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = audioData.getNumFrames();

        audioData.f0.assign(static_cast<size_t>(totalFrames), 0.0f);
        for (int i = 0; i < totalFrames; ++i)
        {
            const float base = audioData.basePitch[static_cast<size_t>(i)];
            const float delta = audioData.deltaPitch[static_cast<size_t>(i)];
            audioData.f0[static_cast<size_t>(i)] = midiToFreq(base + delta + globalPitchOffset);
        }

        if (applyUvMask && !audioData.voicedMask.empty() &&
            audioData.voicedMask.size() == audioData.f0.size())
        {
            for (int i = 0; i < totalFrames; ++i)
            {
                if (!audioData.voicedMask[static_cast<size_t>(i)])
                    audioData.f0[static_cast<size_t>(i)] = 0.0f;
            }
        }
    }

    std::vector<float> composeF0(const Project& project, bool applyUvMask, float globalPitchOffset)
    {
        std::vector<float> result;
        const int totalFrames = project.getAudioData().getNumFrames();
        
        result.assign(static_cast<size_t>(totalFrames), 0.0f);
        for (int i = 0; i < totalFrames; ++i)
        {
            const float base = project.getAudioData().basePitch[static_cast<size_t>(i)];
            const float delta = project.getAudioData().deltaPitch[static_cast<size_t>(i)];
            result[static_cast<size_t>(i)] = midiToFreq(base + delta + globalPitchOffset);
        }

        if (applyUvMask && !project.getAudioData().voicedMask.empty() &&
            project.getAudioData().voicedMask.size() == result.size())
        {
            for (int i = 0; i < totalFrames; ++i)
            {
                if (!project.getAudioData().voicedMask[static_cast<size_t>(i)])
                    result[static_cast<size_t>(i)] = 0.0f;
            }
        }

        return result;
    }

    void rebuildDeltaForNotes(Project& project, const std::vector<Note*>& affectedNotes)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = audioData.getNumFrames();

        for (auto* notePtr : affectedNotes)
        {
            if (!notePtr || notePtr->isRest()) continue;

            const auto& note = *notePtr;
            const int startFrame = note.getStartFrame();
            const int endFrame = note.getEndFrame();
            const int numFrames = endFrame - startFrame;

            if (numFrames <= 0) continue;

            const auto pristineDelta = note.getOriginalDeltaPitch();
            if (pristineDelta.empty()) continue;

            const auto context = buildAdjacentContext(project.getNotes(), note);

            // Apply transformations in the correct order
            const auto transformedDelta = applyAllTransformations(
                pristineDelta,
                note.getTiltLeft(),
                note.getTiltRight(),
                note.getVarianceScale(),
                note.getSmoothLeftFrames(),
                note.getSmoothRightFrames(),
                note.getHighPassCutoff(),
                context
            );

            for (int i = 0; i < numFrames; ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    audioData.deltaPitch[static_cast<size_t>(globalIdx)] = transformedDelta[static_cast<size_t>(i)];
            }
        }

        composeF0InPlace(project, /*applyUvMask=*/true);
    }

    void rebuildBaseFromNotesForDrag(Project& project, const std::vector<Note*>& affectedNotes)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = audioData.getNumFrames();
        ensureSizes(audioData, totalFrames);

        // Regenerate base pitch from ALL notes to account for pitch changes
        auto segments = collectNoteSegments(project.getNotes());
        if (!segments.empty())
        {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }

        if (audioData.basePitch.size() != static_cast<size_t>(totalFrames))
        {
            audioData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        }

        // Process delta pitch only for affected notes
        for (auto* notePtr : affectedNotes)
        {
            if (!notePtr || notePtr->isRest()) continue;

            const auto& note = *notePtr;
            const int startFrame = note.getStartFrame();
            const int endFrame = note.getEndFrame();
            const int numFrames = endFrame - startFrame;

            if (numFrames <= 0) continue;

            const auto pristineDelta = note.getOriginalDeltaPitch();
            if (pristineDelta.empty()) continue;

            const auto context = buildAdjacentContext(project.getNotes(), note);

            // Apply transformations in the correct order
            const auto transformedDelta = applyAllTransformations(
                pristineDelta,
                note.getTiltLeft(),
                note.getTiltRight(),
                note.getVarianceScale(),
                note.getSmoothLeftFrames(),
                note.getSmoothRightFrames(),
                note.getHighPassCutoff(),
                context
            );

            for (int i = 0; i < numFrames; ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    audioData.deltaPitch[static_cast<size_t>(globalIdx)] = transformedDelta[static_cast<size_t>(i)];
            }
        }

        // Determine the affected frame range
        int minFrame = totalFrames;
        int maxFrame = 0;
        for (auto* notePtr : affectedNotes)
        {
            if (notePtr)
            {
                minFrame = std::min(minFrame, notePtr->getStartFrame());
                maxFrame = std::max(maxFrame, notePtr->getEndFrame());
            }
        }

        // Only recompose f0 for the affected range
        for (int i = std::max(0, minFrame); i < std::min(totalFrames, maxFrame); ++i)
        {
            const float base = audioData.basePitch[static_cast<size_t>(i)];
            const float delta = audioData.deltaPitch[static_cast<size_t>(i)];
            audioData.f0[static_cast<size_t>(i)] = midiToFreq(base + delta);
            
            // Apply uv mask if applicable
            if (!audioData.voicedMask.empty() && 
                static_cast<int>(audioData.voicedMask.size()) > i &&
                !audioData.voicedMask[static_cast<size_t>(i)])
            {
                audioData.f0[static_cast<size_t>(i)] = 0.0f;
            }
        }
    }
} // namespace PitchCurveProcessor
