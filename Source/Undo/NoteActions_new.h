#pragma once

#include "../JuceHeader.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include "../Audio/PitchCurveProcessor.h"
#include "UndoableAction.h"
#include <functional>

namespace chowdsp
{
template<typename T>
struct Serializable;
}

class NoteDeleteAction;
class NoteAddAction;

class NoteBoundaryAdjustAction;

// Define the serialization methods outside the class
extern template struct chowdsp::Serializable<NoteDeleteAction>;
extern template struct chowdsp::Serializable<NoteAddAction>;
extern template struct chowdsp::Serializable<NoteBoundaryAdjustAction>;

class NoteFloatPropertyAction : public UndoableAction
{
public:
    NoteFloatPropertyAction(Note *note, float oldVal, float newVal,
                           void (Note::*setFunc)(float), float (Note::*getFunc)() const,
                           const juce::String &propertyName);
    bool perform() override;
    bool undo() override;
    int getSizeInUnits() override { return sizeof(*this); }
    juce::String getName() const override { return "Change " + propertyName_; }

private:
    Note *note;
    float oldVal, newVal;
    void (Note::*setFunc_)(float);
    float (Note::*getFunc_)() const;
    juce::String propertyName_;

    JUCE_LEAK_DETECTOR(NoteFloatPropertyAction)
};

class NoteIntPropertyAction : public UndoableAction
{
public:
    NoteIntPropertyAction(Note *note, int oldVal, int newVal,
                         void (Note::*setFunc)(int), int (Note::*getFunc)() const,
                         const juce::String &propertyName);
    bool perform() override;
    bool undo() override;
    int getSizeInUnits() override { return sizeof(*this); }
    juce::String getName() const override { return "Change " + propertyName_; }

private:
    Note *note;
    int oldVal, newVal;
    void (Note::*setFunc_)(int);
    int (Note::*getFunc_)() const;
    juce::String propertyName_;

    JUCE_LEAK_DETECTOR(NoteIntPropertyAction)
};

class NoteBoolPropertyAction : public UndoableAction
{
public:
    NoteBoolPropertyAction(Note *note, bool oldVal, bool newVal,
                          void (Note::*setFunc)(bool), bool (Note::*getFunc)() const,
                          const juce::String &propertyName);
    bool perform() override;
    bool undo() override;
    int getSizeInUnits() override { return sizeof(*this); }
    juce::String getName() const override { return "Change " + propertyName_; }

private:
    Note *note;
    bool oldVal, newVal;
    void (Note::*setFunc_)(bool);
    bool (Note::*getFunc_)() const;
    juce::String propertyName_;

    JUCE_LEAK_DETECTOR(NoteBoolPropertyAction)
};

class NoteStringPropertyAction : public UndoableAction
{
public:
    NoteStringPropertyAction(Note *note, const juce::String &oldVal, const juce::String &newVal,
                            void (Note::*setFunc)(const juce::String &), const juce::String &(Note::*getFunc)() const,
                            const juce::String &propertyName);
    bool perform() override;
    bool undo() override;
    int getSizeInUnits() override { return sizeof(*this); }
    juce::String getName() const override { return "Change " + propertyName_; }

private:
    Note *note;
    juce::String oldVal, newVal;
    void (Note::*setFunc_)(const juce::String &);
    const juce::String &(Note::*getFunc_)() const;
    juce::String propertyName_;

    JUCE_LEAK_DETECTOR(NoteStringPropertyAction)
};

class NoteBoundaryAdjustAction : public UndoableAction
{
public:
    NoteBoundaryAdjustAction(Note *leftNote, Note *rightNote,
                           const Note &originalLeft, const Note &originalRight,
                           const Note &changedLeft, const Note &changedRight,
                           std::function<void()> onChanged = nullptr)
        : leftNote(leftNote), rightNote(rightNote),
          originalLeftNote(originalLeft), originalRightNote(originalRight),
          changedLeftNote(changedLeft), changedRightNote(changedRight),
          onChanged(onChanged) {}

