# Measuring generated-frame pacing

Frame rate does not tell you whether generated frames are *paced*. A generator can present twice
as many frames and deliver no extra smoothness if each generated frame lands next to its real
frame instead of halfway between them. The overlay cannot see this: it reports render frame
time, which stays smooth while the delivered cadence is ragged.

PresentMon reports the cadence directly. `capture-pacing.ps1` loads the standard bench scene,
settles it, and captures 20 s of presents; `pmstats.awk` reduces the CSV.

```
capture-pacing.ps1 -Label dlssg -Settings SettingsUser.dlssg_unlocked.json
awk -F, -f pmstats.awk pm_dlssg.csv   # rate, deviation, bunching
awk -F, -f hist.awk    pm_dlssg.csv   # deviation histogram + sd excluding the worst 1%
awk -F, -f seg.awk     pm_dlssg.csv   # the same split into four time segments
```

## Read MsBetweenDisplayChange, not MsBetweenPresents

This is the trap, and it is easy to draw exactly the wrong conclusion from it.

`MsBetweenPresents` is when the application *called* present. `MsBetweenDisplayChange` is when
the frame actually reached the screen. A frame generator that meters flips in software submits
its two presents together and spaces them afterwards, so the two columns disagree completely:

|  | present intervals | display changes |
| --- | --- | --- |
| FSR-FG, uncapped | sd 0.39 ms, 0.0% under 1 ms | sd 0.51 ms, 0.0% under 1 ms |
| DLSS-G, uncapped | sd 3.39 ms, **49.7%** under 1 ms | sd 1.02 ms, **1.1%** under 1 ms |

Judged on presents alone DLSS-G looks completely unpaced. Judged on what is scanned out it is
paced, just about twice as loosely as FFX. Only the second column describes what a player sees.

## Cadence is correct, and best where it matters

Uncapped at 279 fps the display cannot give each frame its own scanout, so some variation there
is expected and not very meaningful. Capped -- how the game is actually played -- DLSS-G is
exact:

```
display changes 595 | mean 33.35 ms | sd 0.01 ms | 0% bunched
  min 33.31 ms  max 33.40 ms  100% of intervals in a single 8 ms bucket
```

Locked to the target with a hundredth of a millisecond of deviation -- which turns out to be the
vblank clock rather than a pacing result, since two refresh intervals on this display is 33.350 ms.
See "The DLSS-G comparison was confounded by present mode" below.

## What that means for the two generators

**FFX (FSR-FG)** owns its present loop and spaces the generated frame itself, so both columns
agree and the cadence is tight.

**DLSS-G** emits its generated frame from inside the same present call as the real one — the same
property that forces `GetRenderedFrameRateLimit` to hand it the frame limit undivided — and then
meters the flip in software. On Ada that metering is software, not the hardware flip metering
Blackwell added.

Nothing is dropped at the DXGI layer in either case.

## FSR-FG carries residual jitter that DLSS-G does not

> Read "The DLSS-G comparison was confounded by present mode" below before trusting the ratios in
> this section. DLSS-G runs on MAILBOX and is quantised to vblank; FSR-FG runs on IMMEDIATE and is
> not. The two columns are not the same measurement.

Capped, the two generators differ sharply in delivered cadence:

| capped | display sd | min | max |
| --- | --- | --- | --- |
| DLSS-G | **0.01 ms** | 33.31 | 33.40 |
| FSR-FG | **2.65 ms** | 28.92 | 67.33 |

FSR-FG holds target on 97.6% of frames, but 2.2% arrive late and 0.5% are doubled outright --
a visible hitch every six seconds or so. DLSS-G never misses.

The difference is which pacer is in charge. FFX paces by blocking the caller in `Present` (see
below), so its cadence is whatever its own internal pacer produces and nothing upstream of the
present can improve it. DLSS-G is handed the OUTPUT target and meters its flips downstream of the
game's present, which is why it can hit a hundredth of a millisecond. FFX's pacer is not
reachable in the sense of overriding where it puts the frame -- but it is tunable, and its default
tuning was the defect. See "Fixed: FFX's frame-pacing tuning" below.

