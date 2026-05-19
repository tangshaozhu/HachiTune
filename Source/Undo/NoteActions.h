#pragma once

#include "UndoableAction.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include "../Utils/BasePitchCurve.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/Constants.h"
#include <vector>
#include <functional>

/**
 * Generic action for changing a single float property on a Note.
 * Uses a member function pointer to call the appropriate setter.
 */
class NoteFloatPropertyAction : public UndoableAction
{
public:
    using Setter = void (Note::*)(float);

    NoteFloatPropertyAction(Note *note, float oldVal, float newVal,
                            Setter setter, juce::String actionName,
                            std::function<void(Note *)> onNoteChanged = nullptr)
        : note(note), oldVal(oldVal), newVal(newVal),
          setter(setter), actionName(std::move(actionName)),
          onNoteChanged(onNoteChanged) {}

    void undo() override
    {
        if (!note)
            return;
        (note->*setter)(oldVal);
        note->markDirty();
        if (onNoteChanged)
            onNoteChanged(note);
    }
    void redo() override
    {
        if (!note)
            return;
        (note->*setter)(newVal);
        note->markDirty();
        if (onNoteChanged)
            onNoteChanged(note);
    }
    juce::String getName() const override { return actionName; }

private:
    Note *note;
    float oldVal;
    float newVal;
    Setter setter;
    juce::String actionName;
    std::function<void(Note *)> onNoteChanged;
};

/**
 * Action for adjusting boundary between two adjacent notes.
 * Follows the EXACT same pattern as NoteMergeAction: remove old notes, add new notes.
 */
class NoteBoundaryAdjustAction : public UndoableAction
{
public:
    NoteBoundaryAdjustAction(Project* project, 
                           const Note &originalLeft, const Note &originalRight,
                           const Note &adjustedLeft, const Note &adjustedRight,
                           std::function<void()> onChanged = nullptr)
        : project(project),
          originalLeftNote(originalLeft), originalRightNote(originalRight),
          adjustedLeftNote(adjustedLeft), adjustedRightNote(adjustedRight),
          onChanged(onChanged) {}

