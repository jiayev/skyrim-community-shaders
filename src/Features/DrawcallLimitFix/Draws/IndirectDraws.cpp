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
	std::uint32_t IndirectDraws::ShadowViewCapacity() { return UINT32_MAX; }
	void IndirectDraws::DecideShadowCoverage() {}
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
	void IndirectDraws::RefreshMainLookups() {}
	std::shared_ptr<const void> IndirectDraws::BuildAhead(std::shared_ptr<const void>) { return nullptr; }
	void IndirectDraws::PostAheadContext() {}
	std::function<void(org::runtime::IUploadService&)> IndirectDraws::PrepareFrameUploads() { return {}; }
	void IndirectDraws::DropFrameUploads() {}
	bool IndirectDraws::DrawsReady(const std::shared_ptr<const void>&) const { return true; }
	void IndirectDraws::InstallDraws(std::shared_ptr<const void>) {}
	void IndirectDraws::KickFadeWriteBack() {}
	bool IndirectDraws::DecideTreeLod() { return false; }
	void IndirectDraws::CaptureReflectionFace() {}
	void IndirectDraws::PrepareReflection() {}
	bool IndirectDraws::ReflectionDrawable() const { return false; }
	void IndirectDraws::ExecuteReflection() {}
	std::string IndirectDraws::ReflectionReport() { return {}; }
	void IndirectDraws::MakeRevisionShapes() {}
	void IndirectDraws::BuildPoint() {}
	void IndirectDraws::SelectRevision() {}
	bool IndirectDraws::DecideCoverage() { return true; }
	bool IndirectDraws::SetApplicable(std::uint32_t) const { return true; }
	bool IndirectDraws::RevisionClaims() const { return false; }
	void IndirectDraws::NoteSetApplied(std::uint32_t) {}
	std::uint64_t IndirectDraws::TakeStreamsRefused() { return 0; }
	void IndirectDraws::EndFrame() {}
	void IndirectDraws::DrainAsync() {}
	std::string IndirectDraws::AsyncReport() { return {}; }
}

#endif
