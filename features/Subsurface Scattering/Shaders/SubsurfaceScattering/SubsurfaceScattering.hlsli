struct PerPassSSS
{
	uint ValidMaterial;
	uint IsBeastRace;
	uint pad0[2];
};

StructuredBuffer<PerPassSSS> perPassSSS : register(t36);