    void undo() override
    {
        if (!project) return;
        
        // Remove the adjusted notes by finding them via startFrame (same as NoteMergeAction)
        auto& notes = project->getNotes();
        
        bool leftRemoved = false;
        bool rightRemoved = false;
        
        for (auto it = notes.begin(); it != notes.end(); ) {
            if (!leftRemoved && it->getStartFrame() == adjustedLeftNote.getStartFrame()) {
                it = notes.erase(it);
                leftRemoved = true;
            } else if (!rightRemoved && it->getStartFrame() == adjustedRightNote.getStartFrame()) {
                it = notes.erase(it);
                rightRemoved = true;
            } else {
                ++it;
            }
            
            if (leftRemoved && rightRemoved) break;
        }
        
        // Only restore original notes if both adjusted notes were successfully removed
        if (leftRemoved && rightRemoved) {
            project->addNote(originalLeftNote);
            project->addNote(originalRightNote);
        } else {
            return;
        }
        
        // CRITICAL: Rebuild basePitch and deltaPitch (EXACT same as NoteMergeAction)
        rebuildPitchData();
        
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        if (!project) return;
        
        // Remove the original notes by finding them via startFrame (same as NoteMergeAction)
        auto& notes = project->getNotes();
        
        bool leftRemoved = false;
        bool rightRemoved = false;
        
        for (auto it = notes.begin(); it != notes.end(); ) {
            if (!leftRemoved && it->getStartFrame() == originalLeftNote.getStartFrame()) {
                it = notes.erase(it);
                leftRemoved = true;
            } else if (!rightRemoved && it->getStartFrame() == originalRightNote.getStartFrame()) {
                it = notes.erase(it);
                rightRemoved = true;
            } else {
                ++it;
            }
            
            if (leftRemoved && rightRemoved) break;
        }
        
        // Only add adjusted notes if both original notes were successfully removed
        if (leftRemoved && rightRemoved) {
            project->addNote(adjustedLeftNote);
            project->addNote(adjustedRightNote);
        } else {
            return;
        }
        
        // CRITICAL: Rebuild basePitch and deltaPitch (EXACT same as NoteMergeAction)
        rebuildPitchData();
        
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override { return "Adjust Note Boundary"; }

private:
    void rebuildPitchData() {
        if (!project) return;
        
        auto& audioData = project->getAudioData();
        const int totalFrames = audioData.getNumFrames();
        
        // Step 1: Build note segments for base pitch generation
        std::vector<BasePitchCurve::NoteSegment> segments;
        const auto& notes = project->getNotes();
        segments.reserve(notes.size());
        for (const auto& n : notes) {
            if (n.isRest()) continue;
            
            BasePitchCurve::NoteSegment seg;
            seg.startFrame = n.getStartFrame();
            seg.endFrame = n.getEndFrame();
            seg.midiNote = n.getMidiNote() + n.getPitchOffset()
                         - (n.getTiltLeft() + n.getTiltRight()) / 2.0f;
            segments.push_back(seg);
        }
        
        std::sort(segments.begin(), segments.end(),
                  [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
        
        // Step 2: Regenerate basePitch from notes
        if (!segments.empty()) {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }
        
        // Step 3: Recalculate deltaPitch from f0 and new basePitch (EXACT same as NoteMergeAction)
        audioData.deltaPitch.resize(static_cast<size_t>(totalFrames));
        for (int i = 0; i < totalFrames; ++i) {
            const float baseMidi = audioData.basePitch[static_cast<size_t>(i)];
            if (i < static_cast<int>(audioData.voicedMask.size()) && 
                audioData.voicedMask[static_cast<size_t>(i)] && 
                audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                const float f0Midi = freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                audioData.deltaPitch[static_cast<size_t>(i)] = f0Midi - baseMidi;
            } else {
                audioData.deltaPitch[static_cast<size_t>(i)] = 0.0f;
            }
        }
        
        // Step 4: Update cached baseF0
        audioData.baseF0.resize(static_cast<size_t>(totalFrames));
        for (int i = 0; i < totalFrames; ++i) {
            audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);
        }
        
        // Step 5: Sync global deltaPitch back to each note's deltaPitch vectors
        for (auto& n : project->getNotes()) {
            if (n.isRest()) continue;
            
            const int startFrame = n.getStartFrame();
            const int endFrame = n.getEndFrame();
            const int numFrames = endFrame - startFrame;
            
            if (numFrames <= 0) continue;
            
            std::vector<float> noteDelta(static_cast<size_t>(numFrames));
            for (int i = 0; i < numFrames; ++i) {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames) {
                    noteDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
                }
            }
            
            n.setOriginalDeltaPitch(noteDelta);
            n.setDeltaPitch(noteDelta);
        }
        
        // CRITICAL: Recompose F0 from basePitch and deltaPitch
        PitchCurveProcessor::composeF0InPlace(*project, /*applyUvMask=*/true);
    }
    
    Project* project;
    Note originalLeftNote;
    Note originalRightNote;
    Note adjustedLeftNote;
    Note adjustedRightNote;
    std::function<void()> onChanged;
};

/**
 * Generic action for changing a single float property on multiple Notes.
 * Uses a member function pointer to call the appropriate setter.
 */
class MultiNoteFloatPropertyAction : public UndoableAction
{
public:
    using Setter = void (Note::*)(float);

    MultiNoteFloatPropertyAction(const std::vector<Note *> &notes,
                                 const std::vector<float> &oldVals,
                                 const std::vector<float> &newVals,
                                 Setter setter, juce::String actionName,
                                 std::function<void()> onChanged = nullptr)
        : notes(notes), oldVals(oldVals), newVals(newVals),
          setter(setter), actionName(std::move(actionName)),
          onChanged(onChanged) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldVals.size(); ++i)
        {
            if (notes[i])
            {
                (notes[i]->*setter)(oldVals[i]);
                notes[i]->markDirty();
            }
        }
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size() && i < newVals.size(); ++i)
        {
            if (notes[i])
            {
                (notes[i]->*setter)(newVals[i]);
                notes[i]->markDirty();
            }
        }
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override { return actionName; }

private:
    std::vector<Note *> notes;
    std::vector<float> oldVals;
    std::vector<float> newVals;
    Setter setter;
    juce::String actionName;
    std::function<void()> onChanged;
};

