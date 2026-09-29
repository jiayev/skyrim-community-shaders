#pragma once

/**
 * @file LocationTypeIcons.h
 * @brief Font Awesome 5 glyphs for vanilla LocType* keywords.
 *
 * Locations don't use a fixed DATA bitfield like weather — they carry BGSKeyword
 * forms whose editor IDs start with "LocType". A single location may hold several;
 * callers that want "one icon" should pick a primary (e.g. dungeon-ish over
 * habitation, or the first match in kLocationTypeIcons order).
 *
 * Coverage is solid in FA5 solid: there is a literal dungeon, city skyline, dragon,
 * skull, church, storefront, campground, ship, etc. A few types reuse a close
 * neighbour (castle → rook, jail → lock, farm → tractor) rather than inventing
 * custom art.
 */

#include "IconsFontAwesome5.h"

#include <string_view>

namespace LocationTypeIcons
{
	struct Entry
	{
		std::string_view editorId;  ///< Full KYWD editor ID, e.g. "LocTypeDungeon"
		const char* icon;           ///< ICON_FA_* UTF-8 glyph
		std::string_view label;     ///< Short English label (editor-ID suffix, spaced)
	};

	// ---------------------------------------------------------------------------
	// Settlements & holds
	// ---------------------------------------------------------------------------
	inline constexpr Entry kCity{ "LocTypeCity", ICON_FA_CITY, "City" };
	inline constexpr Entry kTown{ "LocTypeTown", ICON_FA_HOME, "Town" };
	inline constexpr Entry kSettlement{ "LocTypeSettlement", ICON_FA_USERS, "Settlement" };
	inline constexpr Entry kHabitation{ "LocTypeHabitation", ICON_FA_HOME, "Habitation" };
	inline constexpr Entry kHabitationHasInn{ "LocTypeHabitationHasInn", ICON_FA_HOTEL, "Habitation (Inn)" };
	inline constexpr Entry kHold{ "LocTypeHold", ICON_FA_FLAG, "Hold" };
	inline constexpr Entry kHoldCapital{ "LocTypeHoldCapital", ICON_FA_CROWN, "Hold Capital" };
	inline constexpr Entry kHoldMajor{ "LocTypeHoldMajor", ICON_FA_LANDMARK, "Hold Major" };
	inline constexpr Entry kHoldMinor{ "LocTypeHoldMinor", ICON_FA_MAP_MARKER_ALT, "Hold Minor" };

	// ---------------------------------------------------------------------------
	// Buildings & civic
	// ---------------------------------------------------------------------------
	inline constexpr Entry kHouse{ "LocTypeHouse", ICON_FA_HOME, "House" };
	inline constexpr Entry kPlayerHouse{ "LocTypePlayerHouse", ICON_FA_HOUSE_USER, "Player House" };
	inline constexpr Entry kDwelling{ "LocTypeDwelling", ICON_FA_HOME, "Dwelling" };
	inline constexpr Entry kStewardsDwelling{ "LocTypeStewardsDwelling", ICON_FA_HOME, "Steward's Dwelling" };
	inline constexpr Entry kInn{ "LocTypeInn", ICON_FA_BED, "Inn" };
	inline constexpr Entry kStore{ "LocTypeStore", ICON_FA_STORE, "Store" };
	inline constexpr Entry kTemple{ "LocTypeTemple", ICON_FA_CHURCH, "Temple" };
	inline constexpr Entry kGuild{ "LocTypeGuild", ICON_FA_UNIVERSITY, "Guild" };
	inline constexpr Entry kCastle{ "LocTypeCastle", ICON_FA_CHESS_ROOK, "Castle" };
	inline constexpr Entry kBarracks{ "LocTypeBarracks", ICON_FA_SHIELD_ALT, "Barracks" };
	inline constexpr Entry kJail{ "LocTypeJail", ICON_FA_LOCK, "Jail" };
	inline constexpr Entry kCemetery{ "LocTypeCemetery", ICON_FA_CROSS, "Cemetery" };

	// ---------------------------------------------------------------------------
	// Work sites & military
	// ---------------------------------------------------------------------------
	inline constexpr Entry kFarm{ "LocTypeFarm", ICON_FA_TRACTOR, "Farm" };
	inline constexpr Entry kLumberMill{ "LocTypeLumberMill", ICON_FA_TREE, "Lumber Mill" };
	inline constexpr Entry kMine{ "LocTypeMine", ICON_FA_HARD_HAT, "Mine" };
	inline constexpr Entry kMilitaryCamp{ "LocTypeMilitaryCamp", ICON_FA_CAMPGROUND, "Military Camp" };
	inline constexpr Entry kMilitaryFort{ "LocTypeMilitaryFort", ICON_FA_CHESS_ROOK, "Military Fort" };

