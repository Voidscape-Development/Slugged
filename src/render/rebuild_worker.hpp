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

#pragma once

#include "frame.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace slugged {

// Builds frames off the graphics thread.
//
// Shaping a long chat log, decomposing new glyphs and stroking outlines can take
// milliseconds; on the graphics thread that is a dropped frame for the whole of
// OBS. One worker thread does it for every source instead. A single thread is
// deliberate: FreeType faces are not thread-safe, and with one consumer the font
// lock is never contended by another rebuild.
class RebuildWorker {
public:
	// One source's mailbox. Submitting replaces any input still waiting, so a
	// source that changes faster than it can be rebuilt only ever builds its
	// newest text.
	struct Job {
		std::mutex mutex;

		std::optional<FrameInput> pending;
		uint64_t pendingId = 0;

		std::optional<FrameOutput> ready;
		uint64_t readyId = 0;

		bool queued = false;
		bool cancelled = false;

		// Only touched by the worker thread.
		FrameState state;
	};

	static RebuildWorker &instance();

	void submit(const std::shared_ptr<Job> &job, FrameInput input, uint64_t id);

	// Stops the thread. Only at module unload.
	void stop();

	~RebuildWorker();

private:
	void run();

	std::mutex _mutex;
	std::condition_variable _wake;
	std::deque<std::shared_ptr<Job>> _queue;
	std::thread _thread;
	bool _stopping = false;
};

} // namespace slugged
