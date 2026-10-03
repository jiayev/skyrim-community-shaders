// Drawcall Limit Fix: CS_DCLF_FOLIAGE_PARITY's compare pass (IndirectDraws GpuLayouts.h, FoliageParityConstants).
//
// The colour pass's alpha-tested draws write what each pixel shows (Lighting.hlsl, DCLF_FOLIAGE_PARITY): its object word (the
// object's index + 1 in the low 24 bits, the frame's tag above them), and four words: its albedo (RGB8, and its motion vector's
// error against a static object's in tenths of a pixel), its diffuse and specular (RGBA8) and its motion vector (half2, pixels).
// A word whose tag is not its frame's was not written that frame (a pixel no alpha-tested draw shaded), so nothing is cleared.
//
// Within the frame, whatever the camera does:
// - owned: the Z-prepass's alpha-tested stages keep each pixel's closest fragment their alpha test passed (its owner: depth and
//   object, Lighting.hlsl). Where the owner's depth is the pixel's, the colour pass must have shaded that object there: an
//   owned pixel it left unshaded, or shaded with another object, is counted;
// - near white: the frame's foliage pixels whose albedo, or diffuse, is near white;
// - motion: with the trees' clocks frozen the foliage is static, so its motion vector is the camera's alone; one more than a
//   pixel from that is counted (and more than kMotionFar).
// Against the frame before, where the motion vector says the pixel was (the vectors are checked above), so whatever the camera
// does - the best of the same object's pixels around that point:
// - recoloured: its albedo, diffuse or specular moved by more than kColourStep in a channel;
// - brightened: its lit colour (diffuse and specular) by more than kBrightStep in luminance (whitened: and its albedo to near
//   white).
// With a still camera also: vanished (the frame before had an object here, and this frame has none of it here or beside) and
// appeared (the reverse); a moving camera covers and uncovers, so these count edges then.
cbuffer FoliageParityConstants : register(b0)
{
	uint4 Ids;          // both frames' pairs, by the epoch's parity
	uint4 Colours;
	uint2 FrameBlock;   // the colour epoch's words (Lighting.hlsl, DCLFFoliageParity*): which pair is this frame's, and its tag
	uint ResultsIndex;  // RWByteAddressBuffer: the counters, then the samples (GpuLayouts.h, FoliageCounter)
	uint OwnersIndex;   // RWStructuredBuffer<uint64_t>: the Z-prepass's owner per pixel (64 bits), cleared here for the next frame
	uint DepthIndex;    // Texture2D<float>: the main depth
	uint Padding0;
	uint Width;
	uint Height;
	uint2 Padding;
}
// This frame's (from the frame block) and the frame before's.
static uint2 CurrentIds, CurrentColours, PreviousIds, PreviousColours;
static uint Frame;

// GpuLayouts.h, FoliageCounter.
static const uint kCompared = 0, kVanished = 1, kAppeared = 2, kRecoloured = 3, kWhitened = 4, kFrameSampleCount = 5, kOwned = 6, kUnshaded = 7,
				  kOtherObject = 8, kWhiteAlbedo = 9, kWhiteDiffuse = 10, kEpochTag = 11, kMotion = 12, kMotionFarCount = 13, kInFrameSampleCount = 14,
				  kWhiteSampleCount = 15, kBrightened = 16, kCounters = 20;
// The samples, kSampleWords each: the frame before's comparisons', the in-frame checks', the near-white pixels'.
static const uint kSampleWords = 8, kFrameSamples = 24, kInFrameSamples = 32, kWhiteSamples = 16;
static const uint kFrameSampleBase = 0, kInFrameSampleBase = kFrameSampleBase + kFrameSamples, kWhiteSampleBase = kInFrameSampleBase + kInFrameSamples;
static const uint kColourWords = 4;
static const float kColourStep = 0.35;
static const float kBrightStep = 0.3;
static const float kMotionFar = 8.0;
// The depth buffer's unit (D24): the owner's depth is the fragment's, the buffer's that depth rounded to it.
static const float kDepthUnit = 1.0 / 16777215.0;

uint64_t Address(uint2 a_words) { return (uint64_t(a_words.y) << 32) | uint64_t(a_words.x); }

