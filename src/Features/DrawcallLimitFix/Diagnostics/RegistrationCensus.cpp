#include "RegistrationCensus.h"

#include "CaptureParity.h"

#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Engine/PassCapture.h"
#include "Features/DrawcallLimitFix/Engine/SceneCapture.h"
#include "Features/DrawcallLimitFix/Engine/ShadowViews.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include <ankerl/unordered_dense.h>

namespace DCLF::RegistrationCensus
{
	namespace
	{
		struct Noted
		{
			// Held from the registration to Collect, and let go of there (the render thread, at Present): what Collect reads of the
			// geometry is alive whatever the engine releases meanwhile.
			RE::NiPointer<RE::BSGeometry> geometry;
			const RE::BSBatchRenderer* batch = nullptr;  // an identity: never dereferenced
			std::uint8_t shaderType = 0;                 // the pass's shader (BSShader::Type)
			std::uint8_t hint = 0;                       // accumulationHint
			std::uint8_t flags = 0;                      // NoteFlags
			std::uint8_t occlusionPhase = 0;
			Site site = Site::RegisterPass;
		};

		// Made on the first use, which only a census run reaches (its ring is large); spills past it, never drops (EventQueue).
		EventQueue<Noted, 1u << 16>& Queue()
		{
			static EventQueue<Noted, 1u << 16> queue;
			return queue;
		}

		// The views: the accumulators the census knows by address, the shadow views by kind (ShadowViews), the reflection faces and
		// the occlusion maps (PassCapture's), then any other batch renderer by the order it was first seen in.
		enum View : std::uint16_t
		{
			kMainDepth,
			kMain,
			kFirstPerson,
			kSun,
			kSpot,
			kPoint,
			kFocus,
			kShadowOther,
			kReflection,
			kOcclusionSky,
			kOcclusionPrecipitation,
			kOcclusion,
			kFirstOther,
		};
		constexpr std::array<const char*, kFirstOther> kViewNames{ "main depth", "main", "first person", "sun cascades", "spot shadows", "point shadows",
			"focus shadows", "other shadows", "reflection faces", "Skylighting occlusion", "precipitation occlusion", "occlusion" };

		// A Lighting geometry's class: [0, kReasons) tracked with that verdict (None is never one: kEligible*), [kUntracked,
		// kUntracked + kReasons) untracked with its live leaf's static verdict, then the rest.
		constexpr std::uint8_t kReasons = static_cast<std::uint8_t>(Ineligible::Count);
		enum LightingClass : std::uint8_t
		{
			kUntracked = kReasons,
			kEligibleRecord = 2 * kReasons,  // eligible, with a record, not a member of the view's phase
			kEligibleNoRecord,               // eligible, no record
			kMemberDefect,                   // a member of the view's phase the view withholds, drawn by the engine: a defect
			kMemberCrossfadeCopy,            // a member's LOD cross-fade copy (hint 10), the engine's by the toggles
			kMemberUnmodelledFade,           // a member's fade DCLF does not model (CS_DCLF_FADING=0)
			kMemberParityBoth,               // CS_DCLF_PARITY_BOTH: nothing is withheld
		kEligibleUnbound,                // eligible, a record, not bound (the coordinator's verdict alone)
		kEligibleBound,                  // eligible, bound, not a member of the view's phase in the frame's set (the coordinator's alone)
		};
		// The class key: Lighting's with the top bit, and the next when the class is the live approximation (no coordinator's reply
		// for the geometry yet, T7c0); any other shader's its type and the geometry's RTTI (static data).
		constexpr std::uint64_t kLightingKey = 1ull << 63;
		constexpr std::uint64_t kApproximateKey = 1ull << 62;
		constexpr std::uint64_t kRttiMask = (1ull << 48) - 1;

		constexpr std::array<const char*, RE::BSShader::Type::Total> kShaderNames{ "no shader", "Grass", "Sky", "Water", "BloodSplatter", "ImageSpace",
			"Lighting", "Effect", "Utility", "DistantTree", "Particle" };

