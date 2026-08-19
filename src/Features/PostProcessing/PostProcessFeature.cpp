#include "PostProcessFeature.h"

void PostProcessFeature::CompileComputeShadersAsync(std::wstring_view sourceDir, std::span<const ComputeShaderCompileInfo> infos)
{
	for (const auto& info : infos) {
		auto path = std::filesystem::path(sourceDir) / info.filename;
		globals::shaderCache->EnqueueComputeShaderCompile(
			path.wstring(), info.entry, info.defines,
			[weak = weak_from_this(), ptr = info.programPtr, generation = shaderGeneration.load(std::memory_order_relaxed)](ID3D11ComputeShader* shader) {
				if (!shader)
					return;
				auto self = weak.lock();
				if (!self) {
					shader->Release();
					return;
				}
				std::lock_guard lock(self->shaderMutex);
				if (self->shaderGeneration.load(std::memory_order_relaxed) != generation) {
					shader->Release();
					return;
				}
				ptr->attach(shader);
			});
	}
}

bool PostProcessFeature::AllShadersReady(std::initializer_list<const winrt::com_ptr<ID3D11ComputeShader>*> shaders) const
{
	std::lock_guard lock(shaderMutex);
	for (auto* shader : shaders) {
		if (!*shader)
			return false;
	}
	return true;
}
