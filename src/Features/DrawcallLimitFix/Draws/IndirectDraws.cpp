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
	std::uint8_t IndirectDraws::ShadowCapability() const { return 0; }
	void IndirectDraws::UpdateShadowCapability() {}
	std::string IndirectDraws::ShadowCapabilityReport() const { return {}; }
	std::uint32_t IndirectDraws::ShadowViewCapacity() { return UINT32_MAX; }
	void IndirectDraws::DecideShadowCoverage() {}
	bool IndirectDraws::PhaseReady(const void*, const Lookups&, std::uint32_t, std::uint8_t, std::uint32_t*) const { return false; }
	std::uint64_t IndirectDraws::ShadowReadinessSerial() const { return 0; }
	bool IndirectDraws::FitsScene(const void*, std::uint32_t, bool) const { return true; }
	std::uint64_t IndirectDraws::SceneFitSerial() const { return 0; }
	void IndirectDraws::TakeAheadContext() {}
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
	void IndirectDraws::OcclusionView(std::uint32_t, const RE::NiCamera&) {}
	bool IndirectDraws::OcclusionReady(std::uint32_t) const { return false; }
	std::uint32_t IndirectDraws::ExecuteOcclusion(std::uint32_t) { return 0; }
	void IndirectDraws::PostLookupInputs() {}
	void IndirectDraws::ResolveLookups(const void*, const PipelineCatalog*, Lookups&, LookupsResolveState&) {}
	void IndirectDraws::PostSnapshotWork(std::shared_ptr<const void>) {}
	std::shared_ptr<const void> IndirectDraws::AdoptSnapshot(std::uint32_t) { return nullptr; }
	void IndirectDraws::PostAheadContext() {}
	std::function<void(org::runtime::IUploadService&)> IndirectDraws::PrepareFrameUploads() { return {}; }
	void IndirectDraws::DropFrameUploads() {}
	void IndirectDraws::KickFadeWriteBack() {}
	bool IndirectDraws::DecideTreeLod() { return false; }
	void IndirectDraws::CaptureReflectionFace() {}
	void IndirectDraws::ReflectionFaceCamera(const RE::NiCamera&, std::uint32_t) {}
	std::uint32_t IndirectDraws::ReflectionRootsOwned(bool) { return 0; }
	void IndirectDraws::PrepareReflection() {}
	bool IndirectDraws::ReflectionDrawable() const { return false; }
	void IndirectDraws::ExecuteReflection() {}
	std::string IndirectDraws::ReflectionReport() { return {}; }
	void IndirectDraws::NoteRevisionInputs() {}
	void IndirectDraws::BuildPoint() {}
	bool IndirectDraws::DecideCoverage() { return true; }
	bool IndirectDraws::RevisionClaims() const { return false; }
	std::uint64_t IndirectDraws::TakeStreamsRefused() { return 0; }
	void IndirectDraws::EndFrame() {}
	void IndirectDraws::DrainAsync() {}
	std::string IndirectDraws::AsyncReport() { return {}; }
}

#endif
