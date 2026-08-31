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

#include "host_tokens.hpp"

#include <cstdio>
#include <mutex>
#include <string>

#include <obs.h>
#include <util/platform.h>

#ifdef SLUGGED_HAVE_FRONTEND
#include <obs-frontend-api.h>
#endif

namespace slugged {
namespace host {

namespace {

// How often the sampled values are refreshed. Fast enough that a bitrate or
// dropped-frame counter on screen looks live, slow enough that the locks and
// the CPU query behind it cost nothing measurable.
constexpr float kSampleInterval = 0.25f;

std::string formatDuration(uint64_t seconds)
{
	const unsigned h = unsigned(seconds / 3600);
	const unsigned m = unsigned((seconds / 60) % 60);
	const unsigned s = unsigned(seconds % 60);

	char buf[32];

	if (h > 0)
		std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
	else
		std::snprintf(buf, sizeof(buf), "%u:%02u", m, s);

	return buf;
}

std::string formatNumber(const char *format, double value)
{
	char buf[64];

	std::snprintf(buf, sizeof(buf), format, value);

	return buf;
}

// Everything the frontend event callback writes and the sampling tick reads.
// The callback runs on the UI thread and the tick on the graphics thread, so
// the two are separated by `mutex`.
struct State {
	std::mutex mutex;

	VariableMap values;
	uint64_t generation = 0;

	float accumulator = kSampleInterval;

	os_cpu_usage_info_t *cpu = nullptr;

	// Wall-clock nanoseconds at which the stream and the recording started.
	// Zero when that output is not running.
	uint64_t streamStarted = 0;
	uint64_t recordStarted = 0;

	// Nanoseconds the recording spent paused, so {record_time} matches the time
	// actually written to the file.
	uint64_t recordPausedTotal = 0;
	uint64_t recordPausedAt = 0;

	std::string sceneName;
	std::string previewSceneName;

	// Held for the life of the output so the sampling tick never has to reach
	// into the frontend from the graphics thread.
	obs_output_t *streamOutput = nullptr;

