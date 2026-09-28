# DCLF asynchronous publication: implementation status

The full migration is **not implemented or enabled**. Current DCLF still uses
same-frame AsyncWorker jobs, scene joins, mutable tables/lookups, and the existing
ORG epoch submission path. There is no `published` mode yet and no performance
improvement is claimed by this foundational change.

## Implemented foundations

- `ORGModuleServices::AsyncPrimitives` is a backend-independent header-only target.
  It can be configured without the RHI/shader services through
  `ORGModuleServices/cmake/AsyncPrimitives.cmake`.
- BasicRenderer's `SerializedTaskPump` implementation lives in that shared
  package. The old BasicRenderer header remains a source-compatible facade.
- The shared `GraphScheduler` interface owns cancellation scopes and supports
  classified immediate/delayed dispatch and optional tracing. BasicRenderer's
  graph now consumes it through an adapter; the existing constructor preserves
  its scheduler, task-domain policy, and renderer hooks. Cancellation queries
  copied by continuations retain their scheduler state rather than borrowing a
  callback argument. Foreign scopes are rejected using lifetime-stable identities.
- Renderer payload trace interpretation, GPU-buffer retirement notification,
  and ORG upload-ticket conversion now live in `RendererGraphAdapters.cpp`, not
  the graph implementation. Coalescing, exact-content retention, lifetime trace
  classification, and producer admission policy are immutable host metadata; the
  core no longer switches on renderer artifact kinds.
- `ArtifactResources.h` shares the existing immutable payload, lease, readiness,
  and backend-opaque GPU submission contracts. BasicRenderer's public names are
  aliases, not separate implementations. `ArtifactKindPolicy` conservatively
  retains exact content by default; BasicRenderer explicitly installs its
  existing active-list retention policy.
- `ArtifactIdentity.h` and `ArtifactSnapshot.h` share addresses, immutable
  revision/incarnation identities, strong version handles, dependency recipes,
  alternative expressions, snapshots and typed payload handles. These are
  parameterized by the host's kind type; no renderer kind inventory is imported.
  BasicRenderer retains its concrete public names and helper signatures through
  aliases and forwarding wrappers. Requirements remain non-owning metadata;
  snapshots/handles retain explicit leases.
- `GraphSchedulingLayout` supplies queue dimensions, the graph-control class,
  and legacy acceptance class. The core allocates acceptance mailboxes once from
  a validated layout, rather than sizing/indexing them with renderer enums.
  Producer, acceptance, and waiter admission validate class bounds. Invalid
  acceptance classes become failed graph results without running their actions;
  invalid waiter registration returns an empty subscription. The renderer adapter
  installs its original domain/lane choices.
- Renderer trace recording/report generation lives in `RendererGraphTrace.cpp`
  behind an injected trace-session interface. The default renderer constructor
  retains the existing CSV, Chrome trace, and summary formats. Shared scheduling
  construction without a trace factory leaves tracing disabled.
- Fire-and-forget `PostIntents` now queues noncoalescible host kinds as ordered
  exact requests. Explicit latest-value APIs reject them. Previously that posting
  API marked every request coalescible, bypassing transaction protection.
- `ArtifactBuild.h` now shares build contexts/results, requests, suspension
  factories, observation/awaiter registrations, producer scheduling contracts,
  diagnostics and kind-count-parameterized statistics. BasicRenderer retains its
  public names through aliases and supplies the original enum defaults through
  `RendererArtifactScheduling`. Shared defaults use numeric lane/domain zero
  with no admission group. The suspension identity allocator now lives in the
  compiled `ORGModuleServices::AsyncRuntime` target, with the original renderer
  function forwarding to that implementation. It is not duplicated per kind type.
  As before with a statically linked service, cross-DLL users exchanging IDs
  must route through one runtime owner, not independent static copies.
  Registration reset still cancels exactly once and retains its snapshot lease
  until that snapshot is replaced or destroyed.
- Trace configuration, numeric payloads, diagnostic helpers, mutex phases/counts,
  and the trace-session interface now live in the shared package. The session is
  parameterized by host kind/event/report types. BasicRenderer retains its event
  values, renderer report fields, formatting and recorder implementation; no
  renderer-specific trace events were added to the shared headers.
- `ORGModuleServices::AsyncStateGraph` now compiles the existing dependency engine
  independently of BasicRenderer/RHI/ORG. `StateGraph<Binding>` contains the
  original algorithm; `StateGraphTypes` supplies kind, scheduling and trace types.
  The compiled default supports numeric kinds 0..63. Hosts can explicitly
  instantiate their own binding once; BasicRenderer does so behind its original
  `AsyncStateGraph` constructors and hook interfaces. Its destructor drains before
  destroying the wrapper so renderer callbacks cannot outlive that wrapper.
  There is no second renderer implementation of the algorithm.
  The shared target keeps TBB, spdlog and BasicTelemetry as CPU dependencies;
  existing SARP telemetry names remain unchanged for comparison continuity.
  Independent installed packages `ORGAsyncRuntime` and `ORGAsyncStateGraph` expose
  the CPU targets without pulling the GPU module package into consumers.
  This does not yet provide a nonblocking DCLF frame path.
- `DCLF::PublishedSceneExecutor` implements the shared scheduler with one serialized
  coordinator and two dedicated preparation workers, separate from shader and ORG
  recording services. Each domain has a preallocated bounded queue (256 entries
  by default, including delayed tasks), plus at most one/two running tasks.
  Dispatch rejects saturation instead of waiting for capacity or running inline.
  Queue admission uses a short mutex; this is not a wait-free API. Cancellation
  wakes delayed tasks and workers release their captures before scope completion.
  Exceptions are retained for teardown/test waits; scopes retain cancellation
  state after executor destruction. Wait/Shutdown reject calls from its workers;
  the executor owner must outlive callbacks. Shutdown drains only for teardown.
  Statistics expose queue/active/high-water counts and accepted/completed/cancelled/
  rejected/failed work. Optional per-task tracing is not implemented yet.
  The separately built CPU library is linked into CS but not instantiated by the
  live frame path. Task queue bounds do not implement the scene-byte budget or the
  one-building/one-ready/one-pending capture admission policy.
- `PublicationExchange<T>` implements a bounded single-coordinator/single-owner
  handoff with active, ready, and retired slots. Selection does not allocate,
  acquire a mutex, scan the scene, or release the exchange's old payload. A
  nonthrowing acceptance predicate must validate generation and reserve executable
  capacity; the exchange itself does not prove GPU readiness. Rejected candidates
  and replaced active references are reclaimed by the producer.