	// ---------------------------------------------------------------------------
	// Dungeons & hostile sites
	// ---------------------------------------------------------------------------
	inline constexpr Entry kDungeon{ "LocTypeDungeon", ICON_FA_DUNGEON, "Dungeon" };
	inline constexpr Entry kClearable{ "LocTypeClearable", ICON_FA_SKULL_CROSSBONES, "Clearable" };
	inline constexpr Entry kDraugrCrypt{ "LocTypeDraugrCrypt", ICON_FA_SKULL, "Draugr Crypt" };
	inline constexpr Entry kDragonLair{ "LocTypeDragonLair", ICON_FA_DRAGON, "Dragon Lair" };
	inline constexpr Entry kDragonPriestLair{ "LocTypeDragonPriestLair", ICON_FA_HAT_WIZARD, "Dragon Priest Lair" };
	inline constexpr Entry kBanditCamp{ "LocTypeBanditCamp", ICON_FA_CAMPGROUND, "Bandit Camp" };
	inline constexpr Entry kForswornCamp{ "LocTypeForswornCamp", ICON_FA_CAMPGROUND, "Forsworn Camp" };
	inline constexpr Entry kGiantCamp{ "LocTypeGiantCamp", ICON_FA_MOUNTAIN, "Giant Camp" };
	inline constexpr Entry kFalmerHive{ "LocTypeFalmerHive", ICON_FA_SPIDER, "Falmer Hive" };
	inline constexpr Entry kHagravenNest{ "LocTypeHagravenNest", ICON_FA_CROW, "Hagraven Nest" };
	inline constexpr Entry kVampireLair{ "LocTypeVampireLair", ICON_FA_MOON, "Vampire Lair" };
	inline constexpr Entry kWarlockLair{ "LocTypeWarlockLair", ICON_FA_HAT_WIZARD, "Warlock Lair" };
	inline constexpr Entry kWerewolfLair{ "LocTypeWerewolfLair", ICON_FA_PAW, "Werewolf Lair" };
	inline constexpr Entry kAnimalDen{ "LocTypeAnimalDen", ICON_FA_PAW, "Animal Den" };
	inline constexpr Entry kSprigganGrove{ "LocTypeSprigganGrove", ICON_FA_LEAF, "Spriggan Grove" };
	inline constexpr Entry kDwarvenAutomatons{ "LocTypeDwarvenAutomatons", ICON_FA_ROBOT, "Dwarven Automatons" };
	inline constexpr Entry kOrcStronghold{ "LocTypeOrcStronghold", ICON_FA_HAMMER, "Orc Stronghold" };

	// ---------------------------------------------------------------------------
	// Waterborne
	// ---------------------------------------------------------------------------
	inline constexpr Entry kShip{ "LocTypeShip", ICON_FA_SHIP, "Ship" };
	inline constexpr Entry kShipwreck{ "LocTypeShipwreck", ICON_FA_ANCHOR, "Shipwreck" };

	/** @brief Fallback when the keyword isn't a known LocType (or is mod-added). */
	inline constexpr const char* kUnknownIcon = ICON_FA_MAP_MARKER_ALT;