	// Previous byte count and the time it was taken at, for the bitrate delta.
	uint64_t lastBytes = 0;
	uint64_t lastBytesAt = 0;
	double bitrateKbps = 0.0;
};

State &state()
{
	static State s;

	return s;
}

// Replaces the published table, bumping the generation only when it really
// changed. A stream timer changes once a second, not four times.
void publish(State &s, VariableMap next)
{
	if (s.values == next)
		return;

	s.values = std::move(next);
	s.generation++;
}

#ifdef SLUGGED_HAVE_FRONTEND

std::string currentSceneName(bool preview)
{
	obs_source_t *scene = preview ? obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene();

	if (!scene)
		return {};

	const char *name = obs_source_get_name(scene);
	std::string out = name ? name : "";

	obs_source_release(scene);

	return out;
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	State &s = state();

	std::lock_guard<std::mutex> lock(s.mutex);

	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		s.streamStarted = os_gettime_ns();
		s.lastBytes = 0;
		s.lastBytesAt = 0;
		s.bitrateKbps = 0.0;

		// Taken here, on the UI thread, and held until the stream stops, so
		// the sampling tick only ever touches core obs_output_* calls.
		s.streamOutput = obs_frontend_get_streaming_output();
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		s.streamStarted = 0;
		s.bitrateKbps = 0.0;

		if (s.streamOutput) {
			obs_output_release(s.streamOutput);
			s.streamOutput = nullptr;
		}
		break;

	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		s.recordStarted = os_gettime_ns();
		s.recordPausedTotal = 0;
		s.recordPausedAt = 0;
		break;

	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		s.recordStarted = 0;
		s.recordPausedAt = 0;
		break;

	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		if (!s.recordPausedAt)
			s.recordPausedAt = os_gettime_ns();
		break;

	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		if (s.recordPausedAt) {
			s.recordPausedTotal += os_gettime_ns() - s.recordPausedAt;
			s.recordPausedAt = 0;
		}
		break;

	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		s.sceneName = currentSceneName(false);
		s.previewSceneName = currentSceneName(true);
		break;

	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		s.previewSceneName = currentSceneName(true);
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		if (s.streamOutput) {
			obs_output_release(s.streamOutput);
			s.streamOutput = nullptr;
		}
		break;

	default:
		break;
	}
}

#endif // SLUGGED_HAVE_FRONTEND

} // namespace

void startup()
{
	State &s = state();

	{
		std::lock_guard<std::mutex> lock(s.mutex);

		if (!s.cpu)
			s.cpu = os_cpu_usage_info_start();
	}

#ifdef SLUGGED_HAVE_FRONTEND
	obs_frontend_add_event_callback(frontendEvent, nullptr);
#endif
}

void shutdown()
{
#ifdef SLUGGED_HAVE_FRONTEND
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
#endif

	State &s = state();

	std::lock_guard<std::mutex> lock(s.mutex);

	if (s.cpu) {
		os_cpu_usage_info_destroy(s.cpu);
		s.cpu = nullptr;
	}

	// Null in a build without the frontend API, which is what took the
	// reference in the first place.
	if (s.streamOutput) {
		obs_output_release(s.streamOutput);
		s.streamOutput = nullptr;
	}

	s.values.clear();
	s.generation++;
}

void tick(float seconds)
{
	State &s = state();

	std::lock_guard<std::mutex> lock(s.mutex);

	s.accumulator += seconds;

	if (s.accumulator < kSampleInterval)
		return;

	s.accumulator = 0.0f;

	const uint64_t now = os_gettime_ns();

	VariableMap next;

	next["fps"] = formatNumber("%.0f", obs_get_active_fps());

	if (s.cpu)
		next["cpu"] = formatNumber("%.1f%%", os_cpu_usage_info_query(s.cpu));

	if (s.streamStarted)
		next["stream_time"] = formatDuration((now - s.streamStarted) / 1000000000ULL);
	else
		next["stream_time"] = "0:00";

	if (s.recordStarted) {
		// A recording paused right now is still accruing paused time, so the
		// in-progress pause is subtracted alongside the finished ones.
		const uint64_t pausedNow = s.recordPausedAt ? now - s.recordPausedAt : 0;
		const uint64_t paused = s.recordPausedTotal + pausedNow;
		const uint64_t elapsed = now - s.recordStarted;

		next["record_time"] = formatDuration((elapsed > paused ? elapsed - paused : 0) / 1000000000ULL);
	} else {
		next["record_time"] = "0:00";
	}

	next["scene"] = s.sceneName;
	next["preview_scene"] = s.previewSceneName;

	// The output reference is only ever taken by the frontend hooks, so in a
	// build without them this is the "not streaming" branch every time.
	if (s.streamOutput) {
		const int dropped = obs_output_get_frames_dropped(s.streamOutput);
		const int total = obs_output_get_total_frames(s.streamOutput);

		next["dropped_frames"] = std::to_string(dropped);
		next["dropped_percent"] =
			formatNumber("%.1f%%", total > 0 ? 100.0 * double(dropped) / double(total) : 0.0);

		const uint64_t bytes = obs_output_get_total_bytes(s.streamOutput);

		if (s.lastBytesAt && now > s.lastBytesAt && bytes >= s.lastBytes) {
			const double elapsed = double(now - s.lastBytesAt) / 1000000000.0;

			s.bitrateKbps = (double(bytes - s.lastBytes) * 8.0 / 1000.0) / elapsed;
		}

		s.lastBytes = bytes;
		s.lastBytesAt = now;

		next["bitrate"] = formatNumber("%.0f", s.bitrateKbps);
	} else {
		next["dropped_frames"] = "0";
		next["dropped_percent"] = "0.0%";
		next["bitrate"] = "0";
	}

	publish(s, std::move(next));
}

VariableMap values()
{
	State &s = state();

	std::lock_guard<std::mutex> lock(s.mutex);

	return s.values;
}

uint64_t generation()
{
	State &s = state();

	std::lock_guard<std::mutex> lock(s.mutex);

	return s.generation;
}

} // namespace host
} // namespace slugged
