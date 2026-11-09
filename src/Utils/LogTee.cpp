#include "LogTee.h"

#include <spdlog/sinks/dist_sink.h>

namespace Util::LogTee
{
	namespace
	{
		std::shared_ptr<spdlog::sinks::dist_sink_mt> tee;
	}

	void Install()
	{
		const auto current = spdlog::default_logger();
		if (!current || tee)
			return;
		auto fanOut = std::make_shared<spdlog::sinks::dist_sink_mt>();
		// The sinks keep their own formatters (the pattern set on the logger was applied to each), so the lines read as before.
		for (const auto& sink : current->sinks())
			fanOut->add_sink(sink);
		auto log = std::make_shared<spdlog::logger>(current->name(), fanOut);
		log->set_level(current->level());
		log->flush_on(current->flush_level());
		tee = std::move(fanOut);
		spdlog::set_default_logger(std::move(log));
	}

	void AddSink(const std::shared_ptr<spdlog::sinks::sink>& a_sink)
	{
		if (tee && a_sink)
			tee->add_sink(a_sink);
	}

	void RemoveSink(const std::shared_ptr<spdlog::sinks::sink>& a_sink)
	{
		if (tee && a_sink)
			tee->remove_sink(a_sink);
	}
}
