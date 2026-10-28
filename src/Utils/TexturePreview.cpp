#include "TexturePreview.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "Util.h"

#define I18N_KEY_PREFIX "common.texture_preview."

void TexturePreview::Draw(ID3D11ShaderResourceView* srv, const char* id, const char* label, float scale, bool scalar, float2 defaultRange)
{
	ImGui::PushID(id);
	if (!ImGui::TreeNode("texture", "%s", label)) {
		ImGui::PopID();
		return;
	}
	const auto draw = [&]() {
		if (!srv) {
			ImGui::TextUnformatted(T(TKEY("debug_texture_unavailable"), "Not loaded or not generated yet."));
			return;
		}
		if (!requested) {
			requested = true;
			shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexturePreview.cs.hlsl", {}, "cs_5_0")));
			constants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<Parameters>(), "TexturePreview::Parameters");
		}
		if (!shader || !constants) {
			ImGui::TextUnformatted(T(TKEY("debug_texture_shader_missing"), "Texture preview shader is unavailable."));
			return;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
		srv->GetDesc(&viewDesc);
		winrt::com_ptr<ID3D11Resource> resource;
		srv->GetResource(resource.put());
		uint32_t width, height, depth = 1, levels, firstMip;
		const bool volume = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D;
		if (volume) {
			D3D11_TEXTURE3D_DESC desc;
			resource.as<ID3D11Texture3D>()->GetDesc(&desc);
			firstMip = viewDesc.Texture3D.MostDetailedMip;
			levels = std::min(viewDesc.Texture3D.MipLevels, desc.MipLevels - firstMip);
			width = std::max(desc.Width >> firstMip, 1u);
			height = std::max(desc.Height >> firstMip, 1u);
			depth = std::max(desc.Depth >> firstMip, 1u);
		} else if (viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
			D3D11_TEXTURE2D_DESC desc;
			resource.as<ID3D11Texture2D>()->GetDesc(&desc);
			firstMip = viewDesc.Texture2D.MostDetailedMip;
			levels = std::min(viewDesc.Texture2D.MipLevels, desc.MipLevels - firstMip);
			width = std::max(desc.Width >> firstMip, 1u);
			height = std::max(desc.Height >> firstMip, 1u);
		} else {
			ImGui::TextUnformatted(T(TKEY("debug_texture_unsupported"), "This preview requires a 2D or 3D texture."));
			return;
		}
		auto [it, inserted] = views.try_emplace(id);
		auto& view = it->second;
		if (inserted) {
			view.channel = scalar ? 1 : 0;
			view.range = defaultRange;
		}
		view.mip = std::clamp(view.mip, 0, static_cast<int>(levels) - 1);
		ImGui::SliderInt(T(TKEY("debug_texture_mip"), "Mip level"), &view.mip, 0, static_cast<int>(levels) - 1);
		width = std::max(width >> view.mip, 1u);
		height = std::max(height >> view.mip, 1u);
		depth = std::max(depth >> view.mip, 1u);
		view.slice = std::clamp(view.slice, 0, static_cast<int>(depth) - 1);
		if (volume)
			ImGui::SliderInt(T(TKEY("debug_texture_slice"), "Z slice"), &view.slice, 0, static_cast<int>(depth) - 1);
		const char* channels[] = { "RGB", "R", "G", "B", "A" };
		ImGui::Combo(T(TKEY("debug_texture_channel"), "Channel"), &view.channel, channels, IM_ARRAYSIZE(channels));
		ImGui::DragFloat2(T(TKEY("debug_texture_range"), "Display range"), &view.range.x, 0.01f);
		if (ImGui::Button(T(TKEY("debug_texture_reset_range"), "Reset display range")))
			view.range = defaultRange;
		ImGui::Text(T(TKEY("debug_texture_dimensions"), "%u x %u x %u"), width, height, depth);
		const ImVec2 displaySize{ width * scale, height * scale };
		if (!ImGui::IsRectVisible(displaySize)) {
			ImGui::Dummy(displaySize);
			return;
		}
		if (!view.texture || view.texture->desc.Width != width || view.texture->desc.Height != height) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			view.texture = eastl::make_unique<Texture2D>(desc, "TexturePreview");
			view.texture->CreateSRV({ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
			view.texture->CreateUAV({ .Format = desc.Format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
		}
		const float minimum = std::isfinite(view.range.x) ? view.range.x : 0.f;
		const float maximum = std::isfinite(view.range.y) ? std::max(view.range.y, minimum + 1e-6f) : minimum + 1.f;
		const Parameters data{ static_cast<uint32_t>(view.mip), static_cast<uint32_t>(view.slice),
			static_cast<uint32_t>(view.channel), volume ? 1u : 0u, { minimum, maximum } };
		constants->Update(data);
		auto* context = globals::d3d::context;
		winrt::com_ptr<ID3D11ComputeShader> oldShader;
		winrt::com_ptr<ID3D11Buffer> oldCb;
		winrt::com_ptr<ID3D11UnorderedAccessView> oldUav;
		ID3D11ShaderResourceView* oldSrvs[2] = {};
		context->CSGetShader(oldShader.put(), nullptr, nullptr);
		context->CSGetConstantBuffers(1, 1, oldCb.put());
		context->CSGetShaderResources(0, 2, oldSrvs);
		context->CSGetUnorderedAccessViews(0, 1, oldUav.put());
		ID3D11ShaderResourceView* inputs[2] = { volume ? nullptr : srv, volume ? srv : nullptr };
		auto* cb = constants->CB();
		auto* output = view.texture->uav.get();
		ID3D11UnorderedAccessView* nullUav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetConstantBuffers(1, 1, &cb);
		context->CSSetShaderResources(0, 2, inputs);
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->CSSetShader(shader.get(), nullptr, 0);
		context->Dispatch((width + 7u) >> 3, (height + 7u) >> 3, 1);
		output = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->CSSetShaderResources(0, 2, oldSrvs);
		output = oldUav.get();
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		cb = oldCb.get();
		context->CSSetConstantBuffers(1, 1, &cb);
		context->CSSetShader(oldShader.get(), nullptr, 0);
		for (auto* input : oldSrvs)
			if (input)
				input->Release();
		ImGui::Image(view.texture->srv.get(), displaySize);
	};
	draw();
	ImGui::TreePop();
	ImGui::PopID();
}

#undef I18N_KEY_PREFIX

void TexturePreview::ClearShaderCache()
{
	shader = nullptr;
	requested = false;
}