		std::string LightingClassName(std::uint8_t a_class)
		{
			if (a_class < kReasons)
				return fmt::format("lighting: {}", kIneligibleNames[a_class]);
			if (a_class < kEligibleRecord)
				return fmt::format("lighting: untracked, {}", kIneligibleNames[a_class - kUntracked]);
			switch (a_class) {
			case kEligibleRecord:
				return "lighting: eligible, a record, not a member";
			case kEligibleNoRecord:
				return "lighting: eligible, no record";
			case kMemberDefect:
				return "lighting: MEMBER drawn by the engine";
			case kMemberCrossfadeCopy:
				return "lighting: a member's LOD cross-fade copy";
			case kMemberUnmodelledFade:
				return "lighting: a member's fade DCLF does not model";
			case kMemberParityBoth:
				return "lighting: a member (parity both)";
			case kEligibleUnbound:
				return "lighting: eligible, a record, unbound";
			case kEligibleBound:
				return "lighting: eligible, bound, not a member of the view's phase";
			default:
				return "lighting: ?";
			}
		}

		struct ViewClassKey
		{
			std::uint64_t cls = 0;
			std::uint16_t view = 0;
			bool operator==(const ViewClassKey&) const = default;
		};
		struct ViewClassHash
		{
			using is_avalanching = void;
			std::uint64_t operator()(const ViewClassKey& a_key) const noexcept
			{
				return ankerl::unordered_dense::hash<std::uint64_t>{}(a_key.cls ^ (static_cast<std::uint64_t>(a_key.view) * 0x9E3779B97F4A7C15ull));
			}
		};

		using GeometrySet = ankerl::unordered_dense::set<const RE::BSGeometry*>;
		struct Tally
		{
			std::uint64_t passes = 0;
			GeometrySet geometries;  // by address: a geometry released and another made at its address within a window counts once
		};
		struct GeometryCount
		{
			std::uint64_t passes = 0;
			std::string name;  // read at its first pass of the window
		};
		struct ClassWindow
		{
			std::uint64_t passes = 0;
			const RE::NiRTTI* rtti = nullptr;  // any other shader's: the geometry's
			bool attributed = true;
			ankerl::unordered_dense::map<const RE::BSGeometry*, GeometryCount> geometries;
			ankerl::unordered_dense::map<std::uint16_t, std::uint64_t> views;  // passes
		};

		/** @brief Render thread only (Collect, TakeReport). */
		struct Census
		{
			// The window's.
			std::uint32_t frames = 0;
			std::uint64_t passes = 0, attributed = 0, defects = 0;
			GeometrySet geometries;
			ankerl::unordered_dense::map<std::uint64_t, ClassWindow> classes;
			ankerl::unordered_dense::map<ViewClassKey, Tally, ViewClassHash> viewClasses;
			ankerl::unordered_dense::map<std::uint16_t, Tally> views;
			std::string firstDefect;
			bool coordinatorVerdicts = false;  // the last Collect's
			// T7c0: the window's non-member Lighting passes, those classified by the live approximation (no young reply), and the
			// coordinator's replies taken with the geometries they answered.
			std::uint64_t nonMemberPasses = 0, approximatePasses = 0, repliesTaken = 0, answered = 0;
			// T7c0, the scene work asynchronous: the coordinator's verdicts (SceneStore::AnswerCensus) by geometry, each with its pin (so
			// the address stays the geometry's while its verdict is kept; let go of here, on the render thread) and the Collect that took
			// it; the geometries seen since the last request, pinned; and whether a request is in flight (one at a time).
			struct Reply
			{
				RE::NiPointer<RE::BSGeometry> pin;
				SceneStore::CensusVerdict verdict;
				std::uint64_t collect = 0;
			};
			ankerl::unordered_dense::map<const RE::BSGeometry*, Reply> replies;
			ankerl::unordered_dense::map<const RE::BSGeometry*, RE::NiPointer<RE::BSGeometry>> unasked;
			bool inFlight = false;
			std::uint64_t collects = 0;  // the session's Collects
			// The session's: any other batch renderer's view, by first sight (grows).
			ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint16_t> others;
			// The known accumulators' renderers, made again when one of them changes.
			std::array<const void*, 2> accumulators{};
			ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint16_t> accumulatorViews;
		};
		Census census;