- `LeasedArraySlots<T, N>` permits an external triple-buffer exchange to retain
  immutable payload storage after returning a slot. Writers replace backing only
  when a reader still holds its version. This helper does not synchronize slot
  ownership; the existing face snapshot atomic exchange does that.
- Face snapshots use leased whole-head storage. `ShapeView` and scene-table
  `FaceStream` carry the lease, and the existing main/shadow payload vector copies
  retain it. Head retirement and subsequent morph captures cannot invalidate a
  payload's positions. The usual three buffers are allocated during registration;
  only retained older versions require additional backing.

Shared-package changes are mirrored in the BasicRenderer and CS submodule working
trees. Both must be committed/pinned when delivering the migration.

## Required continuation, in order

The offline capture model now lives in `CapturedScene.h/.cpp`. Its factory copies
member transforms and evaluated component bytes into immutable group pages;
geometry/material are required for each member, with pose/face components required
when marked. Incomplete, duplicated or mismatched components are rejected. Pages
retain version-qualified resource owners and carry source-update, incarnation and
world-generation identities. Resource leases guarantee lifetime, not immutable GPU
contents; the adapter must supply versioned resources or ordered patch semantics.

`CapturedSceneReducer` applies ordered replacement/detach/reset events on the
coordinator. It retains detach tombstones, rejects stale revisions/incarnations and
old-world pages (even in a relabelled envelope), and exposes immutable roots sharing
unchanged group pages. Root-map copying is currently coordinator-side and linear;
this is not the final incremental scene table implementation or render-thread
selection. Lifecycle sequence numbers are global across resets. Allocation failure
leaves the prior root and tombstones intact. There is no coalescing in this reducer.

`CaptureAdmission` now bounds the coordinator's pending envelope by event count
(default 4096) and owned capture bytes (default 128 MiB). The caller supplies other
preparation/staging bytes on each admission. Accounting includes vector capacities,
but excludes allocator overhead and storage behind resource leases; event metadata
has its separate fixed count bound. This is not yet the full pipeline memory budget.
Pressure does not consume the event or its sequence: callers must retain and retry
in order, especially detach/reset/replacement events. One oversized whole group
may enter only with an empty pending envelope and zero other preparation bytes.

Only adjacent `RefreshGroup` events can coalesce. Refreshes must have identical
membership, geometry/material values and resource versions/owners; transforms,
pose and face values may change together. Components are canonicalized so this
comparison is linear in component count plus compared byte sizes. Structural
replacement, detach and reset events never merge. The reducer independently checks
refresh compatibility against its installed group, rejecting mislabeled structural
updates and refreshes without a live base. Coalescing/resource release happens only
on the coordinator, not the render thread. The ingress mailbox, one-building/one-ready
state machine, total resource-budget accounting and pressure-retention integration
remain integration work; the admission class is not yet connected to game hooks.

`CapturePreparation` now joins offline admission and reduction to an explicit
building/ready/acknowledgement state machine. It issues monotonically increasing
build tickets carrying immutable roots and an output-byte reservation. Wrong-ticket
completions cannot alter the current build. Failure, over-reservation results and
rejected ready results permit retry without requiring a new capture. Accepted resets
invalidate ready output immediately and logically cancel older builds; a running
build retains its reservation until completion. Ordered pending input is reduced
transactionally before issuing a successor. The conservative initial policy permits
one building OR one ready result plus pending captures, not simultaneous building
and ready successors. There are no waits in these coordinator methods.

The result is `PreparedCapture`, an opaque CPU artifact, not yet an executable
`PreparedScenePublication`. Its root is charged conservatively in full while
building/ready, plus reserved output bytes. Retained reducer roots, externally held
leases, allocator overhead, GPU resources and ingress storage still need unified
incremental accounting; this is not proof of the complete 128 MiB pipeline budget.
The graph producer/continuation adapters, completion/acknowledgement mailboxes and
frame lease selection remain unwired. Every method, including ready inspection and
acknowledgement, is coordinator-only; render-thread code must not call them directly.

`CaptureService` now hosts that offline state machine asynchronously. Engine-side
test callers post owning capture envelopes through a preallocated ring; a shared
serialized task pump drains them exclusively on the dedicated coordinator. An
injected CPU builder runs on a preparation worker and returns through a one-entry
completion mailbox. A separate ready slot and ticket-checked acknowledgement slot
hand results back without exposing mutable coordinator state. No public post/take/
acknowledgement operation waits for a build; short mailbox/scheduler mutexes remain.
Only shutdown drains workers. The direct builder remains a CPU test seam; the
optional graph backend described below supplies the dependency engine.

Ingress count and capture bytes are independently bounded (one isolated oversized
page permitted). They are not yet part of unified incremental memory accounting.
Rejected posts retain caller ownership for ordered retry. Invalid accepted input or
pump rejection marks the service faulted, rather than silently discarding lifecycle
events. Build failures pause preparation until new input or explicit Retry, avoiding
a tight failing-build loop. Resets retire undelivered old-world output on the
coordinator; already delivered leases still require consumer generation checks and
cleanup-lane release. These CPU mailboxes are not the lock-free frame-selection path
and confer no native draw ownership. Nothing instantiates this service in live CS.

`MakeGraphCaptureBackend` now connects the service to the shared graph through an
asynchronous backend interface, sharing the same dedicated executor. It posts a
typed immutable captured-root artifact, then a preparation artifact depending on
that exact root version. Nonzero build-ticket fingerprints qualify payload requests;
the graph rejects input-bearing requests without them. Preparation executes on the
preparation domain, while exact CPU-ready/terminal continuations report through the
coordinator mailbox. Graph addresses are released after completion, and graph
shutdown occurs before executor teardown. There is no graph wait or snapshot polling
in the normal service path. This currently has only the CPU root/preparation stages,
not shader, import, staging or executable-ticket producers.

`DclfCaptureGraph` is a separately linkable target configured by
`cmake/DclfCaptureGraph.cmake`; the CPU test build enables it with
`DCLF_TEST_SHARED_GRAPH`. It is now linked into `CommunityShaders.dll` through
the module-services CMake integration, but no live DCLF code instantiates the
service yet. Its source remains excluded from the plugin's legacy source glob.

