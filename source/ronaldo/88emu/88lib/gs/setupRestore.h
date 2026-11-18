#pragma once

#include "88lib/gs/partState.h"

namespace emu88Lib::gs
{
	// Finite MIDI configuration for a freshly booted board, with no notes or reset.
	// Includes the tracked parts, effects and bank latches; not front-panel edits
	// that have not been read back, or the tracker's private RPN selections.
	std::vector<synthLib::SMidiEvent> createSetupRestore(DeviceModel _model, const Setup& _setup,
		std::shared_ptr<const ToneCatalog> _catalog = {});
}