Splitting the deviation shows two separate faults rather than one. Removing the worst 1% of
intervals:

| capped | sd | sd excl. worst 1% | intervals >10 ms off |
| --- | --- | --- | --- |
| DLSS-G | 0.013 ms | 0.013 ms | 0% |
| FSR-FG | 2.65 / 2.16 / 1.82 ms | 1.00 / 0.82 / 0.81 ms | 1.0% / 0.3% / 0.5% |

A continuous sub-millisecond phase spread, plus a rare dropped frame that dominates the untrimmed
figure. The tail is what a player notices -- and the tail is the half that turned out to be fixable.



## Fixed: FFX's frame-pacing tuning

FFX does not place its interpolated frame at the midpoint. From the SDK header:

```
target frametime delta = average Frametime - (variance * varianceFactor) - safetyMarginInMs
```

Both terms pull the interpolated frame EARLY, and FFX's defaults (0.1 ms / 0.1) subtract about
0.19 ms at the ~0.9 ms of variance this game shows. That is the whole of the offset by which
FSR-FG's delivered cadence sat behind DLSS-G's on the same target -- mean 33.42-33.64 ms against a
33.35 ms target, drifting a full refresh interval every few seconds and dropping a frame when it
got there.

`sl.fsr_g` was already setting this key, to FFX's defaults. It now takes the values from the host
(`FSRFrameGenOptions::pacingSafetyMarginMs` / `pacingVarianceFactor`, appended LAST in the struct)
and re-applies them from the present thread whenever they change, so no swapchain recreate is
needed. CS sets **0.01 ms / 0.0**.

Measuring the settled half of each capture, counting display intervals more than 10 ms off the
mean -- a doubled frame, the visible hitch:

| safety / variance | capped, hitches per run | mean | worst |
| --- | --- | --- | --- |
| 0.1 / 0.1 (FFX default) | 0.67 0.67 0.67 0.68 1.01 1.34 % | 0.84 | 1.34 |
| 0.05 / 0.05 | 0.34 0.00 1.35 % | 0.56 | 1.35 |
| **0.01 / 0.0 (shipped)** | 0.00 0.00 0.00 0.34 0.67 % | **0.20** | **0.67** |

The shipped pair is the only one whose worst run matches the default's best, and three of its five
runs have no hitch at all. The midpoint overlaps the default completely and was rejected despite a
promising first run -- the third went to 1.35%.

The mean also lands where it should: 33.31-33.35 ms against 33.42-33.64 on the defaults.

### What it costs

Continuous deviation, excluding the worst 1% of intervals, gets slightly worse -- the safety margin
was doing something, just less than it cost:

| safety / variance | capped sd excl. 1% | uncapped sd excl. 1% |
| --- | --- | --- |
| 0.1 / 0.1 | 0.87 0.95 0.95 | 0.48 0.44 |
| 0.05 / 0.05 | 0.58 0.84 2.13 | 0.56 0.49 |
| 0.01 / 0.0 | 1.10 1.53 1.07 | 0.89 0.84 0.85 |

Uncapped the relationship inverts cleanly: at ~275 fps the margin is 5% of a 3.6 ms frame and holds
placement together, where at 33 ms it is 0.6% and only displaces it. A single pair is used anyway,
because the trade is a sub-millisecond continuous deviation -- invisible at either rate -- against a
dropped frame, which is not, and because there are no hitches uncapped under any tuning.

### Present queue depth

Also swept, since it decides how far DXVK's presenter may run ahead of FFX's paced present
(`CS_FSRFG_QDEPTH`). The existing depth of 2 is already the best of the three:

