# Historical synchronization branch: CPU and upscaling backport

This branch keeps CS 16586f1b, BasicRHI 33b442f, ORG f5f3546 and DXVK 4cfa768 as the synchronization baseline. The saved investigation remains on `investigation/resource-scoped-sync-20260926` in all four repositories.

## Included

- CS upscaling interop from cd5c4174 (unchanged in the saved investigation): render-thread command buffers enter DXVK's existing ordered command stream rather than flushing and waiting for its worker. A completion timeline retains ring slots and deferred resources until GPU completion. Other-thread submissions retain the ordered direct-submit behavior. `CS_UPSCALE_SUBMIT=direct` in CommunityShaders-DCLF.ini selects the previous submission path for comparisons.
- The associated eight-slot profiler/query rings and nonblocking reuse of pending profiler slots. Unfinished disjoint queries are not restarted.
- ORG ticket wake counter (including terminal worker failure), and recording the host-frame ID from the actual replacement ticket.
- Staged-copy overlap lookup indexed by backing identity/generation, adapted from the saved BufferCopyDependencies implementation. This backport retains the historical full copy barrier and its clearing semantics; it does not install a boundary ledger.
- CS/ORG CPU Tracy regions for epoch entry, worker work/waits, staging/commit, host preparation and slot waits, stale-ticket replacement, immediate/prepared upload recording, native Present, and upload copy/byte plots.

## Deliberately absent

The saved branch's geometric manifest growth and endpoint-sweep boundary reduction optimize code introduced by the scoped-sync migration. This baseline has neither manifests nor that reducer; adding them would reintroduce the migration's overhead. Registration caches, immutable-resource exclusion bookkeeping and native hazard ledgers are also absent. BasicRHI and DXVK remain at their historical revisions; the existing DXVK enqueue API already supports the upscaling change.

Historical ORG entry/exit and upload barriers remain in place. DGC, shader/HZB behavior, queue topology and rendering settings are unchanged. Payload staging was already on the DCLF worker before this backport.

## Validation

Build and test artifacts are in `build/codex-native-gaps/`:

```
cmake --build --preset Dev --target CommunityShaders
cmake --build build/codex-org-tests --config Release
ctest --test-dir build/codex-org-tests -C Release --output-on-failure
cmake --build build/codex-interop-tests --config Release --target OrgDxvkInteropTest
ctest --test-dir build/codex-interop-tests -C Release --output-on-failure
```

The Vulkan host test additionally runs asynchronously and verifies staged overlapping writes, disjoint patches, imported-buffer replacement and graph rebuild through GPU readbacks. Unavailable GPU execution returns failure, and both host variants have a timeout.

### Backport results (2026-09-27)

- Final Dev build and packaging succeeded. ORG: 9/9 tests passed; DXVK interop: 2/2 passed. Both Vulkan host variants executed GPU work (`ok (sync, locks=14)` / `ok (async, locks=14)`), rather than skipping.
- Deployed CommunityShaders.dll SHA256: `4E8DCCEDEFA5BA66B6744154BEE7E3F6145DE994A0A7F2C70FBA3FD12B9321FA`; package/deployment hashes match. The preceding binary/PDB are retained under `build/codex-native-gaps/pre-port-deployment/`.
- MO2 Riverwood smoke uses the existing frame-based DCLF toggle mechanism (300 off, 1500 on, 2700 off, 3900 on). Initialization confirms `Ring submissions from the render thread go through DXVK's command stream (no wait)`.
- A 15-second Tracy sample (`ported-cpu.tracy`, `ported-cpu.csv`) contains 1,204 upscaling calls, mean 0.164 ms, maximum 0.479 ms. Pending-upload recording averages 42.9 us over 5,285 calls; staged recording averages 37.0 us. These are elapsed CPU-region timings across mixed DCLF modes, not a matched performance acceptance run. ORG ticket waits remain visible separately.
- Warm smoke windows are approximately 84 fps DCLF off and 79 fps on. There is no claim that these establish whole-frame GPU or tail-latency acceptance. Frame generation is disabled in this workload.
- The existing missing `enbseries/enbeffect.fx` startup error also occurred before the backport; it is unrelated. No new submission/ring failures were observed.
- The 100-second runner completed all four toggles and then stopped the game as scheduled. Original diagnostic settings were restored. After the final re-enable, the existing CPU slot validator reported one stale material slot (`Farmhouse05:14`, frame 1466) and neutralized the object's record. This is an unresolved smoke-validation caveat, not proof of a Vulkan synchronization error or a fully clean rendering run; SceneStore and its material logic were not changed by this port. See `port-smoke.log`.