The first concrete component adapter, `CaptureHeadFaceValues`, accepts a leased
`FaceSnapshots::HeadView` with complete shape membership, or the earlier
`ShapeView` span. It copies a versioned CPU header and float4 positions from the
retained owner, validates contiguous ranges and generation, and does not
dereference raw view pointers. The engine-side face morph publication already
captures all shapes of a head into one leased storage version. The live scene
walk now retains one `HeadView` per encountered head and associates it with its
face streams, using raw head pointers only as per-walk lookup keys. Existing GPU
uploads remain unchanged. This is not a complete actor capture or a published
rendering path; the worker adapter is not yet called by live hooks.

The live tracker now assigns monotonic member and actor-group identities through
`SceneIdentity`. Actor siblings discovered under the same `GetUserData()` actor
share a group ID; ordinary objects use their member ID as the group. Detach,
reattach, owner replacement, rescan and world reset allocate fresh IDs, while
table slot reuse changes every journal cause even if numeric draw values match.
The CPU suite tests these lifecycle transitions. These IDs are metadata only:
actor membership is not yet captured as a complete update, and no native
suppression uses them. The actor-key lookup still depends on scene-event order;
publication admission must conservatively reject any unverified replacement
ordering before this can grant ownership.

`GpuResources::Lease` now exposes an already imported D3D11 buffer's stable
Vulkan view, import generation, and owning COM lease without re-importing it.
Geometry slots retain the vertex/index leases and their generations, independent
of the cache's age-based eviction. A later publication can copy those leases
with its numeric geometry values. Import generations distinguish cache eviction
and pointer reuse; they are **not** versions of mutable buffer contents. The
published path still needs explicit ordered content patches, GPU-completion
retirement, and device-lifecycle checks before such a view is executable.

The persistent ORG host now accepts an owned preparation callback through
`TryPostOwnedPreparation` without waiting for mailbox capacity. The callback
runs on its graph-owning async thread with upload and descriptor services active,
outside an epoch callback. A full mailbox, absent async graph, or pending rebuild
leaves the callback with the caller for retry. The Vulkan host test checks
cross-thread service access and owned-input lifetime. DCLF does not yet submit
texture/import requests through this port; descriptor retirement and ticket
qualification remain to implement before ownership can use it.
`OpenRenderGraphPersistentVulkanHostTests` passed in both synchronous and async
modes after this change, including a blocked-consumer test that fills all 64
mailbox cells and verifies the overflow request stays with its caller. The
development CommunityShaders DLL also linked against the updated host. The
mailbox has not yet been exercised by live DCLF imports.

The cold Utility shader's skinned-normal branch used `Bones` even with
`DCLF_BINDLESS`, which deliberately omits that constant buffer. It now calls
`GetBoneRSMatrixBindless` with the captured object bone offset. DXC compiled
both the bindless and constant-buffer VS variants. The shader was backed up as
`build/dclf-profiles/Utility-before-bindless-normal.hlsl` before being deployed.
The first capped game run did not leave loading; a second 55.7-second run reached
534 Tracy frames and logged 33/33 Utility stages ready, zero failed, with no
`Bones` or Utility precompile errors. Its final 400 accumulator scopes measured
p50/p95/p99 614/925/1,119 microseconds (table derivation 519/783/944 microseconds). Those
steady-state values are similar to the previous stationary run and are not an
async pipeline speedup. The trace and CSV are in
`build/dclf-profiles/20260927-164940-utility-bindless-normal-warm/`.

Engine boundary inspection confirms `BuildScenePhase` guards loading-screen
renderer-data lifetime, and `WriteObject` still samples live geometry/property/skin
state. `JoinScenePhase` currently makes the legacy results safe to consume. Removing
those joins before replacing these reads would violate lifetime/thread-affinity
requirements. Remaining capture adapters must preserve that load guard, capture
complete actor membership and evaluated material/skin outputs, and lease GPU inputs.

This is an offline contract plus one engine-safe face capture boundary, not yet a
full-scene engine capture adapter or a parity path. Remaining component schemas,
hook-safe source sampling, actor membership discovery,
stable-ID assignment, graph producers, bounded capture admission and rendered motion
history are still required. The reducer neither proves GPU readiness nor grants
native ownership, and is not called by the live renderer.

1. Wire the tested shared graph/executor combination into the DCLF capture and
   coordinator service, using posted-only engine-thread
   entry points. The shared engine intentionally retains its legacy synchronous
   APIs and teardown drains for other hosts; DCLF must not call those on its
   published normal-frame path. SARP streaming count comparisons remain runtime
   validation work, not something proven by the extraction's CPU tests.
2. Populate the immutable CapturedSceneUpdate/group contracts from verified engine
   hooks, with complete actor/object membership. Eliminate deferred live engine reads before allowing jobs to outlive
   frames. Keep engine-affine sampling at verified hooks.
3. Move scene/accumulator derivation, shader/pipeline lookup preparation, payload
   construction and staging to coordinator-owned state plus two preparation
   workers. Replace mutable KeptView commit patches with separate frame patches.
4. Build PreparedScenePublication roots containing exact resources, payloads,
   ownership/exclusion data, and upload prerequisites. Add the 128 MiB admission
   budget and ordered lifecycle handling; storage leases alone do not bound memory.
5. Implement ORG publication-qualified ticket preparation and nonblocking
   all-segment reservation before native suppression. Move bulk upload recording
   to workers. Preserve DXVK/native ordering and depth/colour consistency.
6. Wire PublicationExchange into frame selection, retain one FrameSceneLease
   across shadows/depth/colour, and remove normal-frame joins/inline fallbacks
   from the new path. Add the startup legacy/published rollout switch only when
   both paths actually exist.
7. Validate with same-input parity, delayed workers, ownership coverage,
   lifecycle/resource stress, then <=60-second game captures before enabling by
   default. The existing shadow-membership parity discrepancy remains separate.

## Tests

