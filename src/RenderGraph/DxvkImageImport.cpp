// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/DxvkImageImport.h"

#include <Resources/ExternalTextureResource.h>
#include <Resources/TextureDescription.h>

namespace RenderGraph
{
	std::optional<rhi::ResourceLayout> HomeLayoutOf(VkImageLayout a_layout)
	{
		switch (a_layout) {
		case VK_IMAGE_LAYOUT_GENERAL:
			return rhi::ResourceLayout::Common;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return rhi::ResourceLayout::ShaderResource;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return rhi::ResourceLayout::RenderTarget;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			return rhi::ResourceLayout::DepthReadWrite;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
			return rhi::ResourceLayout::DepthRead;
		default:
			return std::nullopt;
		}
	}

	rhi::ResourcePtr ImportDxvkImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, const char* a_name)
	{
		if (!HomeLayoutOf(a_image.layout))
			return {};
		rhi::vulkan::ImportedImageDesc import{};
		import.image = a_image.image;
		import.createInfo.flags = a_image.flags;
		import.createInfo.imageType = a_image.type;
		import.createInfo.format = a_image.format;
		import.createInfo.extent = a_image.extent;
		import.createInfo.mipLevels = a_image.mipLevels;
		import.createInfo.arrayLayers = a_image.arrayLayers;
		import.createInfo.samples = a_image.samples;
		import.createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		import.createInfo.usage = a_image.usage;
		import.createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		import.createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		import.currentLayout = a_image.layout;
		// In GENERAL, DXVK and the graph share the image with memory barriers alone. In any other layout the graph uses it
		// exclusively and moves it between layouts, starting from and returning to this one.
		import.simultaneousAccess = a_image.layout == VK_IMAGE_LAYOUT_GENERAL;
		import.debugName = a_name;
		rhi::ResourcePtr resource;
		if (rhi::vulkan::import_image(a_device, import, resource) != rhi::Result::Ok)
			return {};
		return resource;
	}

	std::shared_ptr<org::ExternalTextureResource> ImportDxvkTexture(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image,
		org::TextureDescription a_desc, const char* a_name)
	{
		const auto home = HomeLayoutOf(a_image.layout);
		if (!home)
			return nullptr;
		auto resource = ImportDxvkImage(a_device, a_image, a_name);
		if (!resource)
			return nullptr;
		// The home state: the graph finds the image in it and gives it back in it (external home states).
		a_desc.initialLayout = *home;
		auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, *home == rhi::ResourceLayout::Common);
		if (result)
			result->SetName(a_name);
		return result;
	}
}
