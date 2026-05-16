#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Undo/UndoActions.h"
#include "CoordinateMapper.h"
#include <functional>

/**
 * Handles note splitting operations.
 */
class NoteSplitter {
public:
    NoteSplitter() = default;
    ~NoteSplitter() = default;

    void setProject(Project* proj) { project = proj; }
    void setUndoManager(PitchUndoManager* manager) { undoManager = manager; }
    void setCoordinateMapper(CoordinateMapper* mapper) { coordMapper = mapper; }

    /**
     * Find note at the given world coordinates.
     */
    Note* findNoteAt(float x, float y);

    /**
     * Split a note at the given frame position.
     * Returns true if split was successful.
     */
    bool splitNoteAtFrame(Note* note, int splitFrame);

    /**
     * Split note at world X coordinate.
     * Returns true if split was successful.
     */
    bool splitNoteAtX(Note* note, float x);

    /**
     * Find the boundary between two adjacent notes at the given X coordinate.
     * Returns the right note if mouse is near the left edge, or left note if near the right edge.
     * Returns nullptr if not near any boundary.
     */
    Note* findNoteBoundaryAt(float x, float y, float& boundaryX);

    /**
     * Merge two adjacent notes into one.
     * Returns true if merge was successful.
     */
    bool mergeNotes(Note* leftNote, Note* rightNote);

    // Callbacks
    std::function<void()> onNoteSplit;

private:
    Project* project = nullptr;
    PitchUndoManager* undoManager = nullptr;
    CoordinateMapper* coordMapper = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteSplitter)
};