| depth | sd | sd excl. 1% | hitches |
| --- | --- | --- | --- |
| 1 | 1.674 | 1.637 | 0.00% |
| **2 (current)** | **1.096** | **1.073** | 0.00% |
| 3 | 1.390 | 1.360 | 0.00% |

Both extremes of `dxvkSetSyncPresent` remain unusable for a different reason: fully synchronous
wedges the swapchain recreate that installs the FFX wrap, and unrestricted wedges shortly after it.


### DLSS-G was not affected, and has no equivalent knob

The tuning above is specific to `sl.fsr_g`. DLSS-G measured identically either side of it, and
`DLSSGOptions` exposes no pacing field at all -- only `numFramesToGenerate` and
`queueParallelismMode` -- because its flip metering happens inside the plugin and driver:

| DLSS-G, capped | mean | sd | hitches |
| --- | --- | --- | --- |
| before | 33.348 | 0.016 | 0% |
| after | 33.348 | 0.014 | 0% |
| after | 33.348 | 0.016 | 0% |

Check the log before believing any DLSS-G capped number. DLSS-G takes the cap undivided, so a run
where frame generation never engaged presents at the same 30 fps as one where it did, and scores
the same from outside. The runs above logged `rendered 15.0 fps | post-FG 30.0 fps | mult=2`.

With the FSR fix in, the two are equivalent uncapped and FSR-FG is marginally ahead:

| | DLSS-G | FSR-FG |
| --- | --- | --- |
| capped, sd excl. 1% | **0.013 ms** | 1.07-1.53 ms |
| capped, hitches | 0% | 0% |
| uncapped, sd excl. 1% | 0.91 / 0.96 ms | **0.85 / 0.89 ms** |

The remaining gap is the capped continuous spread, and it is structural: DLSS-G meters flips
downstream of the game's present, so it re-times frames after the fact, while FFX places its frame
at submission and lives with what the loop hands it. About 1 ms on a 33 ms interval, nothing
dropped.

### Sweeping it again

`CS_FSRFG_SAFETY` and `CS_FSRFG_VARIANCE` override the host's values inside sl.fsr_g, and
`CS_FSRFG_QDEPTH` overrides the queue depth, so neither needs a rebuild to test.

## What actually paces the FSR-FG loop

Worth stating plainly, because two comments in the source used to imply otherwise: on the FSR-FG
path **FidelityFX paces the loop itself, by blocking inside `Present`**. Reflex's `frameLimitUs`
is handed to it and does nothing there.

Instrumenting a present gate made this unambiguous. Gating the game's `Present` call onto a fixed
66.7 ms grid, the gate reported waiting on every single frame:

```
[Pace] 300 frames | waited 300 (58.04 ms avg) | late 0 | re-anchored 0 | arrival vs grid -58.06 ms avg
```

58 ms of wait in a 66.7 ms frame. The render loop's own work is only ~8.7 ms; the other 58 ms was
never Reflex sleeping and never the loop being slow -- it was FFX holding the caller until it was
ready for the next real frame. The gate did not add pacing, it *relocated* FFX's pacing wait to
before the present call.

## Tried and rejected: gating the present

That relocation is harmful, for a reason that is obvious once the mechanism is right: arriving at
FFX exactly when it wants the next frame leaves it no slack to place the interpolated one. The
delivered cadence splits systematically instead of jittering -- in one run 63.8% of intervals sat
2-4 ms off the mean, against 3.5% ungated. It also costs up to 58 ms of latency, since the wait
happens with a fully rendered frame in hand.

Four runs, deviation excluding the worst 1% of intervals:

```
gate off   1.39 / 4.35 ms
gate on    3.04 / 0.43 ms
```

The 0.43 ms run is real and is the reason this was worth chasing -- segmented, it held sd 0.40 /
0.47 / 0.56 ms over fifteen seconds, near DLSS-G's 0.013 ms. But it is a phase accident: the gate
happened to land in step with FFX's own pacer. Adding 2% headroom to the Reflex cap to make the
lock reproducible did not (that run went to 3.12 ms), because the headroom was addressing a slow
render loop that does not exist.