    void undo() override
    {
        if (!leftNote || !rightNote) return;
        
        // Store current durations to handle delta pitch resizing
        int currentLeftDuration = leftNote->getEndFrame() - leftNote->getStartFrame();
        int currentRightDuration = rightNote->getEndFrame() - rightNote->getStartFrame();
        
        // Restore original states
        leftNote->setStartFrame(originalLeftNote.getStartFrame());
        leftNote->setEndFrame(originalLeftNote.getEndFrame());
        leftNote->setSrcStartFrame(originalLeftNote.getSrcStartFrame());
        leftNote->setSrcEndFrame(originalLeftNote.getSrcEndFrame());
        
        rightNote->setStartFrame(originalRightNote.getStartFrame());
        rightNote->setEndFrame(originalRightNote.getEndFrame());
        rightNote->setSrcStartFrame(originalRightNote.getSrcStartFrame());
        rightNote->setSrcEndFrame(originalRightNote.getSrcEndFrame());
        
        // Adjust delta pitch arrays to match new durations
        adjustDeltaPitchForNewDuration(*leftNote, currentLeftDuration);
        adjustDeltaPitchForNewDuration(*rightNote, currentRightDuration);
        
        // Mark notes as dirty
        leftNote->markDirty();
        rightNote->markDirty();
        
        // Mark notes as synth dirty to ensure proper updates
        leftNote->markSynthDirty();
        rightNote->markSynthDirty();
        
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        if (!leftNote || !rightNote) return;
        
        // Store current durations to handle delta pitch resizing
        int currentLeftDuration = leftNote->getEndFrame() - leftNote->getStartFrame();
        int currentRightDuration = rightNote->getEndFrame() - rightNote->getStartFrame();
        
        // Apply changed states
        leftNote->setStartFrame(changedLeftNote.getStartFrame());
        leftNote->setEndFrame(changedLeftNote.getEndFrame());
        leftNote->setSrcStartFrame(changedLeftNote.getSrcStartFrame());
        leftNote->setSrcEndFrame(changedLeftNote.getSrcEndFrame());
        
        rightNote->setStartFrame(changedRightNote.getStartFrame());
        rightNote->setEndFrame(changedRightNote.getEndFrame());
        rightNote->setSrcStartFrame(changedRightNote.getSrcStartFrame());
        rightNote->setSrcEndFrame(changedRightNote.getSrcEndFrame());
        
        // Adjust delta pitch arrays to match new durations
        adjustDeltaPitchForNewDuration(*leftNote, currentLeftDuration);
        adjustDeltaPitchForNewDuration(*rightNote, currentRightDuration);
        
        // Mark notes as dirty
        leftNote->markDirty();
        rightNote->markDirty();
        
        // Mark notes as synth dirty to ensure proper updates
        leftNote->markSynthDirty();
        rightNote->markSynthDirty();
        
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override { return "Adjust Note Boundary"; }

private:
    void adjustDeltaPitchForNewDuration(Note& note, int oldDuration) {
        std::vector<float> oldDelta = note.getDeltaPitch();
        int newDuration = note.getEndFrame() - note.getStartFrame();
        
        if (oldDuration <= 0 || newDuration <= 0) {
            // If invalid duration, clear delta pitch
            note.setDeltaPitch(std::vector<float>());
            return;
        }
        
        if (oldDuration == newDuration) {
            // No change in duration, no adjustment needed
            return;
        }
        
        std::vector<float> newDelta;
        newDelta.reserve(newDuration);
        
        if (oldDelta.empty()) {
            // If no old delta, initialize with zeros
            newDelta.assign(newDuration, 0.0f);
        } else {
            // Resample the delta pitch to match new duration
            for (int i = 0; i < newDuration; ++i) {
                float ratio = static_cast<float>(i) / static_cast<float>(newDuration);
                float oldIndex = ratio * oldDuration;
                
                // Bilinear interpolation
                int idx1 = static_cast<int>(oldIndex);
                int idx2 = std::min(idx1 + 1, oldDuration - 1);
                float frac = oldIndex - idx1;
                
                if (idx1 >= 0 && idx1 < static_cast<int>(oldDelta.size())) {
                    float val1 = (idx1 < static_cast<int>(oldDelta.size())) ? oldDelta[idx1] : 0.0f;
                    float val2 = (idx2 < static_cast<int>(oldDelta.size())) ? oldDelta[idx2] : 0.0f;
                    
                    float interpolated = val1 * (1.0f - frac) + val2 * frac;
                    newDelta.push_back(interpolated);
                } else {
                    newDelta.push_back(0.0f);
                }
            }
        }
        
        note.setDeltaPitch(std::move(newDelta));
    }

    Note *leftNote;
    Note *rightNote;
    Note originalLeftNote;
    Note originalRightNote;
    Note changedLeftNote;
    Note changedRightNote;
    std::function<void()> onChanged;
};

class NoteAddAction : public UndoableAction
{
public:
    NoteAddAction(Project *project, const Note &note, std::function<void()> onAdd = nullptr, std::function<void()> onRemove = nullptr);
    bool perform() override;
    bool undo() override;
    int getSizeInUnits() override { return sizeof(*this); }
    juce::String getName() const override { return "Add Note"; }

private:
    Project *project;
    Note noteToAdd;
    std::function<void()> onAdd, onRemove;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteAddAction)
};

class NoteDeleteAction : public UndoableAction
{
public:
    NoteDeleteAction(Project *project, const Note &note, std::function<void()> onAdd = nullptr, std::function<void()> onDelete = nullptr);
    bool perform() override;
    bool undo() override;
    int getSizeInUnits() override { return sizeof(*this); }
    juce::String getName() const override { return "Delete Note"; }

private:
    Project *project;
    Note noteToDelete;
    std::function<void()> onAdd, onDelete;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteDeleteAction)
};

template<typename T>
struct chowdsp::Serializable<NoteDeleteAction>
{
    static constexpr bool is_serializable = false;
};

template<typename T>
struct chowdsp::Serializable<NoteAddAction>
{
    static constexpr bool is_serializable = false;
};

template<typename T>
struct chowdsp::Serializable<NoteBoundaryAdjustAction>
{
    static constexpr bool is_serializable = false;
};