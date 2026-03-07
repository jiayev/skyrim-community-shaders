/// FFT Core - GPU Fast Fourier Transform for image convolution
/// Stockham auto-sort FFT with group shared memory
/// Mixed-radix butterflies (2, 4, 8) for power-of-two signal lengths

#pragma once

// Requires defines: RADIX, SCAN_LINE_LENGTH
// SCAN_LINE_LENGTH must be a power of RADIX

#ifndef TWO_PI
#	define TWO_PI (2.0f * 3.14159265358979323846f)
#endif

#define Complex float2
#define FFTMemoryBarrier() GroupMemoryBarrierWithGroupSync()

#define NUMTHREADSX (SCAN_LINE_LENGTH / RADIX)
#define STRIDE (SCAN_LINE_LENGTH / RADIX)

// --- Complex Arithmetic ---

Complex ComplexMult(in Complex A, in Complex B)
{
	return Complex(A.x * B.x - A.y * B.y, A.x * B.y + B.x * A.y);
}

Complex ComplexConjugate(in Complex Z)
{
	return Complex(Z.x, -Z.y);
}

// --- Radix FFT Kernels ---

void Radix2FFT(in bool bIsForward, inout Complex V0, inout Complex V1)
{
	Complex Tmp = V0;
	V0 = Tmp + V1;
	V1 = Tmp - V1;
}

void Radix2FFT(in bool bIsForward, inout Complex V[RADIX])
{
	Complex Tmp = V[0];
	V[0] = Tmp + V[1];
	V[1] = Tmp - V[1];
}

void Radix4FFT(in bool bIsForward, inout Complex V0, inout Complex V1, inout Complex V2, inout Complex V3)
{
	// Stage 1: length-2 butterflies
	Complex T0 = V0 + V2;
	Complex T2 = V0 - V2;
	Complex T1 = V1 + V3;
	Complex T3 = V1 - V3;

	// Stage 2: twiddle and combine
	Complex Rot;
	if (bIsForward)
		Rot = Complex(-T3.y, T3.x);  // i * T3
	else
		Rot = Complex(T3.y, -T3.x);  // -i * T3

	V0 = T0 + T1;
	V1 = T2 + Rot;
	V2 = T0 - T1;
	V3 = T2 - Rot;
}

void Radix4FFT(in bool bIsForward, inout Complex V[RADIX])
{
	Radix4FFT(bIsForward, V[0], V[1], V[2], V[3]);
}

void Radix8FFT(in bool bIsForward, inout Complex V0, inout Complex V1, inout Complex V2, inout Complex V3,
	inout Complex V4, inout Complex V5, inout Complex V6, inout Complex V7)
{
	// Split into even and odd
	Radix4FFT(bIsForward, V0, V2, V4, V6);
	Radix4FFT(bIsForward, V1, V3, V5, V7);

	float c = 0.7071067811865475f;  // 1/sqrt(2)
	Complex Twiddle;
	if (bIsForward)
		Twiddle = Complex(c, c);
	else
		Twiddle = Complex(c, -c);

	Complex Rslt[8];
	Complex Tmp;

	Rslt[0] = V0 + V1;
	Rslt[4] = V0 - V1;

	Tmp = ComplexMult(Twiddle, V3);
	Rslt[1] = V2 + Tmp;
	Rslt[5] = V2 - Tmp;

	if (bIsForward) {
		Rslt[2] = Complex(V4.x - V5.y, V4.y + V5.x);
		Rslt[6] = Complex(V4.x + V5.y, V4.y - V5.x);
	} else {
		Rslt[2] = Complex(V4.x + V5.y, V4.y - V5.x);
		Rslt[6] = Complex(V4.x - V5.y, V4.y + V5.x);
	}

	Twiddle.x = -Twiddle.x;
	Tmp = ComplexMult(Twiddle, V7);
	Rslt[3] = V6 + Tmp;
	Rslt[7] = V6 - Tmp;

	V0 = Rslt[0];
	V1 = Rslt[1];
	V2 = Rslt[2];
	V3 = Rslt[3];
	V4 = Rslt[4];
	V5 = Rslt[5];
	V6 = Rslt[6];
	V7 = Rslt[7];
}

