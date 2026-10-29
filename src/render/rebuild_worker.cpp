/*
Slugged - GPU vector text for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "rebuild_worker.hpp"
#include "shape_cache.hpp"
#include "../text/font_manager.hpp"
#include "../util/log.hpp"

namespace slugged {

RebuildWorker &RebuildWorker::instance()
{
	static RebuildWorker worker;

	return worker;
}

RebuildWorker::~RebuildWorker()
{
	stop();
}

void RebuildWorker::submit(const std::shared_ptr<Job> &job, FrameInput input, uint64_t id)
{
	bool enqueue = false;

	{
		std::lock_guard<std::mutex> lock(job->mutex);

		job->pending = std::move(input);
		job->pendingId = id;

		if (!job->queued) {
			job->queued = true;
			enqueue = true;
		}
	}

	if (!enqueue)
		return;

	std::lock_guard<std::mutex> lock(_mutex);

	if (_stopping)
		return;

	if (!_thread.joinable())
		_thread = std::thread([this]() { run(); });

	_queue.push_back(job);
	_wake.notify_one();
}

void RebuildWorker::stop()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);

		_stopping = true;
		_queue.clear();
	}

	_wake.notify_all();

	if (_thread.joinable())
		_thread.join();
}

void RebuildWorker::run()
{
	for (;;) {
		std::shared_ptr<Job> job;

		{
			std::unique_lock<std::mutex> lock(_mutex);

			_wake.wait(lock, [this]() { return _stopping || !_queue.empty(); });

			if (_stopping)
				return;

			job = std::move(_queue.front());
			_queue.pop_front();
		}

		FrameInput input;
		uint64_t id = 0;

		{
			std::lock_guard<std::mutex> lock(job->mutex);

			job->queued = false;

			if (job->cancelled || !job->pending)
				continue;

			input = std::move(*job->pending);
			id = job->pendingId;
			job->pending.reset();
		}

		std::optional<FrameOutput> output;

		try {
			FontManager &fonts = FontManager::instance();
			auto guard = fonts.lock();

			// Between rebuilds nothing holds a font or shape pointer, so this
			// is the one safe moment to close what has gone idle.
			ShapeCache::instance().evict(fonts.collect());

			output = buildFrame(input, job->state);
		} catch (const std::exception &e) {
			slugged_log(LOG_ERROR, "rebuilding text failed: %s", e.what());
		}

		if (!output)
			continue;

		std::lock_guard<std::mutex> lock(job->mutex);

		if (!job->cancelled) {
			job->ready = std::move(output);
			job->readyId = id;
		}
	}
}

} // namespace slugged