		void AddRenderers(ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint16_t>& a_map, const RE::BSBatchRenderer* a_batch, std::uint16_t a_view)
		{
			// A geometry group sorts its passes in a batch renderer of its own (ShadowViews::Rebuild, PassCapture::RefreshMainRenderers).
			if (!a_batch)
				return;
			a_map.try_emplace(a_batch, a_view);
			for (auto* group : a_batch->geometryGroups)
				if (group && group->batchRenderer)
					a_map.try_emplace(group->batchRenderer, a_view);
		}

		void RefreshAccumulatorViews()
		{
			// The Z-prepass's accumulator (render mode 0xC) and the main camera's. First person's (Main::unk_FPCulling) is not a
			// BSShaderAccumulator* at 0x14338c820 (a read there crashed at the main menu): its passes count as another view.
			static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> depthAccumulator{ REL::Offset(0x338c828) };
			static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> mainAccumulator{ REL::Offset(0x338c830) };
			const std::array<RE::BSGraphics::BSShaderAccumulator*, 2> accumulators{ *depthAccumulator, *mainAccumulator };
			const std::array<const void*, 2> sources{ accumulators[0], accumulators[1] };
			if (sources == census.accumulators)
				return;
			census.accumulators = sources;
			census.accumulatorViews.clear();
			constexpr std::array<std::uint16_t, 2> kViews{ kMainDepth, kMain };
			for (std::size_t i = 0; i < accumulators.size(); ++i)
				if (accumulators[i])
					AddRenderers(census.accumulatorViews, accumulators[i]->GetRuntimeData().batchRenderer, kViews[i]);
		}

		/** @brief One Collect's inputs, read once. */
		struct Frame
		{
			std::shared_ptr<const SetSnapshot> set;
			bool parityBoth = false;
			bool coordinatorVerdicts = false;
			ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint16_t> shadowViews;  // the frame's shadow views (ShadowViews)
			// A non-member's class, by geometry, with kApproximateKey when it is the live approximation.
			ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint64_t> verdicts;
			GeometrySet records;  // the installed tables' geometries (made on first use)
			bool recordsMade = false;
		};

		std::uint16_t ViewOf(const Noted& a_noted, const Frame& a_frame)
		{
			if (a_noted.site == Site::Occlusion)
				return a_noted.occlusionPhase == kSetOccluderSky ? kOcclusionSky : a_noted.occlusionPhase == kSetOccluderPrecipitation ? kOcclusionPrecipitation : kOcclusion;
			if (const auto it = census.accumulatorViews.find(a_noted.batch); it != census.accumulatorViews.end())
				return it->second;
			if (const auto it = a_frame.shadowViews.find(a_noted.batch); it != a_frame.shadowViews.end())
				return it->second;
			if (PassCapture::Get().IsReflectionRenderer(a_noted.batch))
				return kReflection;
			return census.others.try_emplace(a_noted.batch, static_cast<std::uint16_t>(kFirstOther + census.others.size())).first->second;
		}

		/** @brief The view's phase of the set, when the view withholds that phase's members from the engine (PassCapture's tests); else 0. */
		std::uint8_t WithheldPhase(const Noted& a_noted, std::uint16_t a_view, const Frame& a_frame)
		{
			const auto& set = a_frame.set;
			if (!set)
				return 0;
			switch (a_view) {
			case kMainDepth:
			case kMain:
				return kSetMain;  // WithholdMain: any main member
			case kReflection:
				return (a_noted.flags & kReflectionFace) && (set->drawn & kSetReflection) ? kSetReflection : 0;
			case kOcclusionSky:
			case kOcclusionPrecipitation:
				return (set->drawn & a_noted.occlusionPhase) ? a_noted.occlusionPhase : 0;
			default:
				break;
			}
			std::uint8_t phase = 0;
			return PassCapture::Get().CastersForBatch(a_noted.batch, &phase) ? phase : 0;
		}

		Ineligible StaticVerdict(const RE::BSGeometry& a_geometry)
		{
			return SceneStore::ClassifyStatic(SceneCapture::LiveLeaf(a_geometry).view, nullptr);
		}

		// T7c0: a reply is used while it is this young (Collects since it was taken): a geometry the census keeps seeing is asked again
		// with every request, so a used reply is a frame or two old; an older one (the geometry not seen meanwhile) is let go of.
		constexpr std::uint64_t kReplyCollects = 8;

