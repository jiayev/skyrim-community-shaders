#pragma once

// IndirectDraws' implementation, shared by the files of this folder only (IndirectDraws.h is the interface).

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#	include "Features/DrawcallLimitFix/Runtime/RenderThreadBudget.h"
#	include "Features/DrawcallLimitFix/Engine/Access/EngineReadWindow.h"
#	include "Features/DrawcallLimitFix/Engine/Capture/ConstantMirror.h"
#	include "Features/DrawcallLimitFix/Draws/Pipelines/DrawPipelines.h"
#	include "Features/DrawcallLimitFix/Draws/Pipelines/DrawPipelinesRhi.h"
#	include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#	include "Features/DrawcallLimitFix/Draws/Pipelines/EngineStates.h"
#	include "Features/DrawcallLimitFix/Engine/Capture/FaceSnapshots.h"
#	include "Features/DrawcallLimitFix/Scene/Ports/FadeState.h"
#	include "Features/DrawcallLimitFix/Draws/Gpu/FrameData.h"
#	include "Features/DrawcallLimitFix/Draws/Gpu/FrameValues.h"
#	include "Features/DrawcallLimitFix/Draws/Gpu/GpuResources.h"
#	include "Features/DrawcallLimitFix/Draws/Gpu/GpuTextures.h"
#	include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"
#	include "Features/DrawcallLimitFix/Scene/Ports/MaterialSources.h"
#	include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "Features/DrawcallLimitFix/Scene/Ports/LocalShadows.h"
#	include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#	include "Features/DrawcallLimitFix/Engine/Views/PortalViews.h"
#	include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#	include "Features/DrawcallLimitFix/Engine/Cull/SideEffects.h"
#	include "Features/DrawcallLimitFix/Draws/Pipelines/ShaderPrograms.h"
#	include "Features/DrawcallLimitFix/Engine/Views/ShadowViews.h"
#	include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#	include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"
#	include "RE/B/BSShadowLight.h"
#	include "RE/B/BSLightingShaderMaterialBase.h"
#	include "RE/B/BSShadowDirectionalLight.h"
#	include "RE/B/BSCullingProcess.h"
#	include "RE/N/NiCamera.h"
#	include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#	include <ORGModuleServices/Async/RevisionAssembly.h>
#	include <ORGModuleServices/Async/SerializedTaskPump.h>
#	include "Features/DrawcallLimitFix/Runtime/Switches.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/PayloadChecks.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Reports/Draws.h"
#	include "Features/DrawcallLimitFix/Runtime/SceneScheduler.h"
#	include "Features/DrawcallLimitFix/Scene/Ports/VertexInput.h"
#	include "Deferred.h"
#	include "Features/LinearLighting.h"
#	include "Features/Skin.h"
#	include "Features/LightLimitFix/ORGLightCulling.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/NvPerfBridge.h"
#	include "RenderGraph/PrefixSum.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "ShaderCache.h"
#	include "State.h"
#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/BindingTable.h>
#	include <Render/LatchBlock.h>
#	include <Render/Runtime/ExternalSignalReservation.h>
#	include <Render/Runtime/StagedUploadBatch.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Render/Runtime/DescriptorServiceAccess.h>
#	include <Render/Runtime/IDescriptorService.h>
#	include <Render/Runtime/UploadServiceAccess.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/Buffers/VersionedBuffer.h>
#	include <ORGModuleServices/VersionedBufferGrowth.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <rhi_helpers.h>
#	include <bit>
#	include <chrono>
#	include <cstdlib>
#	include <cstddef>
#	include <filesystem>
#	include <fstream>
#	include <iterator>
#	include <map>
#	include <optional>
#	include <cstring>

#	include "Features/DrawcallLimitFix/Runtime/EventQueue.h"
#	include "Features/DrawcallLimitFix/Runtime/LatestSlot.h"
#	include "Core/GpuLayouts.h"
#	include "Core/Versioned.h"
#	include "Core/GrowableRows.h"

#	include "Core/Helpers.h"
#	include "Revision/Epochs.h"
#	include "Core/Latched.h"
#	include "Main/FrameLayout.h"
#	include "Core/Readbacks.h"
#	include "Core/SceneBuffers.h"
#	include "Main/Resources.h"
#	include "Revision/Shapes.h"
#	include "Shadow/ShadowFrame.h"
#	include "Core/FrameSlots.h"
#	include "Build/KeptStores.h"
#	include "Build/MainPayload.h"
#	include "Build/ShadowPayload.h"
#	include "Build/Geometry.h"
#	include "Build/MainBuild.h"
#	include "Build/ShadowBuild.h"
#	include "Core/Uploads.h"
#	include "Passes/PassHelpers.h"
#	include "Epochs/ViewEpoch.h"
#	include "Revision/Revision.h"
#	include "Impl.h"


#endif
