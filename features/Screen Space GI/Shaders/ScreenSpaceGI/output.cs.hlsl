Texture2D<half4> srcGI : register(t0);
Texture2D<half4> srcAlbedo : register(t1);

RWTexture2D<half4> outGI : register(u0);
RWTexture2D<half3> outGIAlbedo : register(u1);

[numthreads(8, 8, 1)] void main(uint2 dtid
								: SV_DispatchThreadID) {
	half4 o = outGI[dtid];
	half4 i = srcGI[dtid];
	half3 gi = i.rgb * srcAlbedo[dtid].rgb;
	o.rgb += gi;
	o.w *= i.w;
	outGI[dtid] = o;
#ifdef GI_BOUNCE
	outGIAlbedo[dtid] = gi;
#endif
}