## The DLSS-G comparison was confounded by present mode

Every "DLSS-G is 80x tighter" figure on this page compared two things that are not the same
measurement. The bench display runs at 3440x1440 **59.97 Hz** -- a 16.675 ms refresh interval.

The two generators do not get the same Vulkan present mode:

| method | vsync | cap | CS preference | present mode |
| --- | --- | --- | --- | --- |
| DLSS-G | either | capped | 0 (tear-free) | `MAILBOX` |
| FSR-FG | off | either | 2 -> no preference | `IMMEDIATE` |
| FSR-FG | on | either | 2 -> no preference | `FIFO` |

`MAILBOX` and `FIFO` flip at vblank. `IMMEDIATE` tears mid-scanout. So DLSS-G's display-change
timestamps are quantised to a hardware clock and FSR-FG's are continuous.

DLSS-G capped measures **33.348 ms, sd 0.013**. Two refresh intervals is **33.350 ms**. That
figure is the vblank clock, not the quality of DLSS-G's flip metering -- it is what any generator
hitting every second vblank would score.

Turning vsync on for FSR-FG proves it. The intervals become exact refresh multiples:

```
16-17 ms  3.9%      33-34 ms  92.5%      49-51 ms  3.7%
```

Quantised, as predicted -- and much worse in practice: FFX misses the two-refresh slot 7.5% of the
time, so sd goes to 5.96 ms and 12.75% of intervals are a full refresh out. `IMMEDIATE` is the
right mode for FSR-FG and the shipped configuration already picks it.

Which means the residual continuous deviation is **1.1 ms against a 16.675 ms refresh interval** --
6.6% of one refresh. Under a tearing present mode that is a tear line sitting a few scanlines from
where it ideally would, not a judder. The defect that was worth fixing was the dropped frame, and
that is fixed.

### Rejected: decoupling the two FFX pacing terms

The sweep above moved `pacingSafetyMarginMs` and `pacingVarianceFactor` together, so the shipped
pair might not have been the best point -- the variance term is adaptive where the margin is a flat
offset. Holding the margin at 0.01 and restoring the variance factor to FFX's 0.1, three runs each:

| | continuous sd excl. 1% | hitches |
| --- | --- | --- |
| 0.01 / 0.0 (shipped) | 1.29 / 1.50 / 0.77 | none |
| 0.01 / 0.1 | 1.08 / 1.05 / 1.13 | one run at 0.34% |

The continuous difference (mean 1.19 vs 1.09) sits inside the shipped arm's own spread, and the
variance term brought a hitch back. Not a win; shipped pair stands.


## Matched to the Streamline sample

NVIDIA's reference for DLSS-G presentation on Vulkan is the Streamline sample. Its swapchain, in
`donut/src/app/vulkan/DeviceManager_VK.cpp`:

```cpp
.setPresentMode(vsyncEnabled ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate)
```

Nothing in the sample varies that by frame-generation method or by frame cap, and **MAILBOX is
never used**. Its present is a plain `vkQueuePresentKHR` with no present ID or fence chained,
which is what DXVK already does. `swapChainBufferCount` is 3.

The sample does gate vsync on DLSS-G, but on the SDK's own answer rather than an assumption --
`UIRenderer.h`, tagged SL-VSYNC-011: the VSync checkbox is enabled from
`DLSSGState::bIsVsyncSupportAvailable`, and the setting is forced off only when that says no.

CS now does both: the present mode follows the sync interval alone, and the vsync toggle consults
`bIsVsyncSupportAvailable` instead of assuming DLSS-G always precludes it. On this box the flag
reports **no**, so the effective behaviour here is unchanged -- but it is now correct for hardware
where DLSS-G does support vsync.

### What this costs on PresentMon, and why it was done anyway

