struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut MainVS(uint id : SV_VertexID)
{
    VSOut o;

	// Generate fullscreen triangle using vertex ID
	// Vertex 0: (-1, -1) -> (0, 1) UV
	// Vertex 1: (-1,  3) -> (0, -1) UV
	// Vertex 2: ( 3, -1) -> (2, 1) UV
    o.pos.x = (float) (id / 2) * 4.0 - 1.0;
    o.pos.y = (float) (id % 2) * 4.0 - 1.0;
    o.pos.z = 0.0;
    o.pos.w = 1.0;

    o.uv.x = (float) (id / 2) * 2.0;
    o.uv.y = 1.0 - (float) (id % 2) * 2.0;
    
    return o;
}

Texture2D<float> DepthTex : register(t0);
Texture2D<float4> MotionTex : register(t1);

SamplerState s0 : register(s0);

struct PSOut
{
    float2 motion : SV_Target0;
    float depth : SV_Depth;
};

PSOut MainPS(float4 pos : SV_Position, float2 uv : TEXCOORD0)
{
    PSOut o;

    // Load is better than Sample for exact copy
    int2 pixel = int2(pos.xy);

    o.depth = DepthTex.Load(int3(pixel, 0));
    o.motion = MotionTex.Load(int3(pixel, 0)).xy;

    return o;
}