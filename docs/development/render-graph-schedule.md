# Render graph schedule

What the persistent render graph executes, explained: each executable (the whole frame, and each epoch's), its batches per queue,
the passes in each batch, each pass's resource accesses and the state changes (barriers) planned before it, and every cross-queue
wait with the scheduling edges that caused it. The data is ORG's `org::ScheduleTrace` (`extern/OpenRenderGraph/include/Render/
RenderGraph/ScheduleTrace.h`). How CS runs the graph is [render-graph.md](./render-graph.md).

The graph traces its schedule only while something asks for it (`RenderGraph::SetScheduleTraceEnabled`). Two things can ask: the
window and the DevBench tool. `src/RenderGraph/ScheduleWindow.cpp` keeps one flag for each and enables tracing while either is set.
The trace is made once per executable when the graph compiles it. While tracing is on, the first frame after a change may not have
a trace yet.

## The window

Open the Community Shaders menu, go to Drawcall Limit Fix, and click **Render graph schedule** next to **Reload shaders**. The
window is drawn with the menu. Closing it stops its tracing request.

-   **Executable**: the whole frame or one epoch, by label. **Copy as text** copies `ScheduleTraceToText` of the selected
    executable to the clipboard. **Copy all** copies it for every executable.
-   **Lanes**: one lane per queue slot, with the batches in batch order along x. Each batch is a box, and its passes are boxes
    inside it. Arrows go from a producer batch to the consumer batch that waits for it.
    -   Hover a pass to see its queue and preferred queue, its accesses (R/W and resource), and its state changes. Each state
        change shows access, layout and sync as hex before -> after, the subresource range, and the previous pass.
    -   Hover a wait's handle (the dot on the arrow) to see every cause: kind, from pass -> to pass, resource and hazard.
    -   Hover a batch to see its waits and its last-frame submission waits (incoming, uploads or frame values, with timeline and
        value), and whether it has external entry and exit barriers.
    -   Click any of these to pin it in the **Selection** tab.
-   **Resource filter**: highlights the passes that use a resource whose name contains the text (case does not matter). With
    "Filter the causes table" checked, the table lists only the causes on a matching resource.
-   **Waits and causes**: a table with one row for each cause of each wait. Click a row to select the consumer pass.

The UI is ORG's `org::debugui::ShowScheduleInspector` (`DebugUI/ScheduleInspector.h`). It needs only Dear ImGui and is built as
`OpenRenderGraph::ScheduleUI` (`OPENRENDERGRAPH_ENABLE_SCHEDULE_UI`). `cmake/RenderGraph.cmake` turns it on.

## The DevBench tool

The tool is `communityshaders.schedule`, with an `action` argument:

| action    | Result |
| --------- | ------ |
| `enable`  | Starts this tool's tracing request. Returns `{tracing}`. Wait a frame before you read. |
| `disable` | Ends this tool's request. Tracing continues while the window is open. |
| `json`    | Returns the snapshot as `ScheduleTraceToJson` writes it: `{executables, lastSubmission}`. |
| `text`    | Returns `{labels, text}`: `ScheduleTraceToText` of the executable named by `label` (for example `frame` or `epoch 3`). Without `label`, it returns every executable. |

`json` and `text` return an error while nothing has been traced: the tracing is off, the render graph is not running, or no
frame has been traced yet.