	/**
	 * @brief How specific a LocType is when a location carries several.
	 * Higher wins — dungeon descriptors beat City/Habitation/Clearable so Bleak Falls
	 * shows a dungeon glyph rather than a generic habitation or clearable mark.
	 */
	[[nodiscard]] inline int GetLocationTypeIconPriority(std::string_view editorId) noexcept
	{
		// Hostile / dungeon-specific
		if (editorId == kDraugrCrypt.editorId || editorId == kDragonLair.editorId ||
			editorId == kDragonPriestLair.editorId || editorId == kVampireLair.editorId ||
			editorId == kWarlockLair.editorId || editorId == kWerewolfLair.editorId ||
			editorId == kFalmerHive.editorId || editorId == kHagravenNest.editorId ||
			editorId == kSprigganGrove.editorId || editorId == kAnimalDen.editorId ||
			editorId == kDwarvenAutomatons.editorId)
			return 100;
		if (editorId == kDungeon.editorId || editorId == kBanditCamp.editorId ||
			editorId == kForswornCamp.editorId || editorId == kGiantCamp.editorId ||
			editorId == kOrcStronghold.editorId || editorId == kMine.editorId)
			return 90;
		if (editorId == kShipwreck.editorId || editorId == kShip.editorId)
			return 80;
		// Buildings & civic
		if (editorId == kCastle.editorId || editorId == kTemple.editorId ||
			editorId == kJail.editorId || editorId == kBarracks.editorId ||
			editorId == kGuild.editorId || editorId == kMilitaryFort.editorId)
			return 70;
		if (editorId == kInn.editorId || editorId == kStore.editorId ||
			editorId == kPlayerHouse.editorId || editorId == kHouse.editorId ||
			editorId == kDwelling.editorId || editorId == kStewardsDwelling.editorId ||
			editorId == kCemetery.editorId || editorId == kFarm.editorId ||
			editorId == kLumberMill.editorId || editorId == kMilitaryCamp.editorId)
			return 60;
		// Settlements & holds
		if (editorId == kCity.editorId || editorId == kTown.editorId ||
			editorId == kHoldCapital.editorId)
			return 50;
		if (editorId == kSettlement.editorId || editorId == kHoldMajor.editorId ||
			editorId == kHoldMinor.editorId || editorId == kHold.editorId)
			return 40;
		if (editorId == kHabitationHasInn.editorId || editorId == kHabitation.editorId)
			return 30;
		// Nearly every dungeon is also Clearable — keep it weakest so it never steals the icon.
		if (editorId == kClearable.editorId)
			return 10;
		return 0;
	}

	/**
	 * @brief All known LocType mappings, roughly broad → specific within each group.
	 * Order is stable for UI lists; LookupLocationTypeIcon does a linear scan.
	 */
	inline constexpr Entry kLocationTypeIcons[] = {
		// Settlements & holds
		kCity,
		kTown,
		kSettlement,
		kHabitation,
		kHabitationHasInn,
		kHold,
		kHoldCapital,
		kHoldMajor,
		kHoldMinor,
		// Buildings & civic
		kHouse,
		kPlayerHouse,
		kDwelling,
		kStewardsDwelling,
		kInn,
		kStore,
		kTemple,
		kGuild,
		kCastle,
		kBarracks,
		kJail,
		kCemetery,
		// Work sites & military
		kFarm,
		kLumberMill,
		kMine,
		kMilitaryCamp,
		kMilitaryFort,
		// Dungeons & hostile sites
		kDungeon,
		kClearable,
		kDraugrCrypt,
		kDragonLair,
		kDragonPriestLair,
		kBanditCamp,
		kForswornCamp,
		kGiantCamp,
		kFalmerHive,
		kHagravenNest,
		kVampireLair,
		kWarlockLair,
		kWerewolfLair,
		kAnimalDen,
		kSprigganGrove,
		kDwarvenAutomatons,
		kOrcStronghold,
		// Waterborne
		kShip,
		kShipwreck,
	};

	/** @brief FA glyph for a LocType editor ID; kUnknownIcon if unrecognised. */
	[[nodiscard]] inline const char* LookupLocationTypeIcon(std::string_view editorId) noexcept
	{
		for (const auto& entry : kLocationTypeIcons) {
			if (entry.editorId == editorId)
				return entry.icon;
		}
		return kUnknownIcon;
	}

	/** @brief Short label for a LocType editor ID; empty if unrecognised. */
	[[nodiscard]] inline std::string_view LookupLocationTypeLabel(std::string_view editorId) noexcept
	{
		for (const auto& entry : kLocationTypeIcons) {
			if (entry.editorId == editorId)
				return entry.label;
		}
		return {};
	}

	/**
	 * @brief Best single glyph when a place carries multiple LocType keywords.
	 * @param editorIds LocType editor IDs (e.g. from BGSLocation::GetKeywords).
	 * @return Highest-priority known icon, or nullptr if none matched.
	 */
	template <class Range>
	[[nodiscard]] const char* ResolvePrimaryLocationTypeIcon(const Range& editorIds) noexcept
	{
		const char* bestIcon = nullptr;
		int bestPriority = -1;
		for (const auto& id : editorIds) {
			const std::string_view editorId = id;
			const int priority = GetLocationTypeIconPriority(editorId);
			if (priority <= bestPriority)
				continue;
			const char* icon = LookupLocationTypeIcon(editorId);
			if (icon == kUnknownIcon && priority == 0)
				continue;
			bestPriority = priority;
			bestIcon = icon;
		}
		return bestIcon;
	}
}
