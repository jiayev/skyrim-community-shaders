# DCLF scene lifetime survey

A working document for the redesign. It lists every pooled, published or snapshot structure in Drawcall Limit Fix that can hold game
objects alive, and who holds each one. The goal of the redesign is that readers cannot hold arbitrary references to pools, snapshots
or game objects, so that snapshot lifetime is bounded and old snapshots can be cleared directly once they are no longer needed.

Paths are relative to `src/Features/DrawcallLimitFix/` unless they say otherwise. Line numbers are as of `69551c7a4` plus the working
tree on `org_integration`, which includes the `TrimPool` fix. **(inference)** marks a conclusion that is read from the code and not
confirmed at runtime.

## 0. The model in one paragraph

The coordinator (scene lane) builds an immutable `ScenePublication` (`Core/Scene/Model/Publication.h:44-72`):
- `SceneTables` taken from a pool that never shrinks, carrying a node of the retirement chain (`Tables.h:540`)
- claims (`SetSnapshot`)
- `Lookups`, whose entries hold GPU texture and buffer owners
- the catalog
- two `EntrySpace` candidate snapshots (pooled, each holding `NiPointer`s to every entry root)
- the category node set

Each publication's per-publication deltas (placement plans, shading items, seed items, retired imports, all holding `NiPointer`s or
leases) go onto the `PublicationLog`. The snapshot builder wraps the publication into a `SceneSnapshot` (draws, payload, revision).
The render thread adopts that snapshot whole, and every GPU execution then holds the whole snapshot until it retires.

Engine references reach a release point in two ways:
- **Destructor.** `~EntrySpace` pushes its references to `EngineReleases`.
- **Retirement chain.** Retired slots, event batches, material references and node references go into `RetiredBatch`es. A batch is
  freed only when every publication up to it is gone, and its contents are then sent to `EngineReleases` (`SceneSet.cpp:1281-1342`).

`EngineReleases::Release` drains both queues on the render thread at Present (`Core/Engine/EngineReleases.h:33-37`).

So any `shared_ptr` copy of a publication, of its tables, of its candidates, or of a snapshot that holds them is a strong reference
to game objects. A copy of an old `SceneTables` also blocks every later retirement batch.

---

## 1. Scene publications and tables (Core/Scene)