/**
 * Action for resetting tilt values on multiple notes.
 * Used for double-click on TiltLeft/TiltRight handles to reset to 0.
 */
class TiltResetAction : public UndoableAction
{
public:
    enum class TiltSide
    {
        Left,
        Right
    };

    TiltResetAction(const std::vector<Note *> &notes,
                    TiltSide side,
                    const std::vector<float> &oldTilts,
                    const std::vector<float> &oldMidiNotes,
                    std::function<void()> onChanged = nullptr)
        : notes(notes), side(side), oldTilts(oldTilts),
          oldMidiNotes(oldMidiNotes), onChanged(onChanged) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldTilts.size(); ++i)
        {
            if (notes[i])
            {
                if (side == TiltSide::Left)
                    notes[i]->setTiltLeft(oldTilts[i]);
                else
                    notes[i]->setTiltRight(oldTilts[i]);

                if (i < oldMidiNotes.size())
                    notes[i]->setMidiNote(oldMidiNotes[i]);

                notes[i]->markDirty();
                notes[i]->markSynthDirty();
            }
        }
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (notes[i])
            {
                if (side == TiltSide::Left)
                    notes[i]->setTiltLeft(0.0f);
                else
                    notes[i]->setTiltRight(0.0f);

                const float newTiltMean = (notes[i]->getTiltLeft() + notes[i]->getTiltRight()) / 2.0f;
                if (i < oldMidiNotes.size())
                {
                    const float oldTiltLeft = (side == TiltSide::Left) ? oldTilts[i] : notes[i]->getTiltLeft();
                    const float oldTiltRight = (side == TiltSide::Right) ? oldTilts[i] : notes[i]->getTiltRight();
                    const float oldTiltMean = (oldTiltLeft + oldTiltRight) / 2.0f;
                    const float baseline = oldMidiNotes[i] - oldTiltMean;
                    notes[i]->setMidiNote(baseline + newTiltMean);
                }

                notes[i]->markDirty();
                notes[i]->markSynthDirty();
            }
        }
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override
    {
        return side == TiltSide::Left ? "Reset Tilt Left" : "Reset Tilt Right";
    }

private:
    std::vector<Note *> notes;
    TiltSide side;
    std::vector<float> oldTilts;
    std::vector<float> oldMidiNotes;
    std::function<void()> onChanged;
};

/**
 * Action for snapping a note to the nearest semitone (double-click).
 * Combines midiNote and pitchOffset into a rounded integer MIDI value.
 */
class NoteSnapToSemitoneAction : public UndoableAction
{
public:
    NoteSnapToSemitoneAction(Note *note,
                             float oldMidi, float oldOffset,
                             float newMidi,
                             std::function<void(Note *)> onNoteChanged = nullptr)
        : note(note), oldMidi(oldMidi), oldOffset(oldOffset),
          newMidi(newMidi), onNoteChanged(onNoteChanged) {}

    void undo() override
    {
        if (note)
        {
            note->setMidiNote(oldMidi);
            note->setPitchOffset(oldOffset);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNoteChanged && note)
            onNoteChanged(note);
    }

    void redo() override
    {
        if (note)
        {
            note->setMidiNote(newMidi);
            note->setPitchOffset(0.0f);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNoteChanged && note)
            onNoteChanged(note);
    }

    juce::String getName() const override { return "Snap to Semitone"; }

private:
    Note *note;
    float oldMidi;
    float oldOffset;
    float newMidi;
    std::function<void(Note *)> onNoteChanged;
};

/**
 * Action for snapping multiple notes to the nearest semitone.
 */
