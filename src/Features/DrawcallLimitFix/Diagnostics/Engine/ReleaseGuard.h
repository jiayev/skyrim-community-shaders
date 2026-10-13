#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace DCLF
{
	/**
	 * @brief CS_DCLF_RELEASE_GUARD=1: an observer of the engine's last releases on DCLF's own threads. Dropping an engine
	 * object's last reference runs its destructors, which belong on the engine's threads (a node's takes its collision object out of the
	 * Havok world); the scene lane and the pool hand their references back instead (the retirement chain,
	 * EngineReleases). With the switch, NiRefObject::DeleteThis (AE 0x140d27520: the refcount-zero path of every class that keeps the
	 * base's) is replaced by the same call with a count: a release on the scene lane (EngineReadWindow::sceneWork) or another thread of
	 * DCLF's executor (MarkThread) is counted with the object's class name (its RTTI) and reported (LANE ENGINE RELEASE). Counted only.
	 */
	class ReleaseGuard
	{
	public:
		/** @brief Render thread, at install: patches DeleteThis under the switch, once, its bytes checked first. */
		static void Install();
		/** @brief On each of DCLF's executor threads, at its start (SceneScheduler): a_kind 1 the scene lane, 2 the coordinator or a pool worker. */
		static void MarkThread(std::uint8_t a_kind) { dclfThread = a_kind; }
		/** @brief What the report reads since the last Take: the releases on the scene lane and on the other executor threads, the unnamed ones, the first and last class. */
		struct Counters
		{
			std::uint64_t lane = 0, pool = 0, unnamed = 0;
			const char* first = nullptr;
			const char* last = nullptr;
		};
		static bool Installed() { return installed; }
		/** @brief The counters since the last Take, reset (Reports::ReleaseGuardLine). */
		static Counters Take();

	private:
		static void DeleteThis(void* a_object);
		static void Note(void* a_object);

		static inline thread_local std::uint8_t dclfThread = 0;
		static inline bool installed = false;
		static inline std::atomic<std::uint64_t> laneReleases{ 0 }, poolReleases{ 0 }, unnamed{ 0 };
		static inline std::atomic<const char*> first{ nullptr }, last{ nullptr };
	};
}