Standalone shared tests build from `extern/ORGModuleServices/tests/Async` and
exercise retained storage through repeated slot recycling and owner destruction,
concurrent triple-buffer exchange, bounded publication admission, rejection,
whole-value selection, retained frame leases, producer-side reclamation, and task
pump notification/rejection/delayed-handoff behavior. They also exercise shared
payload typing/lifetimes, readiness milestones, and opaque upload prerequisites.
Shared identity tests cover unrelated host kind types, ABA generation mismatch,
non-owning recipes versus owning handles, alternative dependencies, and scheduler
layout bounds/index round trips.
Build-contract tests exercise all result/suspension factories, typed dependency
lookup, custom host scheduling enums/defaults, acceptance callbacks, request
status/handle conversion, and move/reset/destruction of observation and exact
wait registrations while retaining their snapshot leases.
The CPU-only runtime suite exercises a custom host trace event/report vocabulary,
numeric payload forwarding, scheduler/mutex metadata, configuration defaults,
diagnostic names and correlation IDs. It also checks 32,768 concurrent suspension
allocations across two translation units; the renderer regression checks that
legacy and shared allocation calls use the same counter.
These do **not** establish
actor grouping or GPU/native correctness in the game; that integration is pending.

Also run BasicRenderer's `TaskSchedulerManagerTests` and `AsyncStateGraphTests`
after changes to the shared pump facade or scheduler adapter, and the existing
DCLF CPU tests. Graph tests additionally exercise scheduler injection, retained
cancellation, delayed work, foreign-scope rejection, exclusive trace-sink
ownership, immutable host policy, and ordered noncoalescible posted revisions.
The graph suite also runs a one-lane/one-domain configuration, validates legacy
acceptance routing, and rejects invalid producers, acceptance actions, waiters,
and malformed startup layouts.
Trace registration now preserves an existing sink when a competing
registration is rejected; its event-emission path remains atomic/hazard-based.

Identity/scheduling extraction validation: `AsyncStateGraphTests` and
`TaskSchedulerManagerTests` each passed ten consecutive runs; the shared
primitive tests passed twenty consecutive runs. `BasicRendererPublicHeaderSmoke`
and `SARPRendererHostCore` built. Source/ABI audits passed and the renderer boundary
audit reported zero violations. The build-input
audit reports only the seven new, explicitly listed adapter/diagnostic files as untracked;
they must be included when committing. No game runtime/performance validation was
performed for this extraction step, and the deployed game DLL was not changed.
An initial invalid-waiter assertion used a test executable linked before the
final library rebuild; relinking against the final library resolved it, followed
by the ten-run graph regression pass above.

Build/request/continuation extraction validation: shared tests passed twenty
consecutive runs, including custom host scheduling traits and registration
lifetimes. Graph and scheduler tests each passed ten runs. Public-header smoke
and `SARPRendererHostCore` built successfully. Source/ABI/boundary audits passed;
the build-input audit still reports only the seven untracked adapter files above.
Both shared-package copies are byte-identical for this slice. No game launch,
deployment, performance capture, or DCLF runtime enablement was performed.

Trace/identity extraction validation: both CPU-only suites passed twenty runs;
graph/scheduler suites passed ten runs each, including report-format, bounded
trace, concurrent-stop, and legacy/shared identity namespace checks. Public-header
smoke and `SARPRendererHostCore` built. Source/ABI/boundary audits passed. The
build-input audit still flags the same seven untracked adapter files, not missing
source-list entries. Mirrored additions match byte-for-byte; the package's
tracked CMake/README contents match after newline normalization. No game run,
deployment, or performance claim accompanies this structural extraction.

Engine extraction validation: the independently linked numeric-kind graph tests
passed twenty runs, and an installed-package-only consumer of that same suite
also passed twenty runs. These build trees configure no BasicRenderer, RHI or ORG
targets. Coverage includes exact dependencies/retention, posted coalescing,
noncoalescible lifecycle requests, milestone continuations, failure propagation,
cancellation and shutdown. The existing renderer graph and scheduler suites
passed ten runs each against the shared implementation; public-header smoke and
host-core builds passed. Both primitive/runtime suites passed twenty runs each.
Source/ABI/boundary audits passed; the build-input audit continues to flag only
the seven previously untracked adapter files. GPU streaming-count and game
performance/visual validation remain outstanding; no game deployment was made.

Dedicated executor validation: `DclfKeptStateTests`, `DclfSceneExecutorTests`, and
`DclfSceneGraphTests` each passed fifty consecutive runs. Executor coverage includes
independent coordinator progress under two blocked preparation jobs, queue bounds,
delayed cancellation, foreign scopes, coordinator ordering, exception propagation,
worker-wait rejection, retained cancellation queries, and worker-side capture
destruction before shutdown completes. The installed-package graph integration
test verifies exact two-component dependency gating with reordered completions
while unrelated coordinator requests continue. This is not Skyrim actor-group
validation. The executor library also built through the actual CS Ninja build;
the generated plugin source list excludes its separately compiled translation
unit. No full plugin build, deployment, game run, or performance claim accompanies
this slice.

Offline capture validation: all four DCLF CPU suites passed thirty consecutive
runs. New tests cover all 24 geometry/material/pose/face completion permutations,
rejection of incomplete groups, deep-copy isolation from source mutations,
resource-owner retention, immutable old roots, detach/reattach ABA, stale source
updates, reset generation checks and coordinator-worker lifetime. These use
synthetic component bytes, not live Skyrim input; same-capture rendering parity
and game correctness remain unvalidated. The CPU library built in the actual CS
Ninja configuration. No full plugin build, deployment or game run was performed.

Admission validation: all four CPU suites passed thirty runs after adding tests
for pending-entry/byte pressure, retry without sequence loss, whole-group refresh
coalescing, ordered detach and structural replacement, isolated oversized admission,
external preparation-byte accounting and rejection of structural updates labelled
as refreshes. The CPU library also built through the actual CS Ninja configuration.
No live rendering or performance change is claimed by this slice.

Preparation-state validation: all four CPU suites passed thirty runs with the
state machine included. Tests cover late/wrong-ticket completion, immutable worker
inputs while newer captures coalesce, ready acknowledgement, failure/rejection retry,
reservation overrun, stale-world completion and ready invalidation. A real executor
worker is artificially delayed across a reset and its result is rejected without
starting overlapping preparation. The library built in the actual CS Ninja tree.
No game deployment, full plugin build or runtime performance validation occurred.

Mailbox-host validation: all five CPU suites passed thirty consecutive runs. The
new service suite covers preparation on a non-caller thread, reset during blocked
work, immutable retained output, stale/duplicate acknowledgements, explicit retry
after builder failure, visible invalid-input failure, post-shutdown rejection and
idempotent teardown. The actual CS Ninja configuration also built the CPU library.
No full plugin build, game run, deployment or performance claim accompanies this
offline integration.