class MultiNoteSnapToSemitoneAction : public UndoableAction
{
public:
    MultiNoteSnapToSemitoneAction(const std::vector<Note *> &notes,
                                  std::vector<float> oldMidis,
                                  std::vector<float> oldOffsets,
                                  std::vector<float> newMidis,
                                  std::function<void(const std::vector<Note *> &)> onNotesChanged = nullptr)
        : notes(notes),
          oldMidis(std::move(oldMidis)),
          oldOffsets(std::move(oldOffsets)),
          newMidis(std::move(newMidis)),
          onNotesChanged(onNotesChanged) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            auto *note = notes[i];
            if (!note)
                continue;
            note->setMidiNote(oldMidis[i]);
            note->setPitchOffset(oldOffsets[i]);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            auto *note = notes[i];
            if (!note)
                continue;
            note->setMidiNote(newMidis[i]);
            note->setPitchOffset(0.0f);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    juce::String getName() const override { return "Snap Notes to Semitone"; }

private:
    std::vector<Note *> notes;
    std::vector<float> oldMidis;
    std::vector<float> oldOffsets;
    std::vector<float> newMidis;
    std::function<void(const std::vector<Note *> &)> onNotesChanged;
};

/**
 * Action for splitting a note into two.
 */
class NoteSplitAction : public UndoableAction
{
public:
    NoteSplitAction(Project *proj, const Note &original, const Note &firstPart, const Note &secondPart,
                    std::function<void()> onChanged = nullptr)
        : project(proj), originalNote(original), firstNote(firstPart), secondNote(secondPart),
          onChanged(onChanged) {}