		std::uint8_t CoordinatorClass(const SceneStore::CensusVerdict& a_verdict, const RE::BSGeometry& a_geometry)
		{
			// Untracked: the coordinator has no verdict; the live leaf's static one says why (the render thread's read).
			if (!a_verdict.tracked)
				return static_cast<std::uint8_t>(kUntracked + static_cast<std::uint8_t>(StaticVerdict(a_geometry)));
			if (a_verdict.reason != Ineligible::None)
				return static_cast<std::uint8_t>(a_verdict.reason);
			return !a_verdict.record ? kEligibleNoRecord : !a_verdict.resident ? kEligibleUnbound : kEligibleBound;
		}

		std::uint8_t ApproximateClass(const RE::BSGeometry& a_geometry, Frame& a_frame)
		{
			// The frame's own copies. Tracked content lies under a category node (the newest publication's set); what lies between the
			// leaf and it, and the live leaf's static verdict, give the reason the coordinator's walk gives (SceneStore::Classify:
			// static, then the parents); the installed tables, whether it has a record. The per-frame verdicts (hidden, switch, actor,
			// fading) are not told apart here: they count as eligible.
			auto& store = SceneStore::Get();
			const auto leaf = SceneCapture::LiveLeaf(a_geometry);
			const auto* lighting = SceneCapture::LeafView::Lighting(leaf.view.property);
			// T7c0: an ordered node rejects only a blended leaf (SceneStore's OrderedLeaf); a billboard any.
			const bool blended = leaf.view.AlphaBlending() || (lighting && lighting->materialAlpha < 1.0f);
			Ineligible parentReason = Ineligible::None;
			bool tracked = false;
			for (const RE::NiNode* node = a_geometry.parent; node; node = node->parent) {
				if (store.IsCategoryNode(node)) {
					tracked = true;
					break;
				}
				auto* object = const_cast<RE::NiNode*>(node);
				if (netimmerse_cast<RE::NiBillboardNode*>(object))
					parentReason = Ineligible::Billboard;
				else if (parentReason == Ineligible::None && blended && netimmerse_cast<RE::BSOrderedNode*>(object))
					parentReason = Ineligible::UnsupportedParent;
			}
			const auto verdict = SceneStore::ClassifyStatic(leaf.view, nullptr);
			if (!tracked)
				return static_cast<std::uint8_t>(kUntracked + static_cast<std::uint8_t>(verdict));
			if (verdict != Ineligible::None)
				return static_cast<std::uint8_t>(verdict);
			if (parentReason != Ineligible::None)
				return static_cast<std::uint8_t>(parentReason);
			if (!a_frame.recordsMade) {
				a_frame.recordsMade = true;
				if (const auto& tables = store.AcceptedTables())
					for (const auto* geometry : tables->objectGeometry)
						if (geometry)
							a_frame.records.insert(geometry);
			}
			return a_frame.records.contains(&a_geometry) ? kEligibleRecord : kEligibleNoRecord;
		}

		/** @brief A non-member's class key part: the coordinator's verdict where the census has one, else the live approximation. */
		std::uint64_t NonMemberClass(const Noted& a_noted, Frame& a_frame)
		{
			const auto& geometry = *a_noted.geometry;
			// The scene work inline (the parities): no pass runs beside the frame, so the coordinator's own verdicts are read.
			if (a_frame.coordinatorVerdicts)
				return CoordinatorClass(SceneStore::Get().CensusVerdictOf(&geometry), geometry);
			// Asynchronous (T7c0): asked of the coordinator with the next request (pinned until its reply is let go of), and its last
			// reply used while young; the live approximation only until one arrives.
			census.unasked.try_emplace(&geometry, a_noted.geometry);
			if (const auto it = census.replies.find(&geometry); it != census.replies.end() && census.collects - it->second.collect <= kReplyCollects)
				return CoordinatorClass(it->second.verdict, geometry);
			return kApproximateKey | ApproximateClass(geometry, a_frame);
		}