Matching the sample moves DLSS-G capped from MAILBOX to IMMEDIATE, and PresentMon rates that far
worse:

| DLSS-G capped | sd excl. 1% | intervals >10 ms off |
| --- | --- | --- |
| MAILBOX | 0.010 ms | 0.00% |
| IMMEDIATE (sample-matched) | 11.79 ms | 25.68% |

That comparison is not trustworthy, for the reason this page already documents: MAILBOX quantises
flips to vblank, so its 0.010 ms is the vblank clock and not a pacing result. IMMEDIATE has no
such clock, and DLSS-G meters its flips **in software, below the level PresentMon observes** --
it submits the frame pair together and spaces the flips afterwards. The IMMEDIATE histogram shows
exactly that signature: ~38% of intervals on target, with the remainder split between very short
(<10 ms) and very long (58-64 ms) -- a partially-resolved pair, not a measurement of scanout.

So PresentMon cannot adjudicate between "the metering is not working here" and "PresentMon cannot
see the metering". Where a measurement cannot settle a question, the reference implementation
wins.

**Confirmed by looking at it: capped DLSS-G is smooth on IMMEDIATE.** That closes the question in
favour of the second reading -- the metering works, and the 25.68% is PresentMon failing to
resolve a metered pair. Treat any sub-frame DLSS-G number from this tool as meaningless, and do
not "fix" a regression it reports here without first checking the screen.

FSR-FG is unaffected by the change in kind (0.76 -> 1.04 ms, no hitches either way) since it was
already on IMMEDIATE via the old preference's fallback.

## The bench is noisier than one run can show

Every conclusion on this page that rests on a single capture should be distrusted. Across six
capped FSR-FG runs of the same scene with no code change at all, deviation excluding the worst 1%
of intervals ranged 0.81 to 4.35 ms, and the untrimmed figure ranged 1.82 to 6.85 ms. Two separate
changes here produced a convincing first result that reversed on pairing.

Segment each capture before trusting it -- `seg.awk` splits it into quarters. The first quarter is
routinely several times worse than the rest even after a 60 s settle, so a capture that starts
early reads as a regression that is not there.

## Tried and rejected

Chaining present IDs into `VkPresentInfoKHR` so DLSS-G could use `vkWaitForPresentKHR` for
timing feedback. DXVK had stopped chaining them along with the present fence, but the deadlock
argument for dropping the fence never applied to IDs, since nothing in DXVK waits on one.
Restoring them changed nothing for the better: display sd 1.02 -> 1.11 ms, frames bunched at
scanout 1.1% -> 2.1%, and 3% fewer frames. DLSS-G is not waiting on present IDs here.

Present mode and frame cap were also each measured against DLSS-G's cadence and neither moved it.

Handing FSR-FG the output target undivided, so Reflex paces output the way it does for DLSS-G.
It does not pace better and it breaks the cap: against a 30 fps target it delivered 59.8 fps,
because Reflex then holds the render loop at 30 and FFX still doubles it. Normalised for rate
the jitter is unchanged, 7.9% of the frame interval against 8.4%.

Removing DXVK's frame limiter -- pacing-neutral, 2.66 -> 2.65 ms, because Reflex
was already available so CS had it switched off.

Turning Reflex low-latency mode on for the FSR-FG path. With DXVK's limiter gone, one of the two
reasons for leaving it off went with it, and steadying the render loop is exactly what this
defect wants. It does not steady it. Three paired runs each, display-interval deviation:

```
mode off   2.65 / 2.16 / 1.82 ms    mean 2.21
mode on    2.20 / 3.40 / 2.63 ms    mean 2.74
```

Slightly worse, ranges overlapping. Worth recording how this one presented: the first run of
each showed 2.20 against 2.65 and read as a 17% win in the direction the mechanism predicts.
Pairing reversed it. Single runs on this bench have now produced a false positive three times
in a row, so nothing here should be landed on one.