void RadixFFT(in bool bIsForward, inout Complex v[RADIX])
{
#if (RADIX == 2)
	Radix2FFT(bIsForward, v);
#elif (RADIX == 4)
	Radix4FFT(bIsForward, v);
#elif (RADIX == 8)
	Radix8FFT(bIsForward, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
#else
#	error "Unsupported RADIX (must be 2, 4, or 8)"
#endif
}

// --- Utility ---

// Returns Ns contiguous values, then skips R*Ns, then next Ns, etc.
// e.g. R=2, Ns=4: 0,1,2,3, 8,9,10,11, ...
uint Expand(in uint j, in uint Ns, in uint R)
{
	return (j / Ns) * Ns * R + (j % Ns);
}

// --- Group Shared Memory ---
// Single-buffer approach serializing real/imag with bank conflict avoidance

#define NUM_BANKS 32

groupshared float SharedReal[2 * SCAN_LINE_LENGTH];

void CopyLocalXToGroupShared(in Complex Local[RADIX], in uint Head, in uint Stride, in uint BankSkip)
{
	uint i = Head;
	[unroll] for (uint r = 0; r < RADIX; ++r, i += Stride)
	{
		uint j = i + (i / NUM_BANKS) * BankSkip;
		SharedReal[j] = Local[r].x;
	}
}

void CopyLocalYToGroupShared(in Complex Local[RADIX], in uint Head, in uint Stride, in uint BankSkip)
{
	uint i = Head;
	[unroll] for (uint r = 0; r < RADIX; ++r, i += Stride)
	{
		uint j = i + (i / NUM_BANKS) * BankSkip;
		SharedReal[j] = Local[r].y;
	}
}

void CopyGroupSharedToLocalX(inout Complex Local[RADIX], in uint Head, in uint Stride, in uint BankSkip)
{
	uint i = Head;
	[unroll] for (uint r = 0; r < RADIX; ++r, i += Stride)
	{
		uint j = i + (i / NUM_BANKS) * BankSkip;
		Local[r].x = SharedReal[j];
	}
}

void CopyGroupSharedToLocalY(inout Complex Local[RADIX], in uint Head, in uint Stride, in uint BankSkip)
{
	uint i = Head;
	[unroll] for (uint r = 0; r < RADIX; ++r, i += Stride)
	{
		uint j = i + (i / NUM_BANKS) * BankSkip;
		Local[r].y = SharedReal[j];
	}
}

// Exchange data between threads via shared memory transpose
void TransposeData(inout Complex Local[RADIX], uint AHead, uint AStride, uint BHead, uint BStride)
{
	uint BankSkip = (AStride < NUM_BANKS) ? AStride : 0;

	CopyLocalXToGroupShared(Local, AHead, AStride, BankSkip);
	FFTMemoryBarrier();
	CopyGroupSharedToLocalX(Local, BHead, BStride, BankSkip);
	FFTMemoryBarrier();
	CopyLocalYToGroupShared(Local, AHead, AStride, BankSkip);
	FFTMemoryBarrier();
	CopyGroupSharedToLocalY(Local, BHead, BStride, BankSkip);
}

// Twiddle factor application
void Butterfly(bool bIsForward, inout Complex Local[RADIX], uint ThreadIdx, uint Length)
{
	float angle = TWO_PI * float(ThreadIdx % Length) / float(Length * RADIX);
	if (!bIsForward)
		angle *= -1;

	Complex TwiddleInc;
	sincos(angle, TwiddleInc.y, TwiddleInc.x);
	Complex Twiddle = TwiddleInc;
	for (uint r = 1; r < RADIX; ++r) {
		Local[r] = ComplexMult(Twiddle, Local[r]);
		Twiddle = ComplexMult(Twiddle, TwiddleInc);
	}
}

// Mixed-radix final butterfly variants for non-clean radix decomposition
void ButterflyRadix4(bool bIsForward, inout Complex Local[RADIX], uint ThreadIdx, uint NumCols)
{
	float AngleEven = TWO_PI * float(ThreadIdx) / float(NumCols * RADIX);
	float AngleOdd = TWO_PI * float(ThreadIdx + NumCols) / float(NumCols * RADIX);
	if (!bIsForward) {
		AngleEven *= -1;
		AngleOdd *= -1;
	}

	Complex TwiddleIncEven, TwiddleIncOdd;
	sincos(AngleEven, TwiddleIncEven.y, TwiddleIncEven.x);
	sincos(AngleOdd, TwiddleIncOdd.y, TwiddleIncOdd.x);
	Complex TwiddleEven = TwiddleIncEven;
	Complex TwiddleOdd = TwiddleIncOdd;
	for (uint r = 2; r < RADIX; r += 2) {
		Local[r] = ComplexMult(TwiddleEven, Local[r]);
		TwiddleEven = ComplexMult(TwiddleEven, TwiddleIncEven);
		Local[r + 1] = ComplexMult(TwiddleOdd, Local[r + 1]);
		TwiddleOdd = ComplexMult(TwiddleOdd, TwiddleIncOdd);
	}
}

void ButterflyRadix2(bool bIsForward, inout Complex Local[RADIX], uint ThreadIdx, uint NumCols)
{
	float AngleStart = TWO_PI * float(ThreadIdx) / float(NumCols * RADIX);
	float AngleInc = TWO_PI * float(NumCols) / float(NumCols * RADIX);
	if (!bIsForward) {
		AngleStart *= -1;
		AngleInc *= -1;
	}

	Complex TwiddleInc;
	sincos(AngleInc, TwiddleInc.y, TwiddleInc.x);
	Complex Twiddle;
	sincos(AngleStart, Twiddle.y, Twiddle.x);
	for (uint r = 4; r < RADIX; ++r) {
		Local[r] = ComplexMult(Twiddle, Local[r]);
		Twiddle = ComplexMult(Twiddle, TwiddleInc);
	}
}

// --- Main Stockham FFT ---
// In-place FFT using group shared memory for a single signal
void GroupSharedFFT(in const bool bIsForward, inout Complex Local[RADIX], in const uint ArrayLength, in const uint ThreadIdx)
{
	uint NumCols = ArrayLength / RADIX;

	uint IdxS = ThreadIdx;
	uint IdxD = ThreadIdx * RADIX;

	// First radix pass
	RadixFFT(bIsForward, Local);

	// Exchange via shared memory
	TransposeData(Local, IdxD, 1, IdxS, NumCols);

	// Iterative Stockham passes
	uint Ns = RADIX;
	[loop] for (; Ns < NumCols; Ns *= RADIX)
	{
		Butterfly(bIsForward, Local, ThreadIdx, Ns);

		IdxD = Expand(ThreadIdx, Ns, RADIX);

		RadixFFT(bIsForward, Local);

		FFTMemoryBarrier();

		TransposeData(Local, IdxD, Ns, IdxS, NumCols);
	}

	// Final pass with mixed-radix handling
#if SCAN_LINE_LENGTH == 4096 || SCAN_LINE_LENGTH == 512 || SCAN_LINE_LENGTH == 64 || SCAN_LINE_LENGTH <= 8
	Butterfly(bIsForward, Local, ThreadIdx, Ns);
	RadixFFT(bIsForward, Local);
#elif SCAN_LINE_LENGTH == 2048 || SCAN_LINE_LENGTH == 256 || SCAN_LINE_LENGTH == 32
	ButterflyRadix4(bIsForward, Local, ThreadIdx, NumCols);
	Radix4FFT(bIsForward, Local[0], Local[2], Local[4], Local[6]);
	Radix4FFT(bIsForward, Local[1], Local[3], Local[5], Local[7]);
#elif SCAN_LINE_LENGTH == 1024 || SCAN_LINE_LENGTH == 128 || SCAN_LINE_LENGTH == 16
	ButterflyRadix2(bIsForward, Local, ThreadIdx, NumCols);
	Radix2FFT(bIsForward, Local[0], Local[4]);
	Radix2FFT(bIsForward, Local[1], Local[5]);
	Radix2FFT(bIsForward, Local[2], Local[6]);
	Radix2FFT(bIsForward, Local[3], Local[7]);
#else
#	error "Unsupported SCAN_LINE_LENGTH"
#endif

	FFTMemoryBarrier();
}

// Dual-channel FFT: transforms LocalBuffer[0] and LocalBuffer[1] simultaneously
void GroupSharedFFT(in const bool bIsForward, inout Complex LocalBuffer[2][RADIX], in const uint ArrayLength, in const uint ThreadIdx)
{
	GroupSharedFFT(bIsForward, LocalBuffer[0], ArrayLength, ThreadIdx);
	FFTMemoryBarrier();
	GroupSharedFFT(bIsForward, LocalBuffer[1], ArrayLength, ThreadIdx);
}

// Scale two complex signal buffers
void Scale(inout Complex LocalBuffer[2][RADIX], in float ScaleValue)
{
	[unroll] for (uint r = 0; r < RADIX; ++r)
	{
		LocalBuffer[0][r] *= ScaleValue;
		LocalBuffer[1][r] *= ScaleValue;
	}
}

// Scrub NaN/negative values (for image data)
void ScrubNANs(inout Complex LocalBuffer[2][RADIX])
{
	[unroll] for (uint r = 0; r < RADIX; ++r)
	{
		LocalBuffer[0][r] = -min(-LocalBuffer[0][r], Complex(0, 0));
		LocalBuffer[1][r] = -min(-LocalBuffer[1][r], Complex(0, 0));
	}
}