		std::string Describe(const RE::BSGeometry& a_geometry, bool a_rtti)
		{
			const auto* parent = a_geometry.parent;
			const auto* rtti = a_geometry.GetRTTI();
			return fmt::format("'{}' under '{}'{}", a_geometry.name.c_str() ? a_geometry.name.c_str() : "", parent && parent->name.c_str() ? parent->name.c_str() : "",
				a_rtti ? fmt::format(" ({})", rtti && rtti->name ? rtti->name : "?") : std::string());
		}

		void Count(const Noted& a_noted, Frame& a_frame)
		{
			const auto* geometry = a_noted.geometry.get();
			if (!geometry)
				return;
			const std::uint16_t view = ViewOf(a_noted, a_frame);
			std::uint64_t key = 0;
			const RE::NiRTTI* rtti = nullptr;
			bool attributed = true;
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			// A Lighting geometry is DCLF's to classify whichever shader its pass has (the shadow views' passes are Utility's).
			const bool lighting = property && netimmerse_cast<RE::BSLightingShaderProperty*>(const_cast<RE::BSShaderProperty*>(property));
			if (lighting) {
				std::uint64_t cls;
				if (const auto phase = WithheldPhase(a_noted, view, a_frame); phase && (a_frame.set->PhasesOf(geometry) & phase)) {
					// Kept by the hooks although its view withholds the phase: WithholdMain's exceptions, else a defect.
					const bool main = view == kMain || view == kMainDepth;
					cls = a_frame.parityBoth                                  ? kMemberParityBoth :
					      main && a_noted.hint == 10                           ? kMemberCrossfadeCopy :
					      main && (a_noted.flags & kFading)                    ? kMemberUnmodelledFade :
					                                                             kMemberDefect;
				} else {
					if (const auto it = a_frame.verdicts.find(geometry); it != a_frame.verdicts.end())
						cls = it->second;
					else
						cls = a_frame.verdicts.emplace(geometry, NonMemberClass(a_noted, a_frame)).first->second;
					++census.nonMemberPasses;
					census.approximatePasses += (cls & kApproximateKey) ? 1 : 0;
				}
				key = kLightingKey | cls;
				if (cls == kMemberDefect && census.defects++ == 0)
					census.firstDefect = fmt::format("{} in {}", Describe(*geometry, true),
						view < kFirstOther ? std::string(kViewNames[view]) : fmt::format("other #{}", view - kFirstOther));
			} else {
				rtti = geometry->GetRTTI();
				attributed = a_noted.shaderType > RE::BSShader::Type::None && a_noted.shaderType < RE::BSShader::Type::Total && rtti;
				key = (static_cast<std::uint64_t>(a_noted.shaderType) << 48) | (reinterpret_cast<std::uintptr_t>(rtti) & kRttiMask);
			}
			++census.passes;
			census.attributed += attributed ? 1 : 0;
			census.geometries.insert(geometry);
			auto& viewTally = census.views[view];
			++viewTally.passes;
			viewTally.geometries.insert(geometry);
			auto& viewClass = census.viewClasses[{ key, view }];
			++viewClass.passes;
			viewClass.geometries.insert(geometry);
			auto& cls = census.classes[key];
			cls.rtti = rtti;
			cls.attributed = attributed;
			++cls.passes;
			++cls.views[view];
			const auto [it, added] = cls.geometries.try_emplace(geometry);
			if (added)
				it->second.name = Describe(*geometry, lighting);
			++it->second.passes;
		}

		std::string ViewName(std::uint16_t a_view)
		{
			return a_view < kFirstOther ? std::string(kViewNames[a_view]) : fmt::format("other #{}", a_view - kFirstOther);
		}

		std::string ClassName(std::uint64_t a_key, const ClassWindow& a_class)
		{
			if (a_key & kLightingKey)
				return LightingClassName(static_cast<std::uint8_t>(a_key & 0xFF)) + ((a_key & kApproximateKey) ? " (live approximation)" : "");
			const auto type = static_cast<std::size_t>(a_key >> 48);
			return fmt::format("{}: {}", type < kShaderNames.size() ? kShaderNames[type] : "shader ?", a_class.rtti && a_class.rtti->name ? a_class.rtti->name : "no RTTI");
		}
	}

