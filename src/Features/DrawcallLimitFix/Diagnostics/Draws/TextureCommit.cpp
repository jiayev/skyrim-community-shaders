// The main commit's frame textures (always on): a register the drawn pipelines read that the commit could not resolve, described
// once per register (the view's dimension and format; a buffer's size, stride, usage and flags; the resource's debug name).

#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	void DescribeUnresolvedFrameTexture(const FrameTextureUnresolved& a_unresolved)
	{
		const std::uint32_t t = a_unresolved.textureRegister;
		static std::array<bool, kTextureRegisters> described{};
		if (std::exchange(described[t], true))
			return;
		if (auto* view = a_unresolved.view) {
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			view->GetDesc(&viewDesc);
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			std::string what = fmt::format("view dimension {}, format {}", static_cast<std::uint32_t>(viewDesc.ViewDimension), static_cast<std::uint32_t>(viewDesc.Format));
			if (auto buffer = resource.try_as<ID3D11Buffer>()) {
				D3D11_BUFFER_DESC bufferDesc{};
				buffer->GetDesc(&bufferDesc);
				what += fmt::format(", buffer of {} bytes, stride {}, usage {}, CPU access {:X}, bind {:X}, misc {:X}", bufferDesc.ByteWidth, bufferDesc.StructureByteStride,
					static_cast<std::uint32_t>(bufferDesc.Usage), bufferDesc.CPUAccessFlags, bufferDesc.BindFlags, bufferDesc.MiscFlags);
			}
			char name[128]{};
			UINT size = sizeof(name) - 1;
			if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
				what += fmt::format(", '{}'", name);
			logger::warn("[DCLF] frame texture t{} is bound natively but not resolved: {}", t, what);
		} else {
			logger::warn("[DCLF] frame texture t{} is read but nothing is bound there at the capture", t);
		}
	}
}