| Structure | What it is | Created by (thread) | Strong holders: member, thread, how long, what drops it | Game refs it holds | Bounded by today | Unbounded growth |
|---|---|---|---|---|---|---|
| `ScenePublication` (`Model/Publication.h:44-72`) | The immutable scene a frame reads: tables, claims, lookups, catalog, sun/light candidates, category nodes, revision counts | `Coordinator::PublishScene` (`Coordinator/SceneSet.cpp:921-998`), scene lane | <ul><li>`SnapshotWork::publication` in `snapshotWorkSlot` (`Render/Impl/SnapshotBuilder.h:74-81`). Latest wins; a replaced one is dropped on the coordinator (`Render/Snapshot/Snapshot.cpp:162-165`).</li><li>The builder's `work`/`held` (`SnapshotBuilder.h:192-196`), until the build is done.</li><li>`SceneSnapshot::publication` (`SnapshotBuilder.h:86`).</li><li>The builder's `posted.publication` (`SnapshotBuilder.h:233-248`), until the next post (the PassThrough cache).</li><li>`FrameView::installed` (`FrameView/FrameView.h:382`), render thread, until a newer one is installed (`FrameView.cpp:144-145`). The old one goes to `Channels::retiredPublications` (`FrameView.cpp:206-211`), which the coordinator drops at its next pass (`SceneSet.cpp:1024-1027`).</li><li>Transitively, every `ExecutionOwners` list that holds the adopted snapshot (section 3).</li></ul> | Through its members: tables (and so the retirement chain), candidates (`NiPointer` roots), lookups (texture/buffer owners), categoryNodes (raw only) | The next publication, plus the GPU retirement of executions that hold the snapshot | A held `SceneSnapshot` or execution owner. A slow coordinator means `retiredPublications` is not drained: it is drained only by `RunSceneWork` (inference) |
| `SceneTables` pool `tablesPool` (`Coordinator/State/Publication.h:15`) | Pooled published table copies, brought up to date by log replay | `PublishTables` (`SceneSet.cpp:1988-2083`): reuses one with `use_count()==1` (`NearestPooledTables`, `SceneSet.cpp:1474-1496`), else `emplace_back` (`SceneSet.cpp:2006`); scene lane | <ul><li>The pool itself.</li><li>`publishedTables` (`State/Publication.h:30`).</li><li>`ScenePublication::tables`.</li><li>`DrawPublication::tables` (`SnapshotBuilder.h:24`) and `StreamViews::tables` (`Render/Payload/KeptStores.h:202`).</li><li>`FrameView::acceptedTables` (`FrameView.h:375`).</li><li>Readback `Armed`/`Batch`/`Writes::tables` and the main thread's `writtenTables` (`Render/Readback/Readbacks.h:190,217,229,264`).</li><li>`Cull.h:151` (`Direct::CpuFrameArm`).</li><li>Fades `CpuStageInput::tables` (`Features/Fades/FadeCpuStage.cpp:58`) and `FadeWriteBack::Stores::tables` (`Features/Fades/FadeScene.h:81`, set at `FadeWriteBack.cpp:111-124`).</li><li>Parity: `SetParityFrame::tables/payloadTables` (`Diagnostics/Draws/DrawsState.h:161-163`) and `CullCounters` `CpuSide::tables` (`Diagnostics/Draws/CullCounters.cpp:30`).</li><li>The tables spare's `source` (`SceneSet.cpp:1968`), until the next `FinishTablesSpare`.</li></ul> | <ul><li>`retirementHold` (`Model/Tables.h:540`): the chain node, and through it every later batch's references.</li><li>`geometryImports[].vertexOwner/indexOwner` (`Tables.h:72-78`): `GpuResources` leases that pin the game's `ID3D11Buffer`s by `com_ptr` (`Core/Gpu/GpuResources.h:49-54`).</li><li>Raw `objectGeometry` keys.</li></ul> | **The pool never shrinks** (no erase anywhere). `retirementHold` is reset only when an entry is found free at the next publication (`SceneSet.cpp:1484`). A free entry's `geometryImports` keep their last leases until that slot is replayed (`ForGeometryColumns`, `SceneSet.cpp:1428-1434`) | <ul><li>Grows whenever every entry is held. Each idle entry keeps stale vertex and index buffer leases (inference: this is the same mechanism as the EntrySpace spares, and it was not trimmed).</li><li>One held old entry keeps the chain node and so every newer batch (section 2).</li></ul> |
| `SetSnapshot` claims pool `claimsPool` (`SceneSet.cpp:184-194`) | The published claims (geometry to phases) | `PublishClaims` (`SceneSet.cpp:196+`), scene lane | <ul><li>The pool, `publicationClaims` (`State/Publication.h:36`) and `ScenePublication::claims`.</li><li>`FrameView::frameClaims`/`notedClaims` (`FrameView.h:390-392`).</li><li>`PassCapture::frameSet` (`Handoff/PassCapture.h:174-175,263`).</li><li>`PrimaryCull::frameSet` (`PrimaryCull.h:583`) and `FrameValues::rootViewClaims` (`Gpu/FrameValues.h:224`).</li><li>Diagnostics `RegistrationCensus` (`Diagnostics/Engine/RegistrationCensus.cpp:232`).</li></ul> | None (raw geometry keys) | Never shrinks | Grows while every entry is held. Memory only, no game refs |
| `Lookups` snapshot (`Model/Lookups.h:123-292`, `Snapshot()` 285-291) | Pipeline and material bindings parallel to the tables, chunk-shared copies | `PublishLookups` (`SceneSet.cpp:1229`), scene lane; `publishedLookups` (`State/Bindings.h:24`) | `publishedLookups`, `ScenePublication::lookups` and `FrameView::frameLookups` (`FrameView.h:373`). Builds read it by reference | <ul><li>`Material::bindingBlock` leads to `MaterialOwners` (`Lookups.h:161-166,179`), which holds texture imports (`GpuTextures` `Entry`, holding a `com_ptr` to the game SRV, `Gpu/GpuTextures.cpp:185-207`) and an ORG execution lease.</li><li>`shadowMaskOwner` (150), `sharedBindingBlock` (209), `projectedOwners` (213), `shadowTextureOwners` (238).</li></ul> | The next lookups change. Not pooled | Held with any publication or snapshot copy |
| `EntrySpace` sun/light candidates (`Scene/EntrySpace.h:27-64`) | The stable index space of the sun's and point lights' candidate entries, immutable once published | `EntrySpaceTable::Publish` (`Coordinator/EntrySpaceTable.cpp:179-289`), called from `UpdateSunCandidates`/`UpdateLightCandidates` (`Coordinator/SunCandidates.cpp:157-158, 252-253`); scene lane. A spare is caught up on the preparation pool (`DispatchSpare`, `EntrySpaceTable.cpp:339-386`) | See section 1a (many) | `held` (`EntrySpace.h:44`): a `NiPointer` to **every entry root**. For a shared model this is the prototype's root (`BSShaderProperty::fadeNode`), which keeps the model DB entry and its textures alive. Released through `EngineReleases` by `~EntrySpace` (`EntrySpace.h:33-37`) | <ul><li>Pool trimmed to `kFreeSpares = 2` idle entries after each publication (`EntrySpaceTable.cpp:291-314`, `EntrySpaceTable.h:121`).</li><li>Held entries are bounded only by their holders.</li></ul> | Grows by one whenever all are held (`EntrySpaceTable.cpp:185-200, 344-360`). Observed: about 240 in Whiterun while readers hold publications |
| `EntrySpaceChanges` and its `Run`s (`EntrySpace.h:71-86`) | Per-publication change runs, shared between snapshots | `Publish` (`EntrySpaceTable.cpp:238-270`) | `EntrySpace::changes` and `EntrySpaceTable::changes` (`EntrySpaceTable.h:72`) | None | `kMaxRuns`/`kMaxLogEntries` | No |
| `categoryNodes` set (`Publication.h:65`) | Raw `NiNode*` set for the point-light culls | `PublishScene` (`SceneSet.cpp:968-972`) | The publication and `FrameView::frameCategoryNodes` (`FrameView.h:369`) | None (raw) | Copied on change | No |

### 1a. `EntrySpace` holders (every strong reference)

