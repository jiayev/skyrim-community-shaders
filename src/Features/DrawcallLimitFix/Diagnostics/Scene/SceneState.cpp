#include "SceneState.h"

namespace DCLF::Diagnostics
{
	void TrackedNotesDelete::operator()(TrackedNotes* a_notes) const
	{
		delete a_notes;
	}

	TrackedNotes& NotesOf(const SceneView::Tracked& a_entry)
	{
		if (!a_entry.notes)
			a_entry.notes.reset(new TrackedNotes);
		return *a_entry.notes;
	}

	const TrackedNotes& NotesOrNone(const SceneView::Tracked& a_entry)
	{
		static const TrackedNotes none;
		return a_entry.notes ? *a_entry.notes : none;
	}

	SceneState& SceneDiagnostics()
	{
		static SceneState state;
		return state;
	}
}