	void Note(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, Site a_site, std::uint8_t a_flags, std::uint8_t a_occlusionPhase)
	{
		if (!a_pass || !a_pass->geometry)
			return;
		Noted noted;
		noted.geometry = RE::NiPointer<RE::BSGeometry>(a_pass->geometry);
		noted.batch = a_batch;
		noted.shaderType = a_pass->shader ? static_cast<std::uint8_t>(a_pass->shader->shaderType.get()) : std::uint8_t{ 0 };
		noted.hint = a_pass->accumulationHint;
		noted.flags = a_flags;
		noted.occlusionPhase = a_occlusionPhase;
		noted.site = a_site;
		Queue().Push(std::move(noted));
	}

	void Collect()
	{
		if (!Enabled())
			return;
		++census.frames;
		RefreshAccumulatorViews();
		Frame frame;
		frame.set = PassCapture::Get().CurrentSet();
		frame.parityBoth = PassCapture::ParityBoth();
		// DrawcallLimitFix.cpp's SceneWorkInline: the parities that run the scene pass at the frame's start, on the render thread.
		frame.coordinatorVerdicts = SwitchEnabled(Switch::PersistentParity) || SwitchEnabled(Switch::WalkParity) || CaptureParity::Enabled();
		census.coordinatorVerdicts = frame.coordinatorVerdicts;
		++census.collects;
		// T7c0: the coordinator's reply to the last request, kept by geometry (a reply replaced lets go of its pin here).
		if (auto reply = frame.coordinatorVerdicts ? nullptr : SceneStore::Get().TakeCensusReply()) {
			++census.repliesTaken;
			census.answered += reply->geometries.size();
			for (std::size_t i = 0; i < reply->geometries.size(); ++i) {
				const auto* geometry = reply->geometries[i].get();
				census.replies[geometry] = { std::move(reply->geometries[i]), reply->verdicts[i], census.collects };
			}
			census.inFlight = false;
		}
		// The frame's shadow views (ShadowViews::Rebuild at BeforeShadowMaps; their lights live until the next update).
		if (const auto& views = ShadowViews::Get(); views.Valid())
			for (const auto& view : views.All()) {
				const std::uint16_t id = view.focus                                          ? kFocus :
				                         view.kind == ShadowViews::Kind::Directional          ? kSun :
				                         view.kind == ShadowViews::Kind::Frustum              ? kSpot :
				                         view.kind == ShadowViews::Kind::Parabolic            ? kPoint :
				                                                                                kShadowOther;
				AddRenderers(frame.shadowViews, view.batch, id);
			}
		// Each noted pass's geometry reference is let go of here, on the render thread (EventQueue resets the cell).
		Queue().Drain([&](Noted&& a_noted) { Count(a_noted, frame); });
		// T7c0: the distinct geometries seen since the last request, asked of the coordinator's next scene pass (no wake: the frame's
		// own pass takes it), once the last request's reply was taken.
		if (!frame.coordinatorVerdicts && !census.inFlight && !census.unasked.empty()) {
			auto request = std::make_shared<SceneStore::CensusRequest>();
			request->geometries.reserve(census.unasked.size());
			for (auto& [geometry, pin] : census.unasked)
				request->geometries.push_back(std::move(pin));
			census.unasked.clear();
			census.inFlight = true;
			SceneStore::Get().PostCensusRequest(std::move(request));
		}
	}