    void undo() override
    {
        if (!project)
            return;
        project->removeNoteByStartFrame(secondNote.getStartFrame());
        for (auto &note : project->getNotes())
        {
            if (note.getStartFrame() == firstNote.getStartFrame())
            {
                note = originalNote;
                break;
            }
        }
        
        // CRITICAL: After restoring the original note, rebuild basePitch and deltaPitch
        // This ensures data consistency, same as NoteSplitter's approach
        {
            auto& audioData = project->getAudioData();
            const int totalFrames = audioData.getNumFrames();
            
            // Step 1: Build note segments for base pitch generation
            std::vector<BasePitchCurve::NoteSegment> segments;
            const auto& notes = project->getNotes();
            segments.reserve(notes.size());
            for (const auto& n : notes) {
                if (n.isRest()) continue;
                
                BasePitchCurve::NoteSegment seg;
                seg.startFrame = n.getStartFrame();
                seg.endFrame = n.getEndFrame();
                seg.midiNote = n.getMidiNote() + n.getPitchOffset()
                             - (n.getTiltLeft() + n.getTiltRight()) / 2.0f;
                segments.push_back(seg);
            }
            
            std::sort(segments.begin(), segments.end(),
                      [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
            
            // Step 2: Regenerate basePitch from notes
            if (!segments.empty()) {
                audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
            }
            
            // Step 3: Rebuild deltaPitch from Note objects (same as rebuildBaseFromNotes)
            // Clear global deltaPitch first
            audioData.deltaPitch.assign(static_cast<size_t>(totalFrames), 0.0f);
            
            for (auto& n : project->getNotes()) {
                if (n.isRest()) continue;
                
                const int startFrame = n.getStartFrame();
                const int endFrame = n.getEndFrame();
                const int numFrames = endFrame - startFrame;
                
                if (numFrames <= 0) continue;
                
                const auto pristineDelta = n.getOriginalDeltaPitch();
                if (pristineDelta.empty()) continue;
                
                // For now, just copy the original delta directly without transformations
                // (tilt/variance/smoothing are applied in rebuildBaseFromNotes but we keep it simple here)
                for (int i = 0; i < numFrames && i < static_cast<int>(pristineDelta.size()); ++i) {
                    const int globalIdx = startFrame + i;
                    if (globalIdx >= 0 && globalIdx < totalFrames) {
                        audioData.deltaPitch[static_cast<size_t>(globalIdx)] = pristineDelta[static_cast<size_t>(i)];
                    }
                }
            }
            
            // Step 4: Update cached baseF0
            audioData.baseF0.resize(static_cast<size_t>(totalFrames));
            for (int i = 0; i < totalFrames; ++i) {
                audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);
            }
            
            // Step 5: Sync global deltaPitch back to each note's deltaPitch vectors
            for (auto& n : project->getNotes()) {
                if (n.isRest()) continue;
                
                const int startFrame = n.getStartFrame();
                const int endFrame = n.getEndFrame();
                const int numFrames = endFrame - startFrame;
                
                if (numFrames <= 0) continue;
                
                std::vector<float> noteDelta(static_cast<size_t>(numFrames));
                for (int i = 0; i < numFrames; ++i) {
                    const int globalIdx = startFrame + i;
                    if (globalIdx >= 0 && globalIdx < totalFrames) {
                        noteDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
                    }
                }
                
                n.setOriginalDeltaPitch(noteDelta);
                n.setDeltaPitch(noteDelta);
            }
            // CRITICAL: Recompose F0 from basePitch and deltaPitch (apply UV mask to hide non-voiced regions)
            PitchCurveProcessor::composeF0InPlace(*project, /*applyUvMask=*/true);
        }
        
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        if (!project)
            return;
        
        // Restore first note from snapshot
        for (auto &note : project->getNotes())
        {
            if (note.getStartFrame() == originalNote.getStartFrame())
            {
                note = firstNote;
                break;
            }
        }
        
        // Add second note
        project->addNote(secondNote);
        
        // CRITICAL: After splitting, recalculate midiNote based on average F0 for both notes
        // This matches the behavior in NoteSplitter::splitNoteAtFrame
        {
            auto& audioData = project->getAudioData();
            
            // For first note (left part) - find it by startFrame
            for (auto& n : project->getNotes()) {
                if (n.getStartFrame() == firstNote.getStartFrame()) {
                    const int startFrame = n.getStartFrame();
                    const int endFrame = n.getEndFrame();
                    
                    if (!audioData.f0.empty() && endFrame > startFrame) {
                        float sumF0Midi = 0.0f;
                        int validCount = 0;
                        
                        for (int i = startFrame; i < endFrame && i < static_cast<int>(audioData.f0.size()); ++i) {
                            // CRITICAL: Check voicedMask to avoid including non-voiced regions (F0=0) in average calculation
                            if (i < static_cast<int>(audioData.voicedMask.size()) && 
                                audioData.voicedMask[static_cast<size_t>(i)] && 
                                audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                                sumF0Midi += freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                                validCount++;
                            }
                        }
                        
                        if (validCount > 0) {
                            const float avgF0Midi = sumF0Midi / static_cast<float>(validCount);
                            n.setMidiNote(avgF0Midi);
                            n.setPitchOffset(0.0f);
                        }
                    }
                    break;
                }
            }
            
            // For second note (right part) - find it by startFrame
            for (auto& n : project->getNotes()) {
                if (n.getStartFrame() == secondNote.getStartFrame()) {
                    const int startFrame = n.getStartFrame();
                    const int endFrame = n.getEndFrame();
                    
                    if (!audioData.f0.empty() && endFrame > startFrame) {
                        float sumF0Midi = 0.0f;
                        int validCount = 0;
                        
                        for (int i = startFrame; i < endFrame && i < static_cast<int>(audioData.f0.size()); ++i) {
                            // Only count voiced frames with valid F0 (skip non-voiced regions where F0 may be 0 or interpolated)
                            if (i < static_cast<int>(audioData.voicedMask.size()) && 
                                audioData.voicedMask[i] && audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                                sumF0Midi += freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                                validCount++;
                            }
                        }
                        
                        if (validCount > 0) {
                            const float avgF0Midi = sumF0Midi / static_cast<float>(validCount);
                            n.setMidiNote(avgF0Midi);
                            n.setPitchOffset(0.0f);
                        }
                    }
                    break;
                }
            }
        }
        
        // CRITICAL: After splitting the note, rebuild basePitch and deltaPitch
        // This ensures data consistency, same as NoteSplitter's approach
        {
            auto& audioData = project->getAudioData();
            const int totalFrames = audioData.getNumFrames();
            
            // Step 1: Build note segments for base pitch generation
            std::vector<BasePitchCurve::NoteSegment> segments;
            const auto& notes = project->getNotes();
            segments.reserve(notes.size());
            for (const auto& n : notes) {
                if (n.isRest()) continue;
                
                BasePitchCurve::NoteSegment seg;
                seg.startFrame = n.getStartFrame();
                seg.endFrame = n.getEndFrame();
                seg.midiNote = n.getMidiNote() + n.getPitchOffset()
                             - (n.getTiltLeft() + n.getTiltRight()) / 2.0f;
                segments.push_back(seg);
            }
            
            std::sort(segments.begin(), segments.end(),
                      [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
            
            // Step 2: Regenerate basePitch from notes
            if (!segments.empty()) {
                audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
            }
            
            // Step 3: Recalculate deltaPitch from f0 and new basePitch (NOT from originalDeltaPitch)
            // This ensures consistency after midiNote adjustment
            audioData.deltaPitch.resize(static_cast<size_t>(totalFrames));
            for (int i = 0; i < totalFrames; ++i) {
                const float baseMidi = audioData.basePitch[static_cast<size_t>(i)];
                const float f0Midi = freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                audioData.deltaPitch[static_cast<size_t>(i)] = f0Midi - baseMidi;
            }
            
            // Step 4: Update cached baseF0
            audioData.baseF0.resize(static_cast<size_t>(totalFrames));
            for (int i = 0; i < totalFrames; ++i) {
                audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);
            }
            
            // Step 5: Sync global deltaPitch back to each note's deltaPitch vectors
            for (auto& n : project->getNotes()) {
                if (n.isRest()) continue;
                
                const int startFrame = n.getStartFrame();
                const int endFrame = n.getEndFrame();
                const int numFrames = endFrame - startFrame;
                
                if (numFrames <= 0) continue;
                
                std::vector<float> noteDelta(static_cast<size_t>(numFrames));
                for (int i = 0; i < numFrames; ++i) {
                    const int globalIdx = startFrame + i;
                    if (globalIdx >= 0 && globalIdx < totalFrames) {
                        noteDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
                    }
                }
                
                n.setOriginalDeltaPitch(noteDelta);
                n.setDeltaPitch(noteDelta);
            }
            
            // CRITICAL: Recompose F0 from basePitch and deltaPitch
            // This ensures f0 array is synchronized with the rebuilt data
            PitchCurveProcessor::composeF0InPlace(*project, /*applyUvMask=*/true);
        }
        
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override { return "Split Note"; }

private:
    Project *project;
    Note originalNote;
    Note firstNote;
    Note secondNote;
    std::function<void()> onChanged;
};

/**
 * Action for merging two adjacent notes into one.
 */
class NoteMergeAction : public UndoableAction
{
public:
    NoteMergeAction(Project *proj, const Note &leftNote, const Note &rightNote, const Note &mergedNote,
                    std::function<void()> onChanged = nullptr)
        : project(proj), leftNoteSnapshot(leftNote), rightNoteSnapshot(rightNote), mergedNoteSnapshot(mergedNote),
          onChanged(onChanged) {}

    void undo() override
    {
        if (!project)
            return;
        
        // CRITICAL: Verify the merged note still exists before removing
        // After multiple operations, the merged note may have been modified or deleted
        bool mergedNoteFound = false;
        for (auto it = project->getNotes().begin(); it != project->getNotes().end(); ++it) {
            if (it->getStartFrame() == mergedNoteSnapshot.getStartFrame() &&
                it->getEndFrame() == mergedNoteSnapshot.getEndFrame()) {
                project->getNotes().erase(it);
                mergedNoteFound = true;
                break;
            }
        }
        
        // Only restore original notes if we successfully removed the merged note
        if (mergedNoteFound) {
            project->addNote(leftNoteSnapshot);
            project->addNote(rightNoteSnapshot);
        } else {
            // Merged note not found or already modified - skip undo to avoid corruption
            return;
        }
        
        // CRITICAL: After restoring notes, rebuild basePitch and deltaPitch
        // Use the same approach as mergeNotes/redo to ensure consistency
        {
            auto& audioData = project->getAudioData();
            const int totalFrames = audioData.getNumFrames();
            
            // Step 1: Build note segments for base pitch generation
            std::vector<BasePitchCurve::NoteSegment> segments;
            const auto& notes = project->getNotes();
            segments.reserve(notes.size());
            for (const auto& n : notes) {
                if (n.isRest()) continue;
                
                BasePitchCurve::NoteSegment seg;
                seg.startFrame = n.getStartFrame();
                seg.endFrame = n.getEndFrame();
                seg.midiNote = n.getMidiNote() + n.getPitchOffset()
                             - (n.getTiltLeft() + n.getTiltRight()) / 2.0f;
                segments.push_back(seg);
            }
            
            std::sort(segments.begin(), segments.end(),
                      [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
            
            // Step 2: Regenerate basePitch from notes
            if (!segments.empty()) {
                audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
            }
            
            // Step 3: Recalculate deltaPitch from f0 and new basePitch (same as mergeNotes)
            audioData.deltaPitch.resize(static_cast<size_t>(totalFrames));
            for (int i = 0; i < totalFrames; ++i) {
                const float baseMidi = audioData.basePitch[static_cast<size_t>(i)];
                // CRITICAL: Skip non-voiced regions to avoid computing deltaPitch from F0=0
                if (i < static_cast<int>(audioData.voicedMask.size()) && 
                    audioData.voicedMask[static_cast<size_t>(i)] && 
                    audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                    const float f0Midi = freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                    audioData.deltaPitch[static_cast<size_t>(i)] = f0Midi - baseMidi;
                } else {
                    // For non-voiced regions, keep deltaPitch at 0 to avoid extreme negative values
                    audioData.deltaPitch[static_cast<size_t>(i)] = 0.0f;
                }
            }
            
            // Step 4: Update cached baseF0
            audioData.baseF0.resize(static_cast<size_t>(totalFrames));
            for (int i = 0; i < totalFrames; ++i) {
                audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);
            }
            
            // Step 5: Sync global deltaPitch back to each note's deltaPitch vectors
            for (auto& n : project->getNotes()) {
                if (n.isRest()) continue;
                
                const int startFrame = n.getStartFrame();
                const int endFrame = n.getEndFrame();
                const int numFrames = endFrame - startFrame;
                
                if (numFrames <= 0) continue;
                
                std::vector<float> noteDelta(static_cast<size_t>(numFrames));
                for (int i = 0; i < numFrames; ++i) {
                    const int globalIdx = startFrame + i;
                    if (globalIdx >= 0 && globalIdx < totalFrames) {
                        noteDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
                    }
                }
                
                n.setOriginalDeltaPitch(noteDelta);
                n.setDeltaPitch(noteDelta);
            }
            
            // CRITICAL: Recompose F0 from basePitch and deltaPitch (apply UV mask to hide non-voiced regions)
            PitchCurveProcessor::composeF0InPlace(*project, /*applyUvMask=*/true);
        }
        
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        if (!project)
            return;
        
        // CRITICAL: Verify both original notes still exist before removing
        // After multiple operations, the original notes may have been modified or deleted
        bool leftNoteFound = false;
        bool rightNoteFound = false;
        
        auto& notes = project->getNotes();
        
        // Find and remove left note
        for (auto it = notes.begin(); it != notes.end(); ++it) {
            if (it->getStartFrame() == leftNoteSnapshot.getStartFrame() &&
                it->getEndFrame() == leftNoteSnapshot.getEndFrame()) {
                notes.erase(it);
                leftNoteFound = true;
                break;
            }
        }
        
        // Find and remove right note (need to search again as iterator may be invalidated)
        for (auto it = notes.begin(); it != notes.end(); ++it) {
            if (it->getStartFrame() == rightNoteSnapshot.getStartFrame() &&
                it->getEndFrame() == rightNoteSnapshot.getEndFrame()) {
                notes.erase(it);
                rightNoteFound = true;
                break;
            }
        }
        
        // Only add merged note if both original notes were successfully removed
        if (leftNoteFound && rightNoteFound) {
            project->addNote(mergedNoteSnapshot);
        } else {
            // Original notes not found or already modified - skip redo to avoid corruption
            return;
        }
        
        // CRITICAL: After merging, recalculate midiNote based on average F0
        {
            auto& audioData = project->getAudioData();
            
            for (auto& n : project->getNotes()) {
                if (n.getStartFrame() == mergedNoteSnapshot.getStartFrame()) {
                    const int startFrame = n.getStartFrame();
                    const int endFrame = n.getEndFrame();
                    
                    if (!audioData.f0.empty() && endFrame > startFrame) {
                        float sumF0Midi = 0.0f;
                        int validCount = 0;
                        
                        for (int i = startFrame; i < endFrame && i < static_cast<int>(audioData.f0.size()); ++i) {
                            // CRITICAL: Check voicedMask to avoid including non-voiced regions (F0=0) in average calculation
                            if (i < static_cast<int>(audioData.voicedMask.size()) && 
                                audioData.voicedMask[static_cast<size_t>(i)] && 
                                audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                                sumF0Midi += freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                                validCount++;
                            }
                        }
                        
                        if (validCount > 0) {
                            const float avgF0Midi = sumF0Midi / static_cast<float>(validCount);
                            n.setMidiNote(avgF0Midi);
                            n.setPitchOffset(0.0f);
                        }
                    }
                }
            }
        }
        
        // CRITICAL: After merging, rebuild basePitch and deltaPitch (same as NoteSplitter::mergeNotes)
        {
            auto& audioData = project->getAudioData();
            const int totalFrames = audioData.getNumFrames();
            
            // Step 1: Build note segments for base pitch generation
            std::vector<BasePitchCurve::NoteSegment> segments;
            const auto& notes = project->getNotes();
            segments.reserve(notes.size());
            for (const auto& n : notes) {
                if (n.isRest()) continue;
                
                BasePitchCurve::NoteSegment seg;
                seg.startFrame = n.getStartFrame();
                seg.endFrame = n.getEndFrame();
                seg.midiNote = n.getMidiNote() + n.getPitchOffset()
                             - (n.getTiltLeft() + n.getTiltRight()) / 2.0f;
                segments.push_back(seg);
            }
            
            std::sort(segments.begin(), segments.end(),
                      [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
            
            // Step 2: Regenerate basePitch from notes
            if (!segments.empty()) {
                audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
            }
            
            // Step 3: Recalculate deltaPitch from f0 and new basePitch
            audioData.deltaPitch.resize(static_cast<size_t>(totalFrames));
            for (int i = 0; i < totalFrames; ++i) {
                const float baseMidi = audioData.basePitch[static_cast<size_t>(i)];
                // CRITICAL: Skip non-voiced regions to avoid computing deltaPitch from F0=0
                if (i < static_cast<int>(audioData.voicedMask.size()) && 
                    audioData.voicedMask[static_cast<size_t>(i)] && 
                    audioData.f0[static_cast<size_t>(i)] > 0.0f) {
                    const float f0Midi = freqToMidi(audioData.f0[static_cast<size_t>(i)]);
                    audioData.deltaPitch[static_cast<size_t>(i)] = f0Midi - baseMidi;
                } else {
                    // For non-voiced regions, keep deltaPitch at 0 to avoid extreme negative values
                    audioData.deltaPitch[static_cast<size_t>(i)] = 0.0f;
                }
            }
            
            // Step 4: Update cached baseF0
            audioData.baseF0.resize(static_cast<size_t>(totalFrames));
            for (int i = 0; i < totalFrames; ++i) {
                audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);
            }
            
            // Step 5: Sync global deltaPitch back to each note's deltaPitch vectors
            for (auto& n : project->getNotes()) {
                if (n.isRest()) continue;
                
                const int startFrame = n.getStartFrame();
                const int endFrame = n.getEndFrame();
                const int numFrames = endFrame - startFrame;
                
                if (numFrames <= 0) continue;
                
                std::vector<float> noteDelta(static_cast<size_t>(numFrames));
                for (int i = 0; i < numFrames; ++i) {
                    const int globalIdx = startFrame + i;
                    if (globalIdx >= 0 && globalIdx < totalFrames) {
                        noteDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
                    }
                }
                
                n.setOriginalDeltaPitch(noteDelta);
                n.setDeltaPitch(noteDelta);
            }
            
            // CRITICAL: Recompose F0 from basePitch and deltaPitch (apply UV mask to hide non-voiced regions)
            PitchCurveProcessor::composeF0InPlace(*project, /*applyUvMask=*/true);
        }
    }

    juce::String getName() const override { return "Merge Notes"; }

private:
    Project *project;
    Note leftNoteSnapshot;
    Note rightNoteSnapshot;
    Note mergedNoteSnapshot;
    std::function<void()> onChanged;
};