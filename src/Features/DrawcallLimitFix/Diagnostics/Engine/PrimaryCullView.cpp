// PrimaryCullView's out-of-line reads of the cut (PrimaryCullView.h).

#include "PrimaryCullView.h"

namespace DCLF::Diagnostics
{
	bool PrimaryCullView::StoodInWhole(const void* a_node)
	{
		const auto& self = Self();
		if (!self.standInLive)
			return false;
		const auto& cut = self.cut;
		const auto it = cut.eligible.find(static_cast<const RE::NiAVObject*>(a_node));
		return it != cut.eligible.end() && it->second < cut.admitted.size() && cut.admitted[it->second] && !cut.mixed[it->second];
	}
}