Graph-backend/face-adapter validation: all six CPU suites passed thirty runs after
fixing the adapter's missing request fingerprint. The initial graph service test
failed before entering its builder; DebugMCP could not launch the C++ test, so the
cause was traced to the graph's explicit MissingFingerprint rejection contract and
verified by the passing graph-backed regression suite. Debugger breakpoints were
cleared. Face tests cover mixed generations/owners, truncated ranges, header/layout
contents and independence after source storage expires. The executor/service/face
library built through the CS Ninja configuration; the graph adapter built against
the independently installed shared graph. No game launch, deployment, full plugin
build or GPU publication validation occurred.

Plugin-link/whole-head validation: the shared graph target now links into the
actual `CommunityShaders.dll` build, which succeeded after the face-stream bridge
was added. `DclfCapturedSceneTests` checks complete-head copying and rejects
missing ranges; all six DCLF CPU suites passed once after this bridge. The build
has not been deployed or run in-game. The standalone 512-group synthetic capture
benchmark reported post-call p99 between 0.1 and 1.5 microseconds and completion
between 0.20 and 0.45 milliseconds across five direct/graph trials each. That is
only service overhead, not a same-save game A/B or evidence that rendering work
has moved off the render thread. An existing demanding-save Tracy baseline showed
`CS DCLF: accumulator tables and pipelines` at p50 620, p95 1404, p99 24280
microseconds over its last 400 frames; the live code still has that work.

Identity bridge validation: the six CPU suites passed after adding actor sibling,
detach/reattach, pointer-reuse, owner-change and world-reset cases. The live
scene table carries each member and group ID beside its persistent object slot;
the legacy renderer ignores these metadata columns. No new in-game timing or
visual result is claimed for this bridge.

Geometry-lease runtime validation: both the development Ninja and multi-target
Release plugin DLLs built after the import-lease change. One MO2 run reached
gameplay but its early Tracy recording ended during loading; a second run was
stopped at 42.9 seconds with a 12.1-second gameplay capture (947 Tracy frames,
943 accumulator scopes). DCLF logged 9,373 stable game buffers and zero rejected
imports, with main, depth and shadow epochs continuing. The final 400 accumulator
scopes measured p50/p95/p99 678/913/1,133 microseconds; table derivation was
572/779/970 microseconds and `BuildMainPayload` 529/649/739 microseconds. The
earlier demanding-save baseline had 619/1,404/24,280 microseconds for the
accumulator scope, but its camera moved while this capture's camera stayed fixed;
these are not a controlled A/B and do not prove an optimization. Shader
precompile logged several unsupported Utility VS `Bones` errors at startup,
while runtime reported 33 requested/ready shadow stages and no failed stage
requests. No visual or GPU debug-layer validation accompanied this capture.
The trace, per-zone steady-state CSV and game log are in
`build/dclf-profiles/20260927-163309-async-lease-gameplay/`. The deployed DLL's
SHA-256 matched the Release build after the run; the previous deployed DLL was
backed up as `build/dclf-profiles/CommunityShaders-before-async-lease.dll`.

ORG ticket-reservation foundation: `PersistentGraphHost` now offers
`TryReserveReadyEpochs` and `TrySubmitReservedEpoch`. A render-thread caller can
check that every required epoch has a current, complete ticket and retain those
exact tickets without a ticket wait, graph build, or inline reprepare. Submission
consumes each ticket once; an unexpected post-reservation revision change throws
and schedules async replacement instead of waiting or silently omitting a claimed
draw. The existing blocking `SubmitEpoch` path remains for legacy callers.
The synchronous and asynchronous Vulkan host tests pass, including bounded
owned-request admission, reservation, single consumption, resource refresh, and
rebuild refusal; the CS development DLL links. This is not yet a publication-
qualified reservation: DCLF does not use it to decide native ownership, and
ticket readiness alone is not proof that a scene publication can render.
Each reserved epoch now retains a control-mailbox credit. Owned preparation
cannot consume those credits, and the Vulkan test submits a reserved epoch while
the worker is blocked and every other mailbox cell is full. The legacy path
still may wait for mailbox space; a publication reservation cannot mix with it.
The reserved path also still records pending uploads on the render thread.
Worker-recorded bulk uploads and publication-qualified ownership remain separate
steps; neither is claimed by this test.

Live ownership boundary audit: `DrawcallLimitFix::BeginSceneFrame` runs from the
early `Main::Draw` hook before the primary cull and before native pass
registration. `PassCapture::Withhold` consults the currently published main and
shadow claim sets during registration, while `IndirectDraws::PublishClaims`
replaces main claims only after the colour epoch, for the following frame.
Consequently a published-mode frame lease must be selected at or before
`BeginSceneFrame` and install its compatible claim sets there; selecting it at
`EarlyPrepass` or `BeforeDeferredComposite` would be too late to make native
suppression and DCLF execution agree. The existing live flow still joins the
scene job in `AfterShadowMaps`/`EarlyPrepass` and builds accumulator tables
synchronously in `EarlyPrepass`.

Live claim-lease prerequisite: `PassCapture` now atomically installs one
immutable main+shadow claim bundle after `BeginSceneFrame` processes scene
events and before registration begins. The legacy claim producers still
publish for the following frame; the registration hooks and sun-shadow claim
query read the selected bundle throughout this frame. Feature-off and load
handling clear the bundle (loads also clear all shadow claims), handing
registration back to native rendering. This does not yet install claims from a
prepared scene publication or reserve its tickets.

The Release DLL built and a 45-second MO2/Tracy run reached the demanding save.
Its final 300-frame report had 2,514 passes withheld, 2,514 objects claimed,
zero claimed-but-undrawn objects, and zero ownership holes over 300 frames.
Colour, depth and shadow worker builds were used in all 300 reported frames,
but the legacy joins remained: average reported colour wait was 0.225 ms,
shadow 0.036 ms and depth 0.008 ms. The last 400 Tracy accumulator scopes
measured p50/p95/p99 648/826/992 microseconds, with 1,249 microseconds max.
This was not a controlled same-save baseline comparison and no async speedup is
claimed. Trace and log: `build/dclf-profiles/20260927-171924-frame-claims/`
under the SARP workspace. The active MO2 DLL was backed up to
`build/dclf-profiles/20260927-frame-claims/CommunityShaders-before-frame-claims.dll`
and replaced with the matching Release DLL.
An earlier deployment command in this run also overwrote the DLL in MO2's
disabled `Community Shaders` mod with the development build. Its intended backup
directory could not be created by that command, so the exact prior disabled-mod
DLL was not preserved. The active `Community Shaders DXVK ORG` mod was untouched
by that command and was separately backed up as noted above.