	std::string TakeReport()
	{
		if (!Enabled() || !census.passes) {
			census.frames = 0;
			return {};
		}
		const double frames = std::max(1u, census.frames);
		const double share = 100.0 * static_cast<double>(census.attributed) / static_cast<double>(census.passes);
		// T7c0: asynchronous, the coordinator's replies where the census had one, the live approximation (marked) where it had none yet.
		const std::string verdicts = census.coordinatorVerdicts ?
		                                 std::string("the coordinator's (scene work inline)") :
		                                 fmt::format("the coordinator's replies for {} of {} non-member passes ({} replies, {} geometries answered), the live "
													 "approximation for {} (no reply yet: those classes marked so; scene work asynchronous)",
											 census.nonMemberPasses - census.approximatePasses, census.nonMemberPasses, census.repliesTaken, census.answered,
											 census.approximatePasses);
		std::string out = fmt::format(
			"[DCLF] census (T7a) over {} frames: {} passes the engine kept ({:.0f} a frame), {} geometries; {:.1f}% attributed to a named class{}; {} member passes "
			"drawn by the engine{}; Lighting verdicts {}\n",
			census.frames, census.passes, census.passes / frames, census.geometries.size(), share, share >= 95.0 ? " <- OK" : " <- UNATTRIBUTED (gate 95%)",
			census.defects, census.defects ? " <- DEFECT; first: " + census.firstDefect : std::string(" <- OK"), verdicts);
		// By view, each with its classes; most passes first.
		std::vector<std::pair<std::uint16_t, const Tally*>> views;
		for (const auto& [view, tally] : census.views)
			views.emplace_back(view, &tally);
		std::sort(views.begin(), views.end(), [](const auto& a, const auto& b) { return a.second->passes > b.second->passes; });
		std::vector<std::tuple<std::uint64_t, std::uint16_t, const Tally*>> viewClasses;
		for (const auto& [key, tally] : census.viewClasses)
			viewClasses.emplace_back(key.cls, key.view, &tally);
		std::sort(viewClasses.begin(), viewClasses.end(), [](const auto& a, const auto& b) { return std::get<2>(a)->passes > std::get<2>(b)->passes; });
		for (const auto& [view, tally] : views) {
			std::string classes;
			for (const auto& [cls, classView, classTally] : viewClasses)
				if (classView == view)
					classes += fmt::format("{}{} {}/{}", classes.empty() ? "" : ", ", ClassName(cls, census.classes[cls]), classTally->passes, classTally->geometries.size());
			out += fmt::format("[DCLF] census view {}: {} passes ({:.1f} a frame), {} geometries; by class (passes/geometries): {}\n", ViewName(view), tally->passes,
				tally->passes / frames, tally->geometries.size(), classes);
		}
		// By class across the views, with the geometries that registered the most passes.
		constexpr std::size_t kTop = 5;
		std::vector<std::pair<std::uint64_t, const ClassWindow*>> classes;
		for (const auto& [key, cls] : census.classes)
			classes.emplace_back(key, &cls);
		std::sort(classes.begin(), classes.end(), [](const auto& a, const auto& b) { return a.second->passes > b.second->passes; });
		for (const auto& [key, cls] : classes) {
			std::vector<std::pair<std::uint16_t, std::uint64_t>> byView(cls->views.begin(), cls->views.end());
			std::sort(byView.begin(), byView.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string inViews;
			for (const auto& [view, passes] : byView)
				inViews += fmt::format("{}{} {}", inViews.empty() ? "" : ", ", ViewName(view), passes);
			std::vector<const GeometryCount*> top;
			top.reserve(cls->geometries.size());
			for (const auto& [geometry, count] : cls->geometries)
				top.push_back(&count);
			const auto shown = std::min(kTop, top.size());
			std::partial_sort(top.begin(), top.begin() + shown, top.end(), [](const auto* a, const auto* b) { return a->passes > b->passes; });
			std::string names;
			for (std::size_t i = 0; i < shown; ++i)
				names += fmt::format("{}{} x{}", names.empty() ? "" : ", ", top[i]->name, top[i]->passes);
			out += fmt::format("[DCLF] census class {}{}: {} passes ({:.1f} a frame), {} geometries; in {}; top: {}\n", ClassName(key, *cls),
				cls->attributed ? "" : " (unattributed)", cls->passes, cls->passes / frames, cls->geometries.size(), inViews, names);
		}
		// The window restarts; the other renderers' numbering and the accumulators' views are the session's.
		census.frames = 0;
		census.passes = census.attributed = census.defects = 0;
		census.geometries.clear();
		census.classes.clear();
		census.viewClasses.clear();
		census.views.clear();
		census.firstDefect.clear();
		census.nonMemberPasses = census.approximatePasses = census.repliesTaken = census.answered = 0;
		// T7c0: the replies too old to be used let go of (their pins, here on the render thread); a request in flight a whole window (no
		// scene pass answered it: DCLF off) lets go of what waited for the next, which the next frame asks again.
		for (auto it = census.replies.begin(); it != census.replies.end();)
			it = census.collects - it->second.collect > kReplyCollects ? census.replies.erase(it) : std::next(it);
		if (census.inFlight)
			census.unasked.clear();
		return out;
	}
}
