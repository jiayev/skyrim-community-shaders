struct LightingData
{
	float WaterHeight[25];
	bool Reflections;
};

StructuredBuffer<LightingData> lightingData : register(t126);
