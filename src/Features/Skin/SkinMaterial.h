#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>

namespace SkinMaterials
{
	inline constexpr size_t ParameterCount = 20;
	inline constexpr size_t MaxPayloadBytes = 64 * 1024;
	inline constexpr size_t MaxPackageBytes = 4 * 1024 * 1024;
	inline constexpr size_t MaxEntries = 4096;

	struct Parameters
	{
#define SKIN_FLOAT(name, value, min, max) float name = value;
#define SKIN_BOOL(name, value) bool name = value;
#include "SkinParameters.def"
#undef SKIN_FLOAT
#undef SKIN_BOOL
	};

	struct ParameterInfo
	{
		const char* name;
		float Parameters::* number;
		bool Parameters::* toggle;
		double minimum;
		double maximum;
	};

	using Patch = std::array<std::optional<double>, ParameterCount>;
	std::span<const ParameterInfo, ParameterCount> ParameterTable();
	json ToJSON(const Parameters& a_parameters);
	json ToJSON(const Patch& a_patch);
	Patch ReadParameters(const json& a_json, bool a_complete);
	void Apply(Parameters& a_parameters, const Patch& a_patch);

	struct Material
	{
		std::string name;
		Parameters parameters;
		std::string rfaos;
		std::string wetness;
	};

	struct FormKey
	{
		std::string plugin;
		uint32_t localID = 0;
		std::string Key() const;
		bool operator==(const FormKey&) const = default;
	};

	struct Target
	{
		std::string surfaceID;
		std::optional<FormKey> txst;
		std::optional<FormKey> npc;
		std::string Key() const;
		bool HasSurface() const { return !surfaceID.empty() || txst.has_value(); }
	};

	struct UserEdit
	{
		Target target;
		std::optional<Material> material;
		Patch parameters;
	};

	struct NifMaterial
	{
		std::string surfaceID;
		std::optional<Material> material;
		std::string diagnostic;
	};

	struct Resolution
	{
		Material base;
		Parameters parameters;
		std::string source;
		std::vector<std::string> adjustments;
	};

	json Parse(std::string_view a_text);
	json ToJSON(const Material& a_material);
	json ToJSON(const FormKey& a_key);
	json ToJSON(const Target& a_target);
	Material ReadMaterial(const json& a_json);
	FormKey ReadFormKey(const json& a_json);
	std::optional<FormKey> GetFormKey(const RE::TESForm* a_form);
	RE::TESForm* ResolveForm(const FormKey& a_key, RE::FormType a_type);
	NifMaterial ReadNif(RE::BSGeometry* a_geometry);
	std::string NormalizeTexturePath(std::string a_path);
	bool IsSurfaceID(std::string_view a_id);
	void WriteJSON(const std::filesystem::path& a_path, const json& a_json);

	class Store
	{
	public:
		void Refresh(bool a_force = false);
		Resolution Resolve(const NifMaterial& a_nif, RE::FormID a_txst, RE::FormID a_race,
			RE::FormID a_npc, const Target& a_surface, const Parameters& a_default, const UserEdit* a_preview = nullptr) const;
		void SaveEdit(const UserEdit& a_edit);
		void RemoveEdit(const Target& a_target, bool a_material);
		void DeleteEdit(const Target& a_target);
		const UserEdit* FindEdit(const Target& a_target) const;
		uint64_t Revision() const { return revision; }
		const std::vector<std::string>& Diagnostics() const { return diagnostics; }
		const std::map<std::string, UserEdit>& UserEdits() const { return userEdits; }

	private:
		struct FileSnapshot
		{
			std::filesystem::file_time_type time;
			json data;
			std::string error;
		};
		struct RecordMaterial
		{
			Material material;
			std::string source;
		};
		struct Adjustment
		{
			Patch parameters;
			std::string source;
		};
		std::map<std::string, FileSnapshot> files;
		std::unordered_map<RE::FormID, RecordMaterial> records;
		std::unordered_map<RE::FormID, Adjustment> races;
		std::unordered_map<RE::FormID, Adjustment> npcs;
		std::map<std::string, UserEdit> userEdits;
		std::vector<std::string> diagnostics;
		uint64_t revision = 1;
		void Rebuild();
		void SaveUsers(const std::map<std::string, UserEdit>& a_edits);
	};
}
