// IndirectDraws for a build without the render graph or ORG module services: every member a stub, and DCLF marked failed so it never
// claims or draws anything.
#if !(defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES))

#	include "IndirectDraws.h"

namespace DCLF
{
	struct IndirectDraws::Impl
	{};
	// Without the render graph DCLF never draws: it has no claims (Failed).
	IndirectDraws::IndirectDraws() { failed.store(true, std::memory_order_relaxed); }
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
	bool IndirectDraws::FitsScene(const void*, std::uint32_t, bool) const { return true; }
	std::uint64_t IndirectDraws::SceneFitSerial() const { return 0; }
	void IndirectDraws::TakeAheadContext() {}
	void IndirectDraws::CaptureMainPass() {}
	void IndirectDraws::CaptureDepthPass() {}
	void IndirectDraws::ExecuteColour() {}
	void IndirectDraws::ExecuteForward() {}
	std::string IndirectDraws::ForwardReport() { return {}; }
	bool IndirectDraws::RefractionDrawnThisFrame() const { return false; }
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
	void IndirectDraws::NoteEventAdoption(double, std::uint32_t) {}
	std::shared_ptr<const void> IndirectDraws::AdoptSnapshot(std::uint32_t) { return nullptr; }
	void IndirectDraws::PostAheadContext() {}
	std::function<void(org::runtime::IUploadService&)> IndirectDraws::PrepareFrameUploads() { return {}; }
	void IndirectDraws::DropFrameUploads() {}
	void IndirectDraws::KickFadeWriteBack() {}
	void IndirectDraws::WriteFadeStores(bool, const std::function<bool(const void*)>&) {}
	void IndirectDraws::KickVisibilityReadback() {}
	void IndirectDraws::WriteAccumulated(bool, const std::function<bool(const void*)>&) {}
	bool IndirectDraws::AccumulatedVerdict(const void*) const { return true; }
	void IndirectDraws::NoteCameraCut(std::uint32_t) {}
	void IndirectDraws::RequestAccumulatedRefresh() {}
	std::string IndirectDraws::VisibilityReport() { return {}; }
	bool IndirectDraws::DecideTreeLod() { return false; }
	void IndirectDraws::CaptureReflectionFace() {}
	void IndirectDraws::ReflectionFaceCamera(const RE::NiCamera&, std::uint32_t) {}
	std::uint32_t IndirectDraws::ReflectionRootsOwned(bool) { return 0; }
	void IndirectDraws::PrepareReflection() {}
	bool IndirectDraws::ReflectionDrawable() const { return false; }
	bool IndirectDraws::ReflectionSlotReady(const void*, const PipelineCatalog*, std::uint32_t) const { return false; }
	void IndirectDraws::ExecuteReflection() {}
	std::string IndirectDraws::ReflectionReport() { return {}; }
	void IndirectDraws::BeginFirstPersonFrame() {}
	void IndirectDraws::ExecuteFirstPersonDepth(bool) {}
	void IndirectDraws::ExecuteFirstPersonColour() {}
	bool IndirectDraws::FirstPersonDrawable() const { return false; }
	bool IndirectDraws::FirstPersonSlotReady(const void*, const PipelineCatalog*, std::uint32_t) const { return false; }
	bool IndirectDraws::FirstPersonCovered() const { return false; }
	std::string IndirectDraws::FirstPersonReport() { return {}; }
	void IndirectDraws::NoteRevisionInputs() {}
	void IndirectDraws::BuildPoint() {}
	bool IndirectDraws::DecideCoverage() { return true; }
	std::uint64_t IndirectDraws::TakeStreamsRefused() { return 0; }
	void IndirectDraws::EndFrame() {}
	void IndirectDraws::DrainAsync() {}
	std::string IndirectDraws::AsyncReport() { return {}; }
}

#endif
