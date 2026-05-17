#pragma once

#include "../Models/Note.h"

/**
 * Stores pitch tool transformation parameters for a single note.
 * Used by UndoManager to capture and restore transformation state non-destructively.
 */
struct TransformParams
{
    float tiltLeft = 0.0f;
    float tiltRight = 0.0f;
    float varianceScale = 1.0f;
    float highPassCutoff = 0.0f;
    float midiNote = 0.0f;
    float deltaScale = 1.0f;
    float deltaOffset = 0.0f;

    TransformParams() = default;

    /** Capture all transformation params from a note. */
    static TransformParams fromNote(const Note& note)
    {
        TransformParams p;
        p.tiltLeft = note.getTiltLeft();
        p.tiltRight = note.getTiltRight();
        p.varianceScale = note.getVarianceScale();
        p.highPassCutoff = note.getHighPassCutoff();
        p.midiNote = note.getMidiNote();
        p.deltaScale = note.getDeltaScale();
        p.deltaOffset = note.getDeltaOffset();
        return p;
    }

    /** Apply all transformation params back to a note. */
    void applyToNote(Note& note) const
    {
        note.setMidiNote(midiNote);
        note.setTiltLeft(tiltLeft);
        note.setTiltRight(tiltRight);
        note.setVarianceScale(varianceScale);
        note.setHighPassCutoff(highPassCutoff);
        note.setDeltaScale(deltaScale);
        note.setDeltaOffset(deltaOffset);
    }

    bool operator==(const TransformParams& other) const
    {
        return tiltLeft == other.tiltLeft &&
               tiltRight == other.tiltRight &&
               varianceScale == other.varianceScale &&
               midiNote == other.midiNote &&
               deltaScale == other.deltaScale &&
               deltaOffset == other.deltaOffset;
    }

    bool operator!=(const TransformParams& other) const
    {
        return !(*this == other);
    }

    bool isIdentity() const
    {
        return tiltLeft == 0.0f &&
               tiltRight == 0.0f &&
               varianceScale == 1.0f &&
               deltaScale == 1.0f &&
               deltaOffset == 0.0f;
    }
};