| Holder (type::member, file:line) | Thread | How long | Dropped by | Across frames / GPU? |
|---|---|---|---|---|
| `EntrySpaceTable::pool[i].snapshot` (`EntrySpaceTable.h:61`) | scene lane | For ever while in the pool | `TrimPool` (idle entries beyond 2) | n/a (owner) |
| `EntrySpaceTable::last` (`EntrySpaceTable.h:73`) | scene lane | Until the next `Publish` | `Publish` (`EntrySpaceTable.cpp:283`) | One publication |
| `Pooled::source` of the dispatched spare (`EntrySpaceTable.h:65`, set at `.cpp:367`) | scene lane, read by a pool worker by address | Until the next `Publish` finishes the spare (`.cpp:202-204`) | `pooled.source.reset()` | One publication |
| `CoordinatorCandidates::sunCandidates/lightCandidates` (`State/Candidates.h:18,26`) | scene lane | Until republished | `SunCandidates.cpp:65,71,157,252` | Yes, while unchanged |
| `ScenePublication::sunCandidates/lightCandidates` (`Publication.h:60`) | any | The publication's lifetime | see section 1 | Yes |
| `SnapshotWork::sun/light` (`SnapshotBuilder.h:79`), copied from the coordinator (`Snapshot.cpp:159-160`) | coordinator to builder | Until taken or replaced | builder | No |
| Builder `posted.sun/light` (`SnapshotBuilder.h:236`) | builder | Until the next post | the next post (`Snapshot.cpp:~760-771`) | **Yes** (PassThrough cache) |
| `FrameView::frameSunCandidates/frameLightCandidates` (`FrameView.h:358`) | render | Until a newer publication is installed (`FrameView.cpp:163-164`) | install | Yes |
| `GetSunCandidates()` returns a **copy** (`FrameView.h:177,183`). Every reader below takes its own | | | | |
| `ShadowInputs::sunCandidates/lightCandidates` (`Render/Shadow/ShadowPayload.h:64-66`), set at `ShadowEpochs.cpp:834-835` and `ShadowRevision.cpp:301-302` | render, then builder | The shadow payload's life (pooled `shadowPayloadPool`, reset when idle, `ShadowEpochs.cpp:855-866`) | `ShadowPayload::Reset` | **Yes**, via `DrawPublication::shadow`, so also across GPU completion through the adopted snapshot |
| `SunExclusion::candidates` (`Features/Shadows/Local/SunExclusion.h:29`) and the **forward chain** `forwardOwned` (`SunExclusion.h:55`; `ExclusionBuild.cpp:192-218`) | builder, render | While any head of the chain is held. Each link holds the next link and its candidates | A head released by `SunExclusionSlot::BeginFrame/Take` (`SunExclusion.cpp:21-34`; `SunExclusion.h:88-92`), `ShadowPayload::Reset` (`ShadowPayload.h:164-165`), or the cache | **Yes**, and it grows: one link per build while a head is held |
| `SunExclusionCache::candidates` (`Features/Shadows/Local/ExclusionBuild.h:30`) | builder | Until the next exclusion build | the next build | Yes, and indefinitely while shadows are off (inference) |
| `PrimaryCull::Cut::candidates` (`Handoff/PrimaryCull.h:481`) | render (list jobs read it) | Until `SyncCut` sees a newer one (`PrimaryCull.cpp:393-398`) | `SyncCut` | **Yes. Not replaced at all** while `PrepareFrame` returns early: no list processes (`PrimaryCull.cpp:360-361`), `excludePrimaryEntries` off (`372-377`), or no candidates (`380-387`). An old snapshot is then held indefinitely (inference) |
| `PrimaryCull::ListFilter::candidates` (`PrimaryCull.h:311-317`); `listFilter` (atomic, cleared at Present, `PrimaryCull.h:219-226`), `listFilterBuilt` (354; republished while nothing moved), `listFilterKept` (355; `SceneLists.cpp:426,740`) | render, build job | `listFilterBuilt`/`listFilterKept` until a new filter version is built (`SceneLists.cpp:311`) | a new filter | **Yes**: indefinitely while no filter is rebuilt (sun off, preconditions false) (inference) |
| `PrimaryCull::PlanBatch::candidates` (`PrimaryCull.h:551-555`), made at `PrimaryCull.cpp:624-629` | pool worker, then render | Until `TakePlans` (render) | render thread (comment at `PrimaryCull.cpp:620`) | Usually one frame |
| `Direct::CpuFrameArm::candidates` (`Render/Direct/Cull.h:152`) | render, CPU cull jobs | The frame's batches | last batch | Within the frame (inference) |
| Readback `Armed::candidates` (`Readbacks.h:230`, set at `Visibility.cpp:1257`), then `Batch::candidates` (218, `Visibility.cpp:1127`), `Writes::candidates` (191, `Visibility.cpp:921-922`), `writtenCandidates` (266) | render, decode task, main thread | **Armed**: until the frame slot is harvested (`Visibility.cpp:1115-1139`); **Batch**: until decoded and released (`1145-1153`); **written**: until a *non-empty* write arrives (`Visibility.cpp:1348-1350`). Empty write lists are deleted (`915-917`) | as stated; `WriteAccumulated(!live)` resets (`1316-1327`) | **Yes, across GPU completion** (armed and harvested after the GPU) **and across frames** (written: indefinitely in a static view) |
| Visibility decode state `candidates` (`Visibility.cpp:47`) | decode task | Until a batch with newer candidates | the next decode | Yes |
| Fades `CpuStageInput::candidates` (`FadeCpuStage.cpp:59`) | the CPU frame's job | The job | job end | Within a frame (inference) |
| Diagnostics `EarlierSnapshot` (`Diagnostics/Scene/SceneCandidateParity.cpp:57-72`) | scene lane | **weak** (`lock()` only during a check) | n/a | No strong hold (a lock briefly makes one pooled entry busy) |

---

## 2. Retirement chain and event batches

| Structure | What it is | Created by | Strong holders | Game refs | Bounded by | Unbounded growth |
|---|---|---|---|---|---|---|
| `RetirementChain<RetiredBatch>` (`Core/Runtime/Retirement.h:21-106`; instance `Coordinator::retirement`, `Coordinator.h:548`) | A node per publication. A node holds the next newer one (`next`, `Retirement.h:48`). The last owner of a node returns its batch (`~Node`, 34-46); `Drain` on the owner takes it (74-93) | `Publish()` (`Retirement.h:65-71`) from `PublishTables` (`SceneSet.cpp:2071`) | `SceneTables::retirementHold` of each published pool entry (`Tables.h:540`), and through the node's `next` every older node keeps every newer one alive | Through `RetiredBatch` (below) | **Publication order**: a node is freed only when every node before it is freed | **Yes: one held old `SceneTables` blocks every later batch.** The report "retirement (6e E3 ...) snapshots held past the pool (publication:holders)" (`Coordinator.h:343-361`) lists them |
| `RetiredBatch` (`Coordinator.h:539-547`) | What a publication may still name: retired slots, `imports` (buffer leases), `materials` (`MaterialReference`, engine material counts), `references` (`NiPointer<NiRefObject>`), `events` (`shared_ptr<EventBatch>`) | `RetireIntoChain` (`SceneSet.cpp:1268-1279`), `HandBack` (`Coordinator.h:1483-1495`), `ApplyEvents` (`ApplyEvents.cpp:299-300`) | The chain node | All four kinds | As above | As above |
| Draining it, `RecycleRetired` (`SceneSet.cpp:1281-1342`) | Slots back to free lists. `imports` go to `retiredImports`, then the deltas, then the frame, then the epochs' `ExecutionOwners` (`Uploads.cpp:540`, `ShadowEpochs.cpp:548`). Materials, references and events go to `EngineReleases` (1335-1341) | scene lane, each pass | | | | |
| `EventBatch` (`Coordinator/Internal.h:276`), with `SceneTracker::Event` (`Engine/Capture/SceneTracker.h:34-72`) | A drained batch of tracker events: `node` (an attached subtree root, `NiPointer`), `pins` (`NiPointer`s), captures | `CollectEvents` (`Coordinator.h:868`); events pushed by engine hooks on any thread | The coordinator during `ApplyEvents`, then `RetiredBatch::events` | **Detached subtrees**, attached roots, pins | Retirement order | Yes, with the chain |
| `Coordinator::materialOwners` (`Coordinator.h:533`), type `MaterialReference` (`505-530`) | Per material slot: an engine count on the `BSShaderMaterial` | the walk and joins | The coordinator; retired through `RetiredBatch::materials` | Material DB counts | Retirement order | With the chain |
| `HeldMaterial` / `MaterialPort::HeldSnapshot` (`Coordinator/State/Materials.h:18-25`; `Shading/MaterialPort.h:95-104`) | A capture of a material, holding one material count and `com_ptr`s to its SRVs (`views`, 103) | `MaterialPort::PushCapture` (`MaterialPort.cpp:55-72`), any thread; then the `captures` queue (`MaterialPort.h:108`) | `Coordinator::materialSnapshots` (newest per material) | Material count, SRVs | Aged out after `kMaterialSnapshotFrames = 8` unused frames (`Materials.h:24`). A stale capture is handed back at once (`ApplyEvents.cpp:138-141`) | No (frame-aged). Note: `HandBackHeld` (`ApplyEvents.cpp:120-127`) routes the material count to `EngineReleases`, but the `views` `com_ptr`s drop on the scene lane |