Accumulator derivation boundary: the live `BuildAccumulatePhase` now measures
its parts separately from `BuildScenePhase`. A profiled 300-frame interval in
the demanding save put about 0.23 ms in static classification, 0.15 ms in
frame classification, and 0.22 ms in the old combined record section. The
record section is now split into an engine-affine value capture and a
pointer-free `AccumulatePatch` applied to the scene table; the latter does not
dereference a geometry, shader property, material or pass. The same save's
profiled interval reported about 0.10 ms in patch capture and 0.17 ms in
value-only application. The application is **still synchronous on the render
thread**; this extraction establishes a capture/prepare boundary but neither
publishes immutable tables nor shortens the normal-frame critical path.

The extraction made the extras-watch bit follow the newly captured projected-UV
and land-blend flags, rather than the prior object flags. Residency joins are
included in the before/after membership journal. A 45.5-second unprofiled MO2
run after the extraction reached gameplay and logged zero ownership holes,
zero claimed-but-undrawn objects, and zero validation drops in its 300-frame
windows. Its last 400 accumulator scopes measured p50/p95/p99
646/886/1,093 microseconds versus 648/826/992 microseconds in the earlier
same-save, stationary-camera frame-claim trace. The tail is worse and this is
not an optimization result. The two new captures and logs are under
`build/dclf-profiles/20260927-173655-accumulator-patch-split/` and
`build/dclf-profiles/20260927-173844-accumulator-patch-unprofiled/` in SARP.
These runs did not exercise residency or visual correctness; that requires a
targeted save and remains open. The remaining expensive classification and
pipeline/material slot mutation still read live engine data and must move to
coordinator-owned, versioned scene state before the value-only patch can run
off-thread.

Event-driven capture work (September 27): the live scene store now keeps a
material-to-slot reverse index. Material write events visit dependent slots
instead of scanning the entire material table. Existing controller hooks now
detect actual changes to emissive multiplier/colour and texture-transform
buffers; the former wake property dependents at Prepass, and the latter wake
the short transform watch across the engine's two buffer phases. Actor wetness
is sampled once per stable actor group and fanned out to its mesh slots. The
shared Skin cache now resets a reused form ID when its actor instance changes
and prunes only idle entries instead of clearing every actor's fade together
at 1,024 entries.
LightLimitFix exposes the nearest room identity separately from its transient
room number; tracked objects cache the identity on attachment and refresh the
number when the light room map is rebuilt. The room map is still rebuilt by
the engine feature each frame.

Shadow texture and pipeline membership is maintained from per-slot dirty
contributions, with reference-count-equivalent slot sets and a full rebuild
when the index is invalidated. The same-table incremental/rebuild parity check
(`CS_DCLF_PERSISTENT_PARITY`) reported no mismatches in the final capped run;
the final three 300-frame windows also reported zero ownership holes. The
normal demanding-save run reported shadow-set maintenance at 7.5 us/frame
versus 50.9 us/frame in the preceding same-save build. Wetness was 63.2 versus
121.9 us/frame and material writes 8.3 versus 14.5 us/frame. These telemetry
intervals are similar but the save evolves during each capture, so they are
directional, not a controlled per-frame A/B. Texture lookup now skips the
per-register comparison when the material record version is unchanged;
volatile targets and import completion remain on the existing epoch path.
The final Tracy capture and game log are under
`build/dclf-profiles/20260927-183410-shadow-dirty-normal/`; parity capture is
`build/dclf-profiles/20260927-183312-shadow-dirty-parity/`.

After adding per-zone Tracy measurements and the material-record version
shortcut, another normal run (`20260927-183853-event-zones-final`) measured
p50/p95/p99 in microseconds: material writes 8.5/11.0/13.5, wetness
65.7/89.8/108.0, texture transforms 10.1/14.3/17.3, and shadow-set
maintenance 26.8/44.2/58.5 per invocation. The final 300-frame interval
reported 20.9 us/frame of shadow maintenance at 0.7 calls/frame; more scene
changes than the prior run explain why this is above its 7.5 us/frame. The
accumulator scope measured 816/1,182/1,437 us versus 847/1,436/1,796 us
in the earlier same-save normal run; scene motion and runtime variation mean
this is not an isolated effect size. The final parity run
(`20260927-184006-event-final-parity`) reported zero shadow-index mismatches,
zero material-frame or shading mismatches in five checks each, and zero
ownership holes. All game runs stayed below 48 seconds.

Remaining work for this event-driven plan: actor input/event coverage and
incarnation-keyed wetness retirement on lifecycle events; completion-driven texture/import leases;
static-classification writer coverage; and typed immutable capture records on
the publication coordinator. Actor shading, alpha and visibility still use
conservative current-frame sampling where writer coverage is incomplete.
The live pipeline remains legacy and still joins preparation in normal frames.

Event-owned lookup use (September 27 follow-up): pipeline and material use is
now emitted by the derivation and residency writers as exact per-frame slot
lists. The pre-epoch known-texture refresh consumes a material-record change
journal rather than scanning allocated slots. A change is retained until that
slot is first used, so writes before later admission are not lost. Epoch
material and pipeline lookup preparation and frame pipeline-constant sampling
also consume the use lists. The material-write path no longer scans the entire
material cache for orphan entries: a successful cache insertion is immediately
followed by slot allocation, and each slot-removal path erases its cache key.
On parity frames, the use lists are compared with the previous full-table
discovery. LightLimitFix now changes its room-map generation only when the
room-to-index mapping actually changes, avoiding unconditional DCLF room
rebinding. The comparison map is reused across frames.

The 45-second demanding-save parity run at
`build/dclf-profiles/20260927-191943-iteration-free-parity/` logged no use-list
disagreements, no material-frame or shading parity differences, and zero
ownership holes and claimed-but-undrawn objects in its reported 300-frame
windows. A 12-second Tracy capture (907 frames) measured
`CS.DCLF.RefreshKnownMaterialTextures` p50/p95/p99 at 1.33/2.54/3.42 us over
1,807 calls; the preceding same-save parity capture
(`20260927-190426-event-revert-final-parity`) measured 3.99/6.64/9.96 us over
1,816 calls. This removes discovery over unchanged material slots, not the
required epoch import work. The persistent-shadow input parity discrepancy
already tracked separately remains present; this run recorded no new
shadow-set index disagreement. Shader/material/actor writer coverage and
completion-driven descriptor leases are still required before all persistent
state discovery and age-based maintenance can be removed. Camera-dependent
inputs, active wetness fading and live pose/visibility capture still require
bounded current-frame work; they cannot be inferred from yesterday's events.

