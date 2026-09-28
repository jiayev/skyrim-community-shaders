#if !(defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES))

#	include "IndirectDraws.h"

namespace DCLF
{
	struct IndirectDraws::Impl
	{};
	IndirectDraws::IndirectDraws() = default;
	IndirectDraws::~IndirectDraws() = default;
	IndirectDraws& IndirectDraws::Get()
	{
		static IndirectDraws draws;
		return draws;
	}
	void IndirectDraws::PublishClaims() {}
	bool IndirectDraws::DrewLastFrame(const RE::BSGeometry*, std::uint32_t) const { return false; }
	std::uint32_t IndirectDraws::DrainVisibilityFeedback(const std::function<void(const VisibilityFeedbackFrame&)>&) { return 0; }
	IndirectDraws::FeedbackStats IndirectDraws::TakeFeedbackStats() { return {}; }
	void IndirectDraws::CaptureMainPass() {}
	void IndirectDraws::CheckCapturePoint() {}
	void IndirectDraws::CaptureDepthPass() {}
	void IndirectDraws::ExecuteColour() {}
	void IndirectDraws::ProbeTargets(const char*) {}
	void IndirectDraws::RunEpoch(bool) {}
	void IndirectDraws::BeginShadowFrame() {}
	void IndirectDraws::CaptureShadowView(std::uint32_t, std::uint32_t) {}
	void IndirectDraws::ExecuteShadowFrame() {}
	void IndirectDraws::CaptureSkyOcclusion() {}
	bool IndirectDraws::SkyOcclusionReady() const { return false; }
	bool IndirectDraws::ExecuteSkyOcclusion() { return false; }
	void IndirectDraws::KickColourBuild() {}
	void IndirectDraws::KickZPrepassBuild() {}
	void IndirectDraws::KickShadowBuild() {}
	void IndirectDraws::EndFrame() {}
	void IndirectDraws::DrainAsync() {}
	std::string IndirectDraws::AsyncReport() { return {}; }
}

#endif