---

## 3. Render-side snapshots and frame views (Core/Render, Core/Scene/FrameView)

| Structure | What it is | Created by | Strong holders | Game refs | Bounded by | Unbounded growth |
|---|---|---|---|---|---|---|
| `SceneSnapshot` (`Render/Impl/SnapshotBuilder.h:83-100`) | Publication, draws, payload, revision lease, view batches | Snapshot builder (`SnapshotPass`, `Snapshot.cpp:501+`; post at 771), a pool thread | <ul><li>`snapshotSlot` (LatestSlot; an unconsumed one is freed by the next post).</li><li>`adoptedSnapshot` (`SnapshotBuilder.h:146`), render thread, until the next adoption (`Snapshot.cpp:842-843`). The replaced or stale one goes to `retiredSnapshots` (`SnapshotBuilder.h:177`; `Snapshot.cpp:801,843`), dropped by the builder at its next pass (`Snapshot.cpp:505`).</li><li>**Every execution's `ExecutionOwners`** (`MainEpochs.cpp:312`; `ViewEpoch.cpp:476`), until the GPU retires that execution. Released on ORG's cleanup queue host thread (`MainEpochs.cpp:309-311`).</li></ul> | Everything the publication holds, plus payload binding owners | Next adoption + GPU retirement of each execution | `retiredSnapshots` is drained only when the builder runs (inference: a sleeping builder keeps replaced snapshots until it wakes) |
| `DrawPublication` (`SnapshotBuilder.h:22-28`) | tables, streams, main payloads, shadow payload | `RunAhead` (`Snapshot.cpp:333+`), builder | `SceneSnapshot::draws`, `SnapshotBuilder::draws` (197), `posted.draws` (237), `Impl::installedDraws` (`Impl.h:163`) | tables, which carry the chain; shadow payload candidates and exclusions | Next build / adoption | With its holders |
| `MainPayload` pool `payloadPool` (`SnapshotBuilder.h:68`; `AcquirePayload`, `Impl.h:233-245`) and `ShadowPayload` pool (`ShadowEpochs.cpp:855-866`) | Pooled build outputs | builder | `DrawPublication`; `ExecutionOwners::Add(ahead)` (`MainEpochs.cpp:308`) | `bindingOwners` (`Payload/MainPayload.h:105`), `streams` (tables); shadow: candidates, exclusions | Idle entries are `Reset()` when found idle (comment `Impl.h:235-236`: an idle one would otherwise keep the chain) | The pools never shrink, but idle entries are emptied |
| `ScenePayload` / `PayloadPools` installed (`Payload/PayloadVersions.cpp:89`, `Install` 566) | The frame's payload versions | builder (`Finish`) | `SceneSnapshot::payload`, `installedPayload` (`SnapshotBuilder.h:149`), `PayloadPools::installed` | binding owners | Next install, then `BindInstalled(SubmittedRetirementPoint())` (`Snapshot.cpp:822`) | No (GPU-point bound) |
| `ExecutionOwners` (`Render/Epochs/ExecutionOwners.h:16-42`) | Per-submission keep-alive list on the graph's cleanup queue | `Make()`, render thread | ORG's cleanup queue, until the execution's GPU point | The whole adopted snapshot (so tables, the chain, candidate roots), payload bindings, frame textures, retired imports | **GPU completion** (2-3 frames) | No, unless the GPU or the cleanup queue stalls |
| `FrameView` frame copies (`FrameView/FrameView.h:342-392`) | The render thread's one installed publication and its fields | `HandOverAtFrameStart` (`FrameView.cpp:62-168`) | `installed`, `acceptedTables`, `frameLookups`, `frameCatalog`, `frameSun/LightCandidates`, `frameCategoryNodes`, `frameClaims`, `notedClaims` | Via the publication | **Single slot**: replaced on install | No. This is already the BasicRenderer-like slot. The leaks are the copies that readers take from it |
| `FrameView::framePlans/frameShading/frameComponentSeeds/frameRetiredImports` (`FrameView.h:364-367`) | Deltas taken from the log | `HandOverAtFrameStart` (`FrameView.cpp:79-106`) | render thread until the frame consumes them (`TakePlacementPlans`, `TakeRetiredImports`, ...) | `PlacementPlan::Item` `NiPointer<BSGeometry>`/`listedRoot`, `Root::root` (`Publication.h:89-90,123`); `ShadingItem::property` (152); `FadeSeedItem::node` (165); import leases | One frame | No |
| `PublicationLog<PublicationDeltas>` (`Channels.h:193`; `Runtime/PublicationLog.h`) | Every publication's deltas, in order | `PublishScene` `Append` (`SceneSet.cpp:995`) | Log nodes, freed by the render thread behind `notesCursor` (`FrameView.cpp:140`; `PublicationLog.h:64-73`) | The deltas above, until moved out at frame start. A node past the installed sequence keeps its references | Install progress: deltas are taken only up to the installed/adopted sequence (`FrameView.cpp:74,79`) | **Yes during an adoption stall** (stale snapshots, builder behind): every new publication's plans, shading, seed items and retired imports stay on the log (inference) |
| `CategoryCapture` (`Scene/Channels.h:66-86`) | Category nodes and portal roots as the render thread found them | `FrameView::CaptureCategories` (`FrameView/FrameRequests.cpp:642-676`), render | <ul><li>`categoryCapture` (newest, `FrameView.h:342`).</li><li>`categoryCapturesHeld` deque (344): popped once the coordinator applied a newer generation, keeping at least one (`FrameRequests.cpp:654-655`).</li><li>`categoryPosted` LatestSlot (`Channels.h:176`; `FrameRequests.cpp:127`).</li><li>Coordinator `categoryTaken` (`State/Categories.h:13`).</li></ul> | `held` (every category node), `roots[].root`, **`pins`: every node and geometry under new category nodes** (`Channels.h:71-82`) | The next capture, which happens only on a cell attach/detach or a signature move | The newest capture's pins last until the next cell change (inference: after leaving a cell, the last capture keeps the pins it made) |
| `FadeNodeRequest`, `MaterialRequest`, `SwitchCatchUp`, `mirrorCaptureRequests` (`Channels.h:94-154,195-199`) | Requests to the render thread, each holding its node or property | coordinator | The queues until served | `NiPointer`s | Served next frame start | No (inference) |
| `ProjectedCapture` (`Coordinator/SharedBindings.h:50-53,88-89`) | The 4 projected SRVs | render thread posts | `projectedPosted`, `projectedSeen` | `com_ptr` SRVs | The next capture | No |