Descriptor retirement follow-up: ORG's descriptor service now exposes an
atomic slot-plus-owner retirement operation. DCLF places the imported image
and retained D3D11 SRV in that owner; both are released only after the same
submitted queue fences that permit slot reuse. This removes the 16-frame
image graveyard, whose fixed duration was not a GPU-completion guarantee.
The inactivity-triggered cache eviction is still age-based. It cannot become
reference-count-only until every publication and prepared execution ticket
retains its exact descriptor owners, and device/graph rebuild invalidates
cached indices. The new lease covers retirement of an evicted entry, not
all future publication/ticket ownership. The first capped demanding-save run
(`20260927-193949-descriptor-completion-parity`) reached a GPU device loss at
frame 581, before any >600-frame texture eviction could exercise the new
operation. Parity was clean up to that point except for the pre-existing
persistent-shadow membership discrepancy; the run is inconclusive for the
retirement change. Its Aftermath dump is
`gpu-crash-2026-09-27-19-40-29.nv-gpudmp` in the Skyrim SKSE log directory.
The periodic all-import sweep has also been replaced by one deadline per
cached view. `BeginFrame` processes only expired deadlines, and an entry still
in use is rescheduled when its deadline arrives. This removes the steady
whole-cache eviction scan without changing the conservative 600-frame
inactivity policy.
The second 45-second parity run
(`20260927-194439-descriptor-expiry-parity`) completed without device loss,
past 1,190 main epochs. Both reported 300-frame intervals had zero claimed
but undrawn main objects and zero validation drops; the final shadow interval
had zero not-ready views under ownership. The first interval had 24 such
shadow views during cold pipeline readiness, the same count seen in the
preceding `20260927-193208-iteration-free-bitmap-parity` baseline. Material
frame and shading parity checks found no difference; the pre-existing
persistent-shadow membership discrepancy remained. The four DCLF CPU tests
passed. A focused ORG fence-retirement test was added, but its D3D12 test
target is disabled in this Vulkan-only build. A separate test configuration
stopped before compilation because DirectX-Headers is not installed in that
configuration, so that test is not yet executed.

The 1.6.1170 float and colour material-controller hooks now compare the exact
destination field around the engine update and emit a material event only for
an actual write. Ghidra's controller updates at `0x14150DDE0` and
`0x14150EA50` establish the destination-index tables and their early exits;
other runtimes retain the conservative event. A second 44.5-second parity run
(`20260927-192504-iteration-free-controller-parity`) logged no material-frame
or shading mismatches, no use-journal disagreements, and zero ownership holes
in its reported 300-frame windows. It does not prove coverage of other
material writers or actor inputs.

A material-change journal was tested for the pre-epoch known-texture refresh.
It needed to retain changes to slots that became used later in the frame;
same-table parity exposed this and passed after a pending-slot fix. Its measured
refresh cost was about 4.1 us/call in one parity run; the version-aware scan
measured 4.5 us/call in the final parity run, with substantial run-to-run
variation. The journal added state and had no demonstrated net benefit, so it
was removed from the live path.

The final Release DLL (SHA-256
`7A58E9A6A565690507729CFD76FD1E9EFD8E1DC0CE6EB24244F6DDD26E2CC545`)
was deployed to the active `Community Shaders DXVK ORG` MO2 mod, with the
pre-change DLL backed up under `build/dclf-profiles/20260927-event-capture/`.
The final 44.5-second parity run is
`build/dclf-profiles/20260927-190426-event-revert-final-parity/`. It logged zero
shadow-index disagreements, zero material-frame or shading mismatches across
five checks each, zero claimed-but-undrawn objects, and zero ownership holes
in the final 300-frame interval. The four DCLF CPU tests passed. Neither these
tests nor the demanding-save run cover interior portal rotation, equipment
changes, water transitions, or GPU debug-layer validation.

After the residency journal correction, the final Release DLL passed another
44.5-second capped run: the last three 300-frame windows logged zero ownership
holes, zero claimed-but-undrawn objects and zero validation drops. One window
handed 15 withheld passes back to native. The scene changed substantially during
this capture (about 2,500 to 3,560 claimed objects), so its final 400-frame
accumulator p50/p95/p99 of 855/1,298/1,455 microseconds must not be compared
as a stationary performance A/B. Trace and log:
`build/dclf-profiles/20260927-174240-accumulator-resident-journal/`.

### Reference-driven material and descriptor retirement (2026-09-27)

The duplicate `SceneStore::materialCache` has been removed. Persistent material
slots already contain the evaluated records; `materialIndex` supplies reuse.
Each live slot now owns its engine material reference directly. Last-object
reference transitions are consumed after the complete reference-journal batch,
without the material slot's previous age grace period. Reacquisition within
the batch cancels retirement, and incarnation checks reject stale events.
Table reset releases these owners too; previously the duplicate map survived
`ResetSlotTables` without slots through which its references could retire.

A game-material destructor hook cannot replace this ownership rule: DCLF's
own retained reference can prevent that destructor from running. Ghidra's
SetMaterial (`0x14147BFF0`) and material-manager release (`0x1414F7A40`) paths
confirm replacement releases the old material through the engine reference
count. No new executable-address hook is installed for retirement.

ORG descriptor heaps no longer poll a retained-index list during allocation.
The final CPU lease callback returns a GPU-retired slot to the free list.
Lease generations prevent an older callback from reclaiming a newer lease.
`RetireDescriptorSlotWithOwner` now transfers backing ownership into the slot
when GPU completion precedes CPU lease release, closing the previous gap
where only the descriptor index, but not its imported backing, stayed alive.
Backing destruction happens outside the heap lock.

Four DCLF CPU tests pass, including same-batch reacquisition and slot reuse.
The fence/backing lifetime tests now run in both persistent Vulkan host test
modes (two passing tests); they are no longer limited to the unavailable
D3D12 test configuration. They cover multiple CPU consumers, GPU completion
before final lease release, immediate backing destruction on final release,
and descriptor-slot reuse without polling.

