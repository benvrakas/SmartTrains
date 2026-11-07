#pragma once

#include "CoreMinimal.h"

// Hooks the start of a train's dock at a station, so docks shorter than the train check are seen too
// (Docs/Architecture.md, "Docking").
namespace STDockHooks
{
	void Install();
}
