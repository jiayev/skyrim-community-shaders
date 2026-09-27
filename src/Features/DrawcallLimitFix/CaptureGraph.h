#pragma once

#include "CaptureService.h"

namespace DCLF::Published
{
	// CPU-only exact dependency chain: captured immutable root -> prepared output.
	// More GPU/resource stages must be added before this grants execution readiness.
	CaptureService::BackendFactory MakeGraphCaptureBackend(CaptureService::Builder);
}