---

## 4. PrimaryCull and the scene lists (Core/Handoff)

| Structure | Strong holders, lifetime | Game refs | Bound | Growth |
|---|---|---|---|---|
| `Cut::candidates`, `ListFilter`, `PlanBatch` | See 1a | EntrySpace roots | Next snapshot *when the cut runs* | Held indefinitely while the cut is off (inference) |
| `listGraveyard` (`PrimaryCull.h:395`) | Filled by the build job (`SceneLists.cpp:483-601`), cleared at `EndFrame` (Present, `PrimaryCull.h:219-226`) | Scene-list `NiPointer`s (possibly a detached cell's last reference) | One frame | No |
| `listEvents` `ListEvent::held` (`PrimaryCull.h:379-386`) | Attach roots until the build job applies or drops them | `NiPointer` | The next build | No (inference) |
| `listSentinels` (`PrimaryCull.h:375`) | Session | DCLF-made nodes | n/a | No |

---

## 5. GPU texture and buffer owners (Core/Gpu, Coordinator bindings)

| Structure | What | Holders | Game refs | Bound | Growth |
|---|---|---|---|---|---|
| `GpuTextures::Impl::Entry` (`Gpu/GpuTextures.cpp:185-207`) | An imported game SRV: `com_ptr<ID3D11ShaderResourceView>`, BasicRHI image, descriptor | <ul><li>`Binding::owner`.</li><li>`MaterialOwners` (sealed into an ORG `ExecutionResourceLease` by `Seal`, `GpuTextures.cpp:650-656`).</li><li>`Lookups` entries, so every publication and execution.</li><li>The registry holds `live` **weakly** (165).</li></ul> | SRV and its texture | The last owner. The release runs on the cleanup queue | **Rejected entries are pinned strongly** in `Registry::Slot::rejected` (`GpuTextures.cpp:166`; set at 328, 488, 592) "for this device generation", and keep the `view` `com_ptr` (465, 576). So every rejected game texture (unsupported view, non-GENERAL layout, failed import) stays alive until device teardown |
| `MaterialBindings::cache` / `sealed` (`Coordinator/MaterialBindings.h:72-74`) | Answer caches | **weak** | | | No |
| `MaterialBindings::arrived` (77) | One drain's answers | Strong for one pass | SRV entries | One pass | No |
| `SharedBindings` shadow textures: `Lookups::shadowTextureOwners` (`Lookups.h:238`; `SharedBindings.cpp:205-211, 365-375`) | Alpha-tested casters' diffuse imports | Lookups | SRV entries | Removed when no caster names the view (`shadowRemoved`) | No (inference) |
| `GpuResources` `LeasedBuffer::owner` (`Gpu/GpuResources.h:49-54`); registry `Entry::owner` weak (111) | A stable game `ID3D11Buffer` | `SceneTables::geometryImports` (**every pooled table copy**), `RetiredBatch::imports`, `retiredImports`, then the frame, then `ExecutionOwners` | `com_ptr<ID3D11Buffer>` | Last lease | **Idle tables-pool entries keep stale leases** (section 1) |
| `DrawPipelines` versions (`Gpu/Pipelines/DrawPipelinesImpl.h:511,539-545`) | Pipeline catalog versions, swept by `use_count()==1` | the catalog's holders | None | Sweep | No |

---

## 6. Feature components' owners (Features/*)

| Component | Structure | Holders, lifetime | Game refs | Release path | Notes |
|---|---|---|---|---|---|
| Fades | `FadeRootState::owners` (`Features/Fades/FadeRoots.cpp:65-72`) | Coordinator, while a root is listed (`ListNewRoot`, 210-213) | `NiPointer<NiAVObject>` per fade root | `HandBack` (176, 371), then the retirement chain, then `EngineReleases` | Bound by retirement order |
| Fades | `FadeWriteBack::Stores::tables` (`FadeScene.h:68-84`; `ready` slot) | Task, then main thread; latest wins | tables (the chain) | Main thread consumes it | A copy of an old tables while not consumed (inference) |
| Fades | `FadeSeeds` `ReleaseLater` (`FadeSeeds.cpp:25-39`) | seeds | `NiPointer` | `EngineReleases` | |
| TreeWind | `TreeComponent::owners` (`Features/TreeWind/TreeWindTables.cpp:57,126-132,202-205,264`) | Coordinator while listed | `NiPointer` | `HandBack`, then the chain | Same as Fades |
| TreeWind | `ownedTrees`, `written` (`TreeAnimation.cpp:60-73`; `SetOwned` 203-214) | Render/main thread while stood in | `NiPointer` | `EngineReleases` (61-65) | Bound by the owned set |
| TreeWind | `detached` (`TreeAnimation.cpp:49-55`) | The animation list's own references, taken by hand, "never destroyed" | Raw, with refcounts moved (`DecRefCount` at 142, 161) | Put back or released | Process-lifetime map. Entries are bounded by the owned set (inference) |
| Faces | `FaceSnapshots::Impl::Record` (`Features/Faces/FaceSnapshots.cpp:42-60`) | Table of records; retired (195-197) and freed after a morph stage completes (200-220) | `head` `NiPointer<BSFaceGenNiNode>`, `shapes[].shape` `NiPointer<BSDynamicTriShape>` | `released` (225), then `FaceShapes.cpp:185-198` `HandBack`, then the chain | Stage-completion bound, then retirement order |
| Lod | gates and segments (`Features/Lod/*`) | Coordinator state, keys only | No `NiPointer` found | n/a | Deltas travel as `shared_ptr<const void>` on the log (`Publication.h:210`) |
| FrameValues | kept movers and roots (`Gpu/FrameValues.cpp:439-450`), `owedNodes` (497) | render thread, while in the kept placement set | `NiPointer` | `EngineReleases` (304-309, 770) | Bound by plans' "left" deltas |

---

## 7. Reader patterns

"Across frames" means the copy outlives the frame that took it. "Across GPU" means it is kept until an asynchronous GPU completion.

| Consumer | What it copies | From | How long | Across frames | Across GPU |
|---|---|---|---|---|---|
| Render thread, `FrameView` | publication and its fields | adopted snapshot | until a newer one is installed | yes (single slot) | no |
| Render thread, `IndirectDraws` | `adoptedSnapshot`, `installedDraws`, `installedPayload` | snapshot slot | until a newer one is adopted | yes (single slot) | no |
| **GPU epochs** (main, view, shadow) | the whole `adoptedSnapshot`, payloads, retired imports, into `ExecutionOwners` | render | until the GPU retires the execution | yes, 2-3 frames | **yes** |
| Snapshot builder | `work`, `held`, `posted` (publication, sun, light, draws, payload, revision), `retiredSnapshots` | coordinator, render | until the next post or the next builder pass | **yes** (`posted`) | no |
| Shadow build | `ShadowInputs::sun/lightCandidates`, `SunExclusion` chains, `SunExclusionCache::candidates` | `FrameView` | payload life / next build / while a chain head lives | **yes** | yes (via the snapshot's draws) |
| Shadow slots | `SunExclusionSlot::pending`, `ParabolicExclusionSlot` | builder | until taken or forwarded | yes | no |
| **PrimaryCull** | `Cut::candidates`, `ListFilter` (built/kept/frame), `PlanBatch::candidates`, `frameSet` | `FrameView` | until a new snapshot or filter **while the cut runs**; otherwise indefinitely | **yes** | no |
| **Visibility readback** | `Armed/Batch/Writes::tables+candidates`, main thread `writtenTables/writtenCandidates`, decode state `candidates` | `FrameView` | armed until harvest (after the GPU); written until the next *non-empty* write | **yes** | **yes** |
| Fades CPU stage / write-back | tables, candidates | `FrameView` | job / until the main thread takes `ready` | write-back: yes | CPU-stage job: possibly (inference) |
| Pool workers (spares) | raw pointers to the target and source; the lane holds `source` | coordinator | one publication | no | no |
| Pool workers (cut plans, decode, fades) | `PlanBatch`, readback batches, `Stores` | render | until returned to render/main | sometimes | readback: yes |
| Diagnostics/parity | `SetParityFrame::tables/payloadTables` (readback a few frames later), `CullCounters::CpuSide::tables`, `RegistrationCensus::set`, `SceneCandidateParity` (weak) | `FrameView` | parity frames | **yes** | **yes** (SetParity staging readback) |
| Coordinator reports | `TakeRetirementReport` inspects only `use_count` | | | | |

---

## 8. BasicRenderer's handoff, and how it maps

Paths below are under `C:\Users\matth\source\repos\SARP\BasicRenderer\`. The renderer is in `BasicRenderer\`; ORG is in `OpenRenderGraph\`.

**What it actually does.** The maintainer's summary ("holds only a pointer to the current snapshot, released when a new one comes")
fits the *mailbox*, not the renderer's long-lived state. There are two layers.

1. **The scene snapshot is a delta, applied and then dropped.**
   - `SceneFrameSnapshot` (`BasicRenderer\include\BasicRenderer\Scene\SceneFrameSnapshot.h:49-78`) carries changed and removed lists
     and the alive-ID sets.
   - It also carries `retainedMeshArtifacts` (`:57`), strong `shared_ptr<Mesh>`s that keep meshes alive only "through delayed
     ingestion".
   - **Producer.** `Renderer::ScheduleSceneUpdateTask` (`BasicRenderer\src\Runtime\Renderer\RendererSceneIngestion.cpp:385-458`) runs
     on a worker, with one task in flight (`m_sceneTaskInFlight.exchange(true)`, `:387`). It publishes under a mutex into a single
     slot (`m_completedSceneSnapshot = std::move(snapshot)`, `:449-450`), then sets the completed flag (`:455-456`). An unconsumed
     snapshot is overwritten: latest wins.
   - **Consumer.** `Renderer::CommitCompletedSceneSnapshot` (`:349-383`), on the update thread, `std::exchange`s the slot to null
     (`:360`) and calls `IngestSnapshot` (`:373`). The snapshot is a local and dies at function exit.
   - Ingestion copies what it needs into renderer-owned state: `m_retainedSourceMeshes = snapshot.retainedMeshArtifacts`
     (`BasicRenderer\src\Scene\Ingestion\SceneRenderBridge.cpp:819-822`), and ECS rows materialised and destroyed per delta.
   - **Result: no reader ever holds a scene snapshot across frames.**
2. **GPU-facing state is held per frame slot.** `PublishedRendererState`/`RendererStatePublisher`
   (`BasicRenderer\include\BasicRenderer\Streaming\PublishedRendererState.h:142-352`):
   - `m_frameStates[frameSlot]` keeps one state per frame-in-flight slot.
   - `Commit(frameSlot)` "must be called after the frame slot fence has completed" (`:312-314`).
   - Commit moves that slot's old state into `retiredStates` (`BasicRenderer\src\Runtime\Publication\PublishedRendererState.cpp:622-626`),
     swaps in the candidate (`:632-636`, `:788-789`), and stores `m_frameStates[frameSlot] = m_active` (`:794`).
   - Retired states are freed off-thread in a deferred task.
3. **GPU-in-flight protection is fence-gated deferred deletion, not reader references.**
   - `WaitForFrame(m_preparationFrameIndex)` (`BasicRenderer\src\Runtime\Renderer\RendererFrameUpdate.cpp:277-279`) is followed by:
     - `ObjectManager::PublishDeferredRetireCompletedFrame(total - (framesInFlight+1))` (`:280-286`)
     - descriptor deferred releases (`:287`)
     - ECS deferred world ops (`:288`)
     - upload-page releases (`:295-299`)
   - `DeletionManager` (`OpenRenderGraph\src\include\Managers\Singletons\DeletionManager.h:44-99`) is a ring of
     `numFramesInFlight + 1` queues. `MarkForDelete` pushes into `[0]`; `Rotate()` ages each queue and returns the oldest for
     destruction.
   - GPU backings enqueue themselves from their destructors (e.g. `OpenRenderGraph\src\Resources\GPUBacking\GPUTextureBacking.cpp:48-52`).
     So when the last CPU owner drops a mesh or texture, the GPU object is still deferred by the fence ring.

**Invariants worth taking:**
- One producer and one pending slot.
- The consumer empties the slot (exchange) and does not keep the snapshot.
- Producer-side engine/asset ownership is moved into consumer-owned state at a known point (ingest).
- GPU lifetime comes from frame-slot fences plus a deletion ring, never from snapshot references.
- The retained per-slot state is exactly `framesInFlight` deep and is released only after that slot's fence.

**Mapping onto DCLF:**

| BasicRenderer | DCLF today | Mapping |
|---|---|---|
| One pending mailbox slot | `snapshotWorkSlot`, `snapshotSlot`, `LatestSlot` (`Runtime/LatestSlot.h:16-36`) | Already the same |
| Consumer exchanges and keeps only the current one | `FrameView::installed` / `adoptedSnapshot` are single slots, but `GetSunCandidates()`, `AcceptedTables()` and `FrameClaims()` hand out `shared_ptr` copies that readers keep | Give readers a borrowed `const&` or an index valid for the frame. Keep the only strong references in a fixed table owned by the render thread: `current` plus `inFlight[framesInFlight]` |
| Per-frame-slot retained state, released after that slot's fence | `ExecutionOwners` holds the whole adopted snapshot until ORG's cleanup queue retires each execution | Store a snapshot *id* per frame slot. Drop the slot's reference at the fence wait (render thread). Release the snapshot's game references only when no slot names it |
| Producer moves asset ownership into consumer state at ingest | Pooled `EntrySpace` and `SceneTables` carry `NiPointer`s and leases inside every pooled copy; the chain frees in publication order | Keep engine references in one producer-owned (coordinator) owner table keyed by slot or entry. Snapshots name entries by index. The producer releases an owner at a known point: "no frame slot and no current snapshot names a generation <= G" |
| Fence-gated deletion ring | `RetirementChain` (ownership order, held by any reader), `EngineReleases` (Present) | Replace chain-by-reference with a retire-at-frame-number ring: retire with the publication sequence, free once the oldest live sequence (current plus in-flight slots) is past it, then push to `EngineReleases` |

---

## 9. Lifetime hazards found (worst first)

1. **Any held old `SceneTables` stalls the whole retirement chain.** `retirementHold` (`Tables.h:540`) plus `Node::next`
   (`Retirement.h:48`) frees in publication order. Detached subtrees (event batches), material counts, node references and buffer
   leases after that point all stay alive. Holders that keep tables across frames:
   - **visibility readback `writtenTables`** (`Readbacks.h:264`), kept until the next *non-empty* write, so indefinitely in a static
     view (`Visibility.cpp:915-917,1348`)
   - `Armed::tables`, if a slot is armed but not harvested (`Visibility.cpp:1117-1139`)
   - the builder's `posted` cache (`SnapshotBuilder.h:233-248`)
   - `FadeWriteBack::Stores` (`FadeScene.h:81`)
   - parity `SetParityFrame` (`DrawsState.h:161-163`)
2. **`EntrySpace` readers keep candidate snapshots, and their `NiPointer` roots, alive indefinitely in several paths:**
   - `PrimaryCull::Cut::candidates` and `listFilterBuilt`/`listFilterKept` are not replaced while the cut is inactive
     (`PrimaryCull.cpp:360-387`) (inference)
   - readback `writtenCandidates` (`Readbacks.h:266`), as in hazard 1
   - `SunExclusionCache::candidates` while shadows are off (inference)

   The pool grows by one whenever all entries are held (`EntrySpaceTable.cpp:197-200, 357-360`). Each held snapshot pins every
   listed entry root. For shared models that root is the prototype root, which keeps the model DB entry and about 2 GB of textures.
3. **`SunExclusion` forward chains grow without bound while a head is held.** Each link (`forwardOwned`, `SunExclusion.h:55`;
   `ExclusionBuild.cpp:192-218`) holds the next exclusion and its `candidates`. One stale head, for example in a pooled
   `ShadowPayload` inside a held snapshot or the slot's `pending`, pins one `EntrySpace` per later build.
4. **`GpuTextures` pins every rejected game texture for the device generation** (`Registry::Slot::rejected`,
   `GpuTextures.cpp:166,328,488,592`). The pinned entry keeps its `com_ptr<ID3D11ShaderResourceView>` (`:187`). This is retained
   VRAM regardless of scene state.
5. **The `SceneTables` pool never shrinks, and idle entries keep stale `geometryImports` leases** (game `ID3D11Buffer` `com_ptr`s,
   `Tables.h:72-78`; `GpuResources.h:49-54`) until their slots are replayed. This is the same retention shape as the `EntrySpace`
   spares before `TrimPool`. (inference: not measured; the pool size is in `TakeTablesPublication().pool`)
6. **`ExecutionOwners` hold the whole adopted snapshot** (`MainEpochs.cpp:312`, `ViewEpoch.cpp:476`). This is correctly
   GPU-bounded, but it multiplies holders of tables and candidates by the number of executions in flight. Any stall in ORG's cleanup
   queue or a GPU hang extends every lifetime above.
7. **`PublicationLog` holds deltas past the installed sequence** (plans, shading items, seed items with `NiPointer`s; retired import
   leases) for as long as adoption stalls (`FrameView.cpp:74-106,140`) (inference).
8. **The newest `CategoryCapture`'s `pins`** (`Channels.h:82`) live until the next category capture, which needs a cell change or
   signature move, and `categoryCapturesHeld` always keeps one (`FrameRequests.cpp:654`).
9. **Drop-thread drift.** `retiredSnapshots` is drained only when the builder runs (`Snapshot.cpp:505`), and `retiredPublications`
   only when the coordinator runs. The last owner of a snapshot can be ORG's cleanup thread (`ExecutionOwners`).
   `HeldSnapshot::views` drop on the scene lane (`ApplyEvents.cpp:120-127`). These are correct today only because every game
   reference in them goes through `EngineReleases`. They show that "who drops it" is not fixed.

## 10. Constraints any redesign must keep

- **GPU frames in flight read a publication.** Draws, payloads, latches, descriptor indices and the rows' addresses must stay valid
  until each execution's GPU point. Today `ExecutionOwners` does this. A replacement needs a per-frame-slot (or per-execution)
  retirement point, fed by `SubmittedRetirementPoint()` or the graph host's slot fence.
- **Threads.**
  - The coordinator (scene lane) is the only writer of tables, pools and the chain.
  - Pool workers read spares and sources by raw address while the lane holds them (`EntrySpaceTable.cpp:375`,
    `SceneSet.cpp:1975`).
  - The snapshot builder runs on a pool thread.
  - The render thread installs and adopts.
  - The main thread runs `WriteAccumulated` and the fade stores.
  - The decode, fades and cut-plan tasks are asynchronous and return to render or main.
- **Engine objects are released only on the render thread** (`EngineReleases::Release` at Present), and material counts go through
  `MaterialReference` (the engine's DB lock). Any new owner table must route its releases there.
- **Engine-read windows.** Readers that dereference roots (the cut's stand-in, readback writes, fade stores) do so on engine threads
  inside the read window. They rely on the snapshot holding the node. Without reader-held references they need either a
  producer-guaranteed generation fence ("nodes of generation G live until frame F") or keys checked against a live owner table.
- **Parity and diagnostics readers** (`SceneCandidateParity`, `CheckSnapshot`, `CheckPacedPublication`, `SetParityFrame`,
  `CullCounters`) compare across generations and read back after the GPU. They must either use weak handles plus generations (as
  `EarlierSnapshot` already does) or hold a counted slot in the same bounded table.
- **Skipped publications lose nothing.** The deltas on the log must survive a publication the builder skipped, and placement plans
  must apply in order (`Publication.h:81-84`).

## 11. Direction (brief)

- One owner table per producer for engine references and leases (entry roots, fade/tree owners, buffer and texture leases), indexed
  by slot.
- Snapshots carry indices and generations, never `NiPointer`s or `shared_ptr` owners.
- The render thread keeps `current` plus `inFlight[framesInFlight]` snapshot ids. Readers get borrowed references for the duration
  of a frame or job.
- The producer frees an owner once the oldest live id (current plus in-flight, plus a bounded parity slot) is past its retire
  sequence, replacing the reference-ordered `RetirementChain` with a sequence-gated ring.
- Old snapshots can then be cleared (their pools reset) directly at the fence wait.

## 12. Root cause of the measured retention, and the redesign

**Measured.** With `TrimPool` in place, a dump after the COC tour (Whiterun, then the inn) showed 4,351 detached prototypes with one
reference and no model DB entry. DCLF-off has 854. One stale sun `EntrySpace`, generation 933 (Whiterun's candidates: 5,409 slots,
5,406 held), named 4,222 of them through `held`. The current generation, 940, named 552. Generation 933's holders were a
`SunExclusion` (`_Ref_count_obj2<...::SunExclusion>`), a `PrimaryCull::ListFilter` (`_Ref_count_obj2<...::ListFilter>`), and two
anonymous holders. These are the readers in hazard 2: their consumers are idle in an interior, so they never rebuild and never let go.
The model DB requeues an entry whose root is down to one reference every 7 frames, and the texture DB releases at refcount 1, so
everything those prototypes held (about 10k shapes and about 2 GB of textures) stays alive only because of DCLF's last reference.

**The redesign** separates game references from snapshots. Readers stay free to keep what they need, including GPU readbacks, which
must never be skipped:
- Snapshot types hold no game references: no `NiPointer`, `com_ptr`, lease, material count or event batch. Engine objects appear as
  opaque `EntryKey`s with indices and versions.
- The coordinator's owner tables, indexed by entry or slot, hold every game reference. Untracking or replacing an entry retires its
  owner, stamped with the first publication that no longer names it.
- Release depends only on the scene, never on readers:
  - CPU references are freed once the render thread has installed a publication at or past the stamp, then released at Present
    (`EngineReleases`).
  - GPU resources go through a fence ring, as in BasicRenderer's `DeletionManager`. This replaces `RetirementChain`.
- Code that dereferences an engine object from a snapshot resolves the key against the installed publication. A mismatch means the
  object left the scene, and only that write is dropped.
- Caches keep `SnapshotKey`s and catch up through the change runs (`ChangedEntriesSince`). Readers that need a past generation's CPU
  data hold a lease on a pooled snapshot. A lease costs DCLF heap only.

## Verification of the doc itself

- Spot-checked the BasicRenderer citations (`RendererSceneIngestion.cpp:349-383, 445-457`; `PublishedRendererState.h:310-320`,
  `.cpp:618-636, 786-794`; `SceneFrameSnapshot.h:49-60`; `RendererFrameUpdate.cpp:277-300`; `DeletionManager.h:44-99`).
- DCLF citations were read directly during the survey.