// The object at a pixel of a frame's buffer, 0 for none (not written that frame).
uint ObjectAt(uint2 a_buffer, int2 a_pixel, uint a_tag)
{
	if (a_pixel.x < 0 || a_pixel.y < 0 || uint(a_pixel.x) >= Width || uint(a_pixel.y) >= Height)
		return 0;
	const uint word = vk::RawBufferLoad<uint>(Address(a_buffer) + (uint64_t(a_pixel.y) * Width + uint(a_pixel.x)) * 4, 4);
	return (word >> 24) == a_tag ? (word & 0x00FFFFFFu) : 0;
}

bool Near(uint2 a_buffer, int2 a_pixel, uint a_tag, uint a_object)
{
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
			if (ObjectAt(a_buffer, a_pixel + int2(x, y), a_tag) == a_object)
				return true;
	return false;
}

float4 Unpack(uint a_word)
{
	return float4(a_word & 0xFF, (a_word >> 8) & 0xFF, (a_word >> 16) & 0xFF, a_word >> 24) / 255.0;
}

float2 UnpackHalf2(uint a_word) { return float2(f16tof32(a_word), f16tof32(a_word >> 16)); }

// A sample in a range: its slot from the range's counter, its words if the range has room. a_spread keeps one pixel in 31 (by a
// hash of its position), so a range covers the screen rather than its top rows.
void Sample(RWByteAddressBuffer a_results, uint a_counter, uint a_base, uint a_capacity, int2 a_pixel, bool a_spread, uint4 a_first, uint4 a_second)
{
	if (a_spread && ((uint(a_pixel.x) * 73856093u) ^ (uint(a_pixel.y) * 19349663u)) % 31u != 0)
		return;
	uint slot;
	a_results.InterlockedAdd(a_counter * 4, 1, slot);
	if (slot >= a_capacity)
		return;
	const uint offset = (kCounters + (a_base + slot) * kSampleWords) * 4;
	a_results.Store4(offset, a_first);
	a_results.Store4(offset + 16, a_second);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (dispatchID.x >= Width || dispatchID.y >= Height)
		return;
	const int2 pixel = int2(dispatchID.xy);
	const uint position = uint(pixel.x) | (uint(pixel.y) << 16);
	RWByteAddressBuffer results = ResourceDescriptorHeap[ResultsIndex];
	{
		const uint4 frameWords = vk::RawBufferLoad<uint4>(Address(FrameBlock) + 16, 4);  // width, height, tag, owners
		Frame = frameWords.z;
		const bool odd = (Frame & 1) != 0;
		CurrentIds = odd ? Ids.zw : Ids.xy;
		CurrentColours = odd ? Colours.zw : Colours.xy;
		PreviousIds = odd ? Ids.xy : Ids.zw;
		PreviousColours = odd ? Colours.xy : Colours.zw;
		if (all(dispatchID.xy == 0))
			results.Store(kEpochTag * 4, Frame);
	}
	const uint tag = Frame & 0xFF, previousTag = (Frame - 1) & 0xFF;
	const uint object = ObjectAt(CurrentIds, pixel, tag);
	const uint previous = ObjectAt(PreviousIds, pixel, previousTag);
	const uint64_t index = uint64_t(pixel.y) * Width + uint(pixel.x);
	const uint4 colours = object != 0 ? vk::RawBufferLoad<uint4>(Address(CurrentColours) + index * kColourWords * 4, 4) : uint4(0, 0, 0, 0);

	// Within the frame: the owner of the pixel's depth, if an alpha-tested Z-prepass draw has it.
	RWStructuredBuffer<uint64_t> owners = ResourceDescriptorHeap[OwnersIndex];
	const uint64_t owner = owners[uint(index)];
	if (owner != 0) {
		owners[uint(index)] = 0;
		Texture2D<float> depthBuffer = ResourceDescriptorHeap[DepthIndex];
		const float ownerDepth = asfloat(0xFFFFFFFFu - uint(owner >> 32));
		const uint ownerObject = uint(owner) & 0x00FFFFFFu;
		if (abs(depthBuffer.Load(int3(pixel, 0)) - ownerDepth) <= 1.5 * kDepthUnit) {
			results.InterlockedAdd(kOwned * 4, 1);
			const uint failure = object == 0 ? kUnshaded : object != ownerObject ? kOtherObject : 0;
			if (failure != 0) {
				results.InterlockedAdd(failure * 4, 1);
				Sample(results, kInFrameSampleCount, kInFrameSampleBase, kInFrameSamples, pixel, true, uint4(position, failure, ownerObject, object),
					uint4(asuint(ownerDepth), colours.x, 0, 0));
			}
		}
	}

	if (object != 0) {
		// Near white.
		const float4 albedo = Unpack(colours.x), diffuse = Unpack(colours.y);
		const bool whiteAlbedo = min(albedo.r, min(albedo.g, albedo.b)) >= 0.85, whiteDiffuse = min(diffuse.r, min(diffuse.g, diffuse.b)) >= 0.85;
		if (whiteAlbedo)
			results.InterlockedAdd(kWhiteAlbedo * 4, 1);
		if (whiteDiffuse)
			results.InterlockedAdd(kWhiteDiffuse * 4, 1);
		if (whiteAlbedo || whiteDiffuse)
			Sample(results, kWhiteSampleCount, kWhiteSampleBase, kWhiteSamples, pixel, true, uint4(position, whiteAlbedo ? kWhiteAlbedo : kWhiteDiffuse, object, colours.x),
				uint4(colours.y, 0, 0, 0));
		// Motion: the error the colour pass measured against a static object's motion vector, in pixels.
		const float errorLength = float(colours.x >> 24) / 10.0;
		if (errorLength > 1.0) {
			results.InterlockedAdd(kMotion * 4, 1);
			if (errorLength > kMotionFar)
				results.InterlockedAdd(kMotionFarCount * 4, 1);
			Sample(results, kInFrameSampleCount, kInFrameSampleBase, kInFrameSamples, pixel, true, uint4(position, kMotion, object, colours.x),
				uint4(asuint(errorLength), colours.w, 0, 0));
		}
	}

	// Against the frame before, where the motion vector puts the pixel then.
	const float2 motion = UnpackHalf2(colours.w);
	const int2 then = object != 0 ? int2(floor(float2(pixel) + 0.5 + motion)) : pixel;
	uint kind = 0;
	uint4 best = uint4(0, 0, 0, 0);
	if (object != 0) {
		// The same object's pixel nearest in colour among the point's and its neighbours'.
		float bestStep = 1e9, bestBright = 0;
		bool found = false;
		[unroll] for (int y = -1; y <= 1; ++y) {
			[unroll] for (int x = -1; x <= 1; ++x) {
				const int2 at = then + int2(x, y);
				if (ObjectAt(PreviousIds, at, previousTag) != object)
					continue;
				const uint4 earlier = vk::RawBufferLoad<uint4>(Address(PreviousColours) + (uint64_t(at.y) * Width + uint(at.x)) * kColourWords * 4, 4);
				const float3 albedoStep = abs(Unpack(colours.x).rgb - Unpack(earlier.x).rgb);
				const float3 diffuseStep = abs(Unpack(colours.y).rgb - Unpack(earlier.y).rgb);
				const float3 specularStep = abs(Unpack(colours.z).rgb - Unpack(earlier.z).rgb);
				const float step = max(max(max(albedoStep.x, albedoStep.y), albedoStep.z),
					max(max(max(diffuseStep.x, diffuseStep.y), diffuseStep.z), max(max(specularStep.x, specularStep.y), specularStep.z)));
				const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
				const float bright = dot(Unpack(colours.y).rgb + Unpack(colours.z).rgb, lumaWeights) - dot(Unpack(earlier.y).rgb + Unpack(earlier.z).rgb, lumaWeights);
				if (step < bestStep) {
					bestStep = step;
					bestBright = bright;
					best = earlier;
				}
				found = true;
			}
		}
		if (found) {
			results.InterlockedAdd(kCompared * 4, 1);
			if (bestBright > kBrightStep) {
				const float3 albedo = Unpack(colours.x).rgb;
				kind = min(albedo.r, min(albedo.g, albedo.b)) >= 0.85 ? kWhitened : kBrightened;
			} else if (bestStep > kColourStep) {
				kind = kRecoloured;
			}
		} else if (all(motion == 0) && !Near(PreviousIds, pixel, previousTag, object)) {
			kind = kAppeared;
		}
	} else if (previous != 0 && !Near(CurrentIds, pixel, tag, previous)) {
		// Only meaningful for a still camera (the frame before's pixel has no motion of its own here).
		kind = kVanished;
	}
	if (kind == 0)
		return;
	results.InterlockedAdd(kind * 4, 1);
	// The samples are the colour changes' (a moving camera's vanished pixels are its own edges).
	if (kind == kVanished || kind == kAppeared)
		return;
	Sample(results, kFrameSampleCount, kFrameSampleBase, kFrameSamples, pixel, true, uint4(position, kind, object, colours.y),
		uint4(best.y, colours.z, best.z, colours.w));
}
