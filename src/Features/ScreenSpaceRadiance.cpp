#include "ScreenSpaceRadiance.h"

#include "Deferred.h"
#include "State.h"
#include "Util.h"

winrt::com_ptr<ID3D11ComputeShader> ScreenSpaceRadiance::CompileResolve(bool a_quarter, bool a_specular, bool a_sh, bool a_fixedGrid)
{
	std::vector<std::pair<const char*, const char*>> defines{
		{ "SPARSE_MODE", a_quarter ? "2" : "1" },
		{ "SPARSE_PARITY", a_specular ? "1" : "0" }
	};
	if (a_sh)
		defines.emplace_back("SPARSE_SH", "");
	if (a_fixedGrid)
		defines.emplace_back("SPARSE_FIXED_GRID", "");
	winrt::com_ptr<ID3D11ComputeShader> shader;
	shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ResolveSparseRadiance.cs.hlsl", defines, "cs_5_0")));
	return shader;
}

void ScreenSpaceRadiance::Resolve(ID3D11ComputeShader* a_shader, Texture2D& a_input, Texture2D& a_output,
	Texture2D* a_inputSH1, Texture2D* a_outputSH1)
{
	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* srvs[] = {
		a_input.srv.get(), a_inputSH1 ? a_inputSH1->srv.get() : nullptr,
		Util::GetCurrentSceneDepthSRV(true), globals::game::renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS].SRV
	};
	ID3D11UnorderedAccessView* uavs[] = { a_output.uav.get(), a_outputSH1 ? a_outputSH1->uav.get() : nullptr };
	context->CSSetShaderResources(0, 4, srvs);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(a_shader, nullptr, 0);
	auto count = Util::GetScreenDispatchCount(true);
	context->Dispatch(count.x, count.y, 1);
	std::fill(std::begin(srvs), std::end(srvs), nullptr);
	std::fill(std::begin(uavs), std::end(uavs), nullptr);
	context->CSSetShaderResources(0, 4, srvs);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
}
