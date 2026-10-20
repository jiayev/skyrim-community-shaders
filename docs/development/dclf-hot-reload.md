# Shader hot reload

How an edit to a shader under `Data/Shaders` reaches the render graph's live pipelines without a restart: every compute program
(`ComputeProgram`) and every Drawcall Limit Fix graphics pipeline. It follows BasicRenderer's PSOManager model: a pipeline registers
once, generically, with how to build it again, and its consumers bind through the slot it gets. How DCLF builds its programs is
[dclf-architecture.md](./dclf-architecture.md); the switches and test commands are [dclf-validation.md](./dclf-validation.md).

## The registry

`org::services::PipelineRegistry` (`extern/ORGModuleServices`, `PipelineRegistry.h`). CS owns one for the process:
`RenderGraphRuntime::PipelineRegistry()`.

-   `Register(desc, payload, fingerprint, builtFor)` returns a `LivePipelineSlot`: an atomic payload and its identity (`Peek`), as
    `org::PipelineStateSlot`. The payload is type-erased, because a DCLF pipeline is an aggregate (both variants, its pulled
    pipelines, its state class). `desc` has an id for the log, the shader files it is compiled from, and the rebuild. An entry
    dies with its slot's last holder.
-   The rebuild builds the pipeline again from the files as they are now. It must not block: it calls `done` once, from any
    thread, with a new payload, with "unchanged" (its fingerprint equals the slot's and the reload is not forced), or with an
    error. A failure leaves the slot alone, and the error is logged.
-   `RequestReload(file, force)` can be called from any thread. An `.hlsl` that some entry names rebuilds those entries. Any other
    file (an include) rebuilds every entry. An empty name also rebuilds every entry. The compiler reports no include list, so this
    is the conservative fallback. Unchanged entries cost a memory-cache hit, since the compiler is content-addressed and
    dependency-hashed.
-   `Service()` runs on the render thread at each present (`RenderGraphRuntime::SignalFramePace`):
    -   It starts what was asked: it moves `SourcesGeneration`, tells the listeners, and runs the rebuilds' starts on one task.
    -   It finishes a reload once every rebuild is in: it logs one line, stages the new payloads and asks the graph host for a
        rebuild.
-   `Publish` is the safe point. Every graph build runs it, through the `cs.org.pipeline-registry` extension's factory. There the
    host's thread is stopped and the old graph's work has been waited for. `Publish` swaps the staged payloads into their slots
    and moves `Generation`. Each replaced payload is held until the host's retired callback passes `SubmissionsMade()` at that
    build. Every recording after the build is of the new graph: every epoch, revision-driven or live, records again.
-   An entry built for an earlier `SourcesGeneration` (its build began before a reload read the files) is rebuilt by the next
    `Service`. So no pipeline keeps the files of before.

## Triggers

-   DCLF's menu, **Reload shaders**: every entry.
-   CS's **Clear shader cache**, and every full `ShaderCache::Clear`: `DrawcallLimitFix::ClearShaderCache`, every entry.
-   The shader watcher (`UpdateListener::UpdateCache`): the edited file. An `.hlsl` under `Data/Shaders/DrawcallLimitFix/` leaves
    the D3D11 cache alone.
-   `CS_DCLF_TEST_COMMANDS=<frame>:dclf:reload-shaders` reloads every entry. `=all` reloads every entry and forces each to be
    built and swapped. `=<file>` reloads the entries of that file, as the watcher does.

## Compute programs

`ComputeProgram::Load` registers the program with its source. Its rebuild compiles the source again and makes a new SPIR-V's
pipeline under the program's own layout. The program object and its layout stay, so no dispatch signature or owner changes. Passes
bind `program->Pipeline()` at record time.

## DCLF's graphics pipelines

-   **Programs.**
    -   `ShaderPrograms` reads its sources (Lighting, Utility, the families') for one `SourcesGeneration`. It reads them again, and
        empties the stage cache, once that generation moves.
    -   The pipeline lane drops its program entries then (`FollowSources`), and requests again the keys that were parked on a
        program or had failed (`RetryAfterSourcesMoved`).
    -   A rebuild takes its program from `ShaderPrograms::Rebuild*`. These may run on any thread. They make the same requests as
        the lane's `Find*`, straight to the compiler.
    -   A program's fingerprint is its stages' binaries, hashed. It is also the service recipe's shader key, so a changed program is
        never the service's cached pipeline.
-   **Registration.** Every kind's request makes its registration (`Impl::MakeLive`) with the same build lambda the service runs:
    main keys, shadow keys, forward keys, draw families and refraction. Each is registered when admitted. The lane keeps the slot
    where it keeps the payload: `setSlots` by set index, and `slot` on forward entries, draw families and refraction.
-   **Adoption.** Once `Generation` moves, `AdoptLivePipelines` (one step for every kind) takes each swapped slot's payload:
    -   For the main set, it also takes the usage and constant tables, then the Z-prepass groups and state classes again. For the
        shadow set, it takes the discard class.
    -   A set that changed gets new versions (`Versions::Retire`). The next publication applies every index from the new payloads,
        and no index a version held is rewritten.
    -   The catalog is published again. The scene lane resolves its lookups against it, and the next revision binds the new sets.
    -   A replaced forward, draw family or refraction payload is kept for the process, as a target change's is.

## A new feature opts in

Register each pipeline once, at build. Bind through the slot, at record time:

```cpp
org::services::LivePipelineDesc live{ .id = "my.pass", .sources = { "MyFeature/MyPassCS.hlsl" },
    .rebuild = [](const org::services::LiveRebuildRequest& request, org::services::LiveRebuildDone done) {
        // compile asynchronously; if the binary's hash == request.fingerprint && !request.force: done({ .fingerprint = hash });
        // else make the pipeline, done({ .payload = ..., .fingerprint = hash }), or done({ .error = ... })
    } };
auto slot = RenderGraphRuntime::PipelineRegistry()->Register(std::move(live), payload, hash,
    /* SourcesGeneration() read before the sources were */ builtFor);
```

A compute shader gets all of this from `ComputeProgram::Load`. A consumer that copies payloads into its own structures (as DCLF's
sets do) adds a listener (`AddListener`) and adopts once `Generation` moves.

## Verifying

Run with `CS_DCLF_TEST_COMMANDS=<frame>:dclf:reload-shaders=all` and look for:

-   `[ORG] shader reload asked: every live pipeline, each swapped even if unchanged`
-   `[DCLF] SPIR-V builds of Data/Shaders/Lighting.hlsl: N shader files tracked as dependencies (sources generation 1)`, which
    shows the sources were read again.
-   `[DCLF] shader programs: sources generation 1; ... program entries of the files before dropped`
-   `[ORG] shader reload: N of M pipelines rebuilt (K failed, U unchanged) in S s`. With `=all`, N is M minus K.
-   `[ORG] shader reload: N pipelines swapped in at graph build B (registry generation G)`
-   `[DCLF] pipeline lane: registry generation G: X main, Y shadow and Z forward, draw family and refraction pipelines swapped in`

Then check the frame wait for the build's new snapshot (as `dclf:rebuild` shows), and that sequences stay above zero. Without
`=all`, an unedited tree gives `0 of M pipelines rebuilt (0 failed, M unchanged)` and no swap.
