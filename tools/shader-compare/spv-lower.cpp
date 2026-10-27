// spv-lower: BasicRHI's descriptor-buffer lowering (rhi_vulkan_spirv_lowering.h, LowerForDescriptorBuffer) run offline on a module the
// game dumped (BASICRHI_VK_DUMP_LOWERED=<dir>: <n>_<name>_<stage>.in.spv and its .plan.txt), so a change to how a layout's bindings
// are sourced is measured (spv-pipeline-stats) without the game.
//
//   spv-lower <in.spv> <plan.txt> <out.spv> [--source <from>=<to> ...]
//
// --source rewrites every mapping of one BindingSource to another before lowering, keeping its push and record offsets (sources by
// name: Heap, HeapIndirect, BufferIndirectAddress, BufferPushAddress, PushData, StaticSampler); e.g. BufferIndirectAddress=HeapIndirect
// reads each uniform block as a heap descriptor whose index is in the record, as a layout with IndirectIndex constant buffer ranges
// would.
#include "rhi_vulkan_spirv_lowering.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	using rhi::vulkan_binding::BindingSource;

	const std::map<std::string, BindingSource> kSources = { { "Heap", BindingSource::Heap }, { "HeapIndirect", BindingSource::HeapIndirect },
		{ "BufferIndirectAddress", BindingSource::BufferIndirectAddress }, { "BufferPushAddress", BindingSource::BufferPushAddress },
		{ "PushData", BindingSource::PushData }, { "StaticSampler", BindingSource::StaticSampler } };

	std::vector<std::uint32_t> ReadWords(const char* a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::vector<std::uint32_t> words(bytes.size() / 4);
		std::memcpy(words.data(), bytes.data(), words.size() * 4);
		return words;
	}
}

int main(int a_argc, char** a_argv)
{
	if (a_argc < 4) {
		std::cerr << "usage: spv-lower <in.spv> <plan.txt> <out.spv> [--source <from>=<to> ...]\n";
		return 2;
	}
	std::map<BindingSource, BindingSource> rewrites;
	for (int i = 4; i + 1 < a_argc; i += 2) {
		const std::string rule = a_argv[i + 1];
		const auto equals = rule.find('=');
		if (std::string(a_argv[i]) != "--source" || equals == std::string::npos || !kSources.contains(rule.substr(0, equals)) ||
			!kSources.contains(rule.substr(equals + 1))) {
			std::cerr << "bad option: " << a_argv[i] << " " << rule << "\n";
			return 2;
		}
		rewrites[kSources.at(rule.substr(0, equals))] = kSources.at(rule.substr(equals + 1));
	}
	const auto input = ReadWords(a_argv[1]);
	rhi::vulkan_binding::BindingPlan plan;
	rhi::vulkan_binding::LoweringTarget target;
	std::vector<std::uint32_t> staticSamplers;
	std::uint32_t stage = 0;
	std::string entry;
	std::ifstream planFile(a_argv[2]);
	for (std::string line; std::getline(planFile, line);) {
		std::istringstream in(line);
		std::string kind;
		in >> kind;
		if (kind == "stage")
			in >> stage;
		else if (kind == "entry")
			in >> entry;
		else if (kind == "target")
			in >> target.pushedBytes >> target.spillAddressOffset >> target.pushConstantBytes >> target.recordAlignment >> target.uniformBlockAlignment >>
				target.spillAlignment;
		else if (kind == "plan")
			in >> plan.pushDataBytes;
		else if (kind == "mapping") {
			rhi::vulkan_binding::BindingMapping m;
			std::uint32_t mask = 0, source = 0;
			in >> m.set >> m.firstBinding >> m.bindingCount >> mask >> source >> m.heapElement >> m.pushOffset >> m.recordOffset >> m.pushDataBytes >> m.staticSampler;
			m.resourceMask = mask;
			m.source = static_cast<BindingSource>(source);
			if (const auto found = rewrites.find(m.source); found != rewrites.end())
				m.source = found->second;
			plan.mappings.push_back(m);
		} else if (kind == "staticSamplers")
			for (std::uint32_t e; in >> e;)
				staticSamplers.push_back(e);
	}
	target.staticSamplerElements = staticSamplers;
	std::vector<std::uint32_t> lowered;
	std::string error;
	if (!rhi::vulkan_binding::LowerForDescriptorBuffer(input, plan, static_cast<VkShaderStageFlagBits>(stage), target, lowered, error, entry)) {
		std::cerr << "lowering failed: " << error << "\n";
		return 1;
	}
	std::ofstream(a_argv[3], std::ios::binary).write(reinterpret_cast<const char*>(lowered.data()), static_cast<std::streamsize>(lowered.size() * 4));
	std::cout << "lowered " << input.size() << " -> " << lowered.size() << " words\n";
	return 0;
}
