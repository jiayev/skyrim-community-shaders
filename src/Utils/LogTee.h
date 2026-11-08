#pragma once

// The plugin log's fan-out sink. Install puts the default logger's sinks behind it, so other sinks can be added for a while and
// removed again (the DCLF diagnostic capture copies every line it logs into its bundle this way). Adding and removing are thread-safe.

#include <memory>

namespace spdlog::sinks
{
	class sink;
}

namespace Util::LogTee
{
	/**
	 * @brief SKSEPlugin_Load, after SKSE::Init (which replaces the default logger with CommonLib's): the default logger again, with the
	 * same sinks, level and flush level, writing through the fan-out sink.
	 */
	void Install();

	/** @brief Any thread: a_sink receives every line logged from now on, until RemoveSink. Does nothing before Install. */
	void AddSink(const std::shared_ptr<spdlog::sinks::sink>& a_sink);

	/** @brief Any thread: a_sink receives no more lines. */
	void RemoveSink(const std::shared_ptr<spdlog::sinks::sink>& a_sink);

	/** @brief The log's line pattern, for a sink added here to match the log file's. */
	inline constexpr const char* kPattern = "[%T.%e] [%=5t] [%L] %v";
}