The legacy `CS_DCLF_MATERIAL_CACHE` switch now controls validation only:
`off` disables the diagnostic, `probe` compares every used material slot,
and default retains the bounded eight-record sampler. There is no second
material cache to disable. Geometry/pipeline age retirement and GpuTextures'
600-frame inactivity deadlines remain. Removing texture expiry safely still
requires explicit ownership for every publication, shadow packet, frame patch,
and executable ticket that retains descriptor indices. This change does not
claim that coverage or remove the remaining engine-affine samplers/joins.

The deployed Release DLL SHA-256 is
`4B7C92D8BB8C8A9FCE1EE65C890C38F7713BD66DEA5077E705FDB8E625AAFFD6`.
The final normal-sampler parity run (`20260927-200844-material-ownership-final-parity`)
lasted 44.99 seconds. It reported zero stale sampled materials, no material-frame
or shading mismatches, no main claimed-but-undrawn objects, and no device loss.
The pre-existing shadow-membership discrepancy and first-window 24 not-ready
shadow views remain; the later window had zero not-ready views. Material slot
retirement reached 1,325 while live slots fell from 587 to 438; the two logged
last-frame evaluation counts were 0 and 2, not an aggregate churn measurement.
Accumulator p50/p95/p99 were 824/1,225/1,623 us (845 samples), versus
899/1,308/1,590 us (808 samples) in `20260927-194439-descriptor-expiry-parity`.
These same-save rotating runs are not stationary A/B measurements; p99 did
not improve and no performance win is asserted. The intermediate
`20260927-200333-material-ownership-parity` run used an earlier build and is
not validation of the final duplicate-cache removal.
The separate all-used-material probe
(`20260927-200936-material-ownership-full-probe`, 44.56 seconds) reported
486/487 validated records in its logged frames, zero stale materials, zero
material-frame/transform/shading differences, and zero main claimed-but-undrawn
objects. The same existing shadow issues remained. Neither run constitutes
interior/equipment/cell-transition coverage or GPU debug-layer validation.

### Writer-maintained actor output membership (2026-09-27)

Wetness propagation now uses `ActorValueIndex`, owned by the scene tables.
Object writes attach/update incarnation-qualified member/group IDs; object
reset removes membership and the last member immediately retires the group.
Dense table resets clear the index, while retained-table frames keep it.
No per-frame actor-to-mesh hash map is reconstructed. Capture samples one
representative per actor at the existing safe boundary. Only a changed output
fans out over all members; unchanged actors visit only newly joined or
explicitly reinitialized mesh rows. Updates consume a complete scene-writer
batch and publish the same output to every affected part before payload build.

The index contains no engine pointers and copies independently for table parity.
Skin still owns shared native/DCLF once-per-frame fade advancement. Position,
water, death and stamina sampling, Skin's legacy cache retirement, and actor
shading sampling remain; this is not complete actor writer coverage. No new
engine-address hook is needed for membership because the existing scene writers
already observe the necessary object lifecycle transitions.

CPU tests cover unchanged-output elision, multipart fan-out, equipment join,
reinitialized rows, representative detach, actor/slot incarnation replacement,
last-member retirement, reset and independent snapshots. Runtime parity checks
membership and output against the original actor list using the same frame's
cached Skin outputs. Tracy plots expose sampled actors, changed actors and
visited mesh rows under `CS.DCLF.Wetness.*`.

Scene-record rewrites now retain the wetness row when both member and group
incarnations match. Previously every rewrite zeroed it, defeating changed-only
propagation. New/replaced identities still initialize their rows and are admitted
as pending members. Group/member arrays are dense; writer-only reverse lookup
and swap removal avoid node-based traversal in the capture loop. The tests also
compare 1,000 randomized event batches against full same-input derivation.

Intermediate validation: `20260927-201621-actor-index-parity` (45.14 s) passed
membership/value checks but still propagated almost every rewritten row.
After retaining those rows, `20260927-201935-actor-index-retained-parity`
(44.99 s) propagated 245–288 of about 1,450 mesh rows in later checks with zero
differences, but ended in a GPU device loss. Decoded Aftermath evidence in
that directory reports `Error_DMA_PageFault`, without a fault address or shader
attribution. The earlier 19:40 dump has the same error class, not proof of the
same root cause. The retained pre-change control
`20260927-202143-actor-index-control` (44.54 s) and candidate repeat
`20260927-202245-actor-index-retained-repeat` (44.50 s) both completed without
device loss. The repeat's wetness checks and main ownership checks passed.
The node-based candidate was slower (wetness 78/137/329 us p50/p95/p99 versus
57/82/124 us in that control), motivating dense storage before promotion.
The intermittent GPU fault remains unresolved; a clean repeat is not a fix.

The dense candidate (`20260927-202552-actor-index-dense-parity`, 44.54 s)
completed without device loss, membership/value mismatches or main ownership
holes. Later checks visited 225–320 of 1,448–1,462 mesh rows, sampling
177–179 actors. Wetness p50/p95/p99 were 51/89/312 us; this scope includes
the new periodic full wetness parity check, so its tail is not comparable to
the prior build without that check. Accumulator timings were 805/1,170/1,416 us.
Existing shadow-membership disagreement remains tracked separately.

Final normal-mode comparison (parity disabled for both, same demanding save):

| Zone (us p50/p95/p99) | Retained control | Dense actor index |
|---|---:|---:|
| Wetness | 50.5 / 70.4 / 106.8 | 49.7 / 72.6 / 85.2 |
| Accumulator | 645.6 / 948.3 / 1,132.4 | 712.6 / 1,028.1 / 1,186.5 |

Control: `20260927-202807-actor-index-control-normal` (44.28 s, 955 wetness
samples). Candidate: `20260927-202703-actor-index-dense-normal` (44.56 s,
934 samples). Both completed without device loss or main claimed-but-undrawn
objects. The candidate claimed about 2,622 objects versus 2,512 in the control;
these evolving-scene runs are not a workload-identical A/B. Median wetness cost
is effectively flat; the structural result is removal of the unconditional
per-mesh wetness pass, not a demonstrated overall frame-time win.

The final deployed candidate hash is
`DA90226E3AE172F9C1D1E5BCC21A881DA5EAA541F0BBA441EC47B66A1C2A3707`.
All four CPU tests pass. No equipment/water/cell-transition scenario or GPU
debug-layer run is claimed by these demanding-save captures. Remaining next
steps are actor-value notification coverage and lifecycle-owned shared Skin
state, resource leases for every descriptor consumer, static-classification
writers, and migration of captured derivation to the publication coordinator.
