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
	std::uint8_t IndirectDraws::ShadowPhasesDrawn() const { return 0; }
	bool IndirectDraws::PhaseReady(std::uint32_t, std::uint8_t) const { return false; }
	std::uint64_t IndirectDraws::ShadowReadinessSerial() const { return 0; }
	void IndirectDraws::CaptureMainPass() {}
	void IndirectDraws::CheckCapturePoint() {}
	void IndirectDraws::CaptureDepthPass() {}
	void IndirectDraws::ExecuteColour() {}
	void IndirectDraws::ProbeTargets(const char*) {}
	void IndirectDraws::RunEpoch(bool) {}
	void IndirectDraws::BeginShadowFrame() {}
	void IndirectDraws::CaptureShadowView(std::uint32_t, std::uint32_t) {}
	void IndirectDraws::ExecuteShadowFrame() {}
	void IndirectDraws::CaptureOcclusion(std::uint32_t) {}
	bool IndirectDraws::OcclusionReady(std::uint32_t) const { return false; }
	std::uint32_t IndirectDraws::ExecuteOcclusion(std::uint32_t) { return 0; }
	void IndirectDraws::KickColourBuild() {}
	void IndirectDraws::KickZPrepassBuild() {}
	void IndirectDraws::KickFadeWriteBack() {}
	bool IndirectDraws::DecideTreeLod() { return false; }
	void IndirectDraws::CaptureReflectionFace() {}
	void IndirectDraws::PrepareReflection() {}
	bool IndirectDraws::ReflectionDrawable() const { return false; }
	void IndirectDraws::ExecuteReflection() {}
	std::string IndirectDraws::ReflectionReport() { return {}; }
	void IndirectDraws::JoinFadeWriteBack() {}
	void IndirectDraws::KickShadowBuild() {}
	void IndirectDraws::KickShadowBuildEarly() {}
	void IndirectDraws::MakeRevisionShapes() {}
	bool IndirectDraws::KeepEarlyShadowBuild() { return false; }
	void IndirectDraws::BeforePlacementJoin() {}
	void IndirectDraws::KickSceneStreams() {}
	std::array<std::uint64_t, 3> IndirectDraws::TakeStreamsStats() { return {}; }
	void IndirectDraws::EndFrame() {}
	void IndirectDraws::DrainAsync() {}
	std::string IndirectDraws::AsyncReport() { return {}; }
}

#endif
