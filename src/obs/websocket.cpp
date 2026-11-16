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

#include "websocket.hpp"
#include "slugged_source.hpp"
#include "../core/tokens.hpp"
#include "../core/variables.hpp"
#include "../util/log.hpp"

#include <obs-module.h>

#include <string>

namespace slugged {
namespace websocket {

namespace {

// obs-websocket's plugin API is a set of procs on a proc handler it hands out
// through OBS's own. These are the same calls obs-websocket-api.h makes; the
// header itself is not part of the OBS development packages on every
// platform, so the few calls needed are made here directly.

using RequestCallback = void (*)(obs_data_t *request, obs_data_t *response, void *data);

// Laid out exactly as obs-websocket's obs_websocket_request_callback.
struct VendorRequestCallback {
	RequestCallback callback;
	void *data;
};

proc_handler_t *websocketProcs()
{
	calldata_t call = {};
	proc_handler_t *procs = nullptr;

	if (proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &call))
		procs = static_cast<proc_handler_t *>(calldata_ptr(&call, "ph"));

	calldata_free(&call);

	return procs;
}

// ---- requests ----------------------------------------------------------------

void fail(obs_data_t *response, const char *message)
{
	obs_data_set_bool(response, "ok", false);
	obs_data_set_string(response, "error", message);
}

void succeed(obs_data_t *response)
{
	obs_data_set_bool(response, "ok", true);
}

// The Slugged source or overlay filter a request names. Null when the request
// names none; `named` says whether it tried.
obs_source_t *targetOf(obs_data_t *request, obs_data_t *response, bool &named)
{
	const char *sourceName = obs_data_get_string(request, "sourceName");
	const char *filterName = obs_data_get_string(request, "filterName");

	named = sourceName && *sourceName;

	if (!named)
		return nullptr;

	obs_source_t *source = obs_get_source_by_name(sourceName);

	if (!source) {
		fail(response, "No source has that name.");
		return nullptr;
	}

	if (filterName && *filterName) {
		obs_source_t *filter = obs_source_get_filter_by_name(source, filterName);

		obs_source_release(source);
		source = filter;

		if (!source) {
			fail(response, "That source has no filter with that name.");
			return nullptr;
		}
	}

	if (!isSluggedSource(source)) {
		obs_source_release(source);
		fail(response, "That is not a Slugged source or filter.");
		return nullptr;
	}

	return source;
}

// Calls a proc on the named source, passing `name`, `value` and `amount` from
// the request through when present, and reports the outcome.
bool callOnSource(obs_data_t *request, obs_data_t *response, const char *proc, calldata_t *call)
{
	bool named = false;
	obs_source_t *source = targetOf(request, response, named);

	if (!source) {
		if (!named)
			fail(response, "sourceName is required.");

		return false;
	}

	proc_handler_call(obs_source_get_proc_handler(source), proc, call);
	obs_source_release(source);

	succeed(response);

	return true;
}

void simpleProc(obs_data_t *request, obs_data_t *response, const char *proc)
{
	calldata_t call = {};

	callOnSource(request, response, proc, &call);
	calldata_free(&call);
}

std::string stringOf(obs_data_t *request, const char *key)
{
	// Numbers are welcome where a string is expected: a score sent as 3.
	obs_data_item_t *item = obs_data_item_byname(request, key);

	if (!item)
		return {};

	std::string out;

	switch (obs_data_item_gettype(item)) {
	case OBS_DATA_STRING:
		out = obs_data_item_get_string(item);
		break;
	case OBS_DATA_NUMBER:
		out = obs_data_item_numtype(item) == OBS_DATA_NUM_INT ? std::to_string(obs_data_item_get_int(item))
								     : formatPlainNumber(obs_data_item_get_double(item));
		break;
	case OBS_DATA_BOOLEAN:
		out = obs_data_item_get_bool(item) ? "true" : "false";
		break;
	default:
		break;
	}

	obs_data_item_release(&item);

	return out;
}

void setVariable(obs_data_t *request, obs_data_t *response, void *)
{
	const std::string name = stringOf(request, "name");
	const std::string value = stringOf(request, "value");

	if (name.empty())
		return fail(response, "name is required.");

	if (!*obs_data_get_string(request, "sourceName")) {
		GlobalVariables::set(name, value);
		return succeed(response);
	}

	calldata_t call = {};
	calldata_set_string(&call, "name", name.c_str());
	calldata_set_string(&call, "value", value.c_str());
	callOnSource(request, response, "set_variable", &call);
	calldata_free(&call);
}

void getVariable(obs_data_t *request, obs_data_t *response, void *)
{
	const std::string name = stringOf(request, "name");

	if (name.empty())
		return fail(response, "name is required.");

	if (!*obs_data_get_string(request, "sourceName")) {
		std::string value;
		const bool found = GlobalVariables::lookup(name, value);

		obs_data_set_string(response, "value", value.c_str());
		obs_data_set_bool(response, "found", found);
		return succeed(response);
	}

	calldata_t call = {};
	calldata_set_string(&call, "name", name.c_str());

	if (callOnSource(request, response, "get_variable", &call)) {
		const char *value = nullptr;

		calldata_get_string(&call, "value", &value);
		obs_data_set_string(response, "value", value ? value : "");
	}

	calldata_free(&call);
}

// Adds `sign * amount` to a variable, or resets it to zero.
void stepVariable(obs_data_t *request, obs_data_t *response, double sign, bool reset)
{
	const std::string name = stringOf(request, "name");
	double amount = obs_data_has_user_value(request, "amount") ? obs_data_get_double(request, "amount") : 0.0;

	if (!*obs_data_get_string(request, "sourceName")) {
		// The shared table has no counter settings to fall back on.
		if (name.empty())
			return fail(response, "name is required without sourceName.");

		std::string current;
		double value = 0.0;

		GlobalVariables::lookup(name, current);
		parseNumber(current, value);

		const std::string next = formatPlainNumber(reset ? 0.0 : value + sign * (amount != 0.0 ? amount : 1.0));

		GlobalVariables::set(name, next);
		obs_data_set_string(response, "value", next.c_str());
		return succeed(response);
	}

	calldata_t call = {};
	calldata_set_string(&call, "name", name.c_str());
	calldata_set_float(&call, "amount", amount);
	callOnSource(request, response, reset ? "reset_variable" : (sign > 0.0 ? "increment_variable" : "decrement_variable"),
		     &call);
	calldata_free(&call);
}

void incrementVariable(obs_data_t *request, obs_data_t *response, void *)
{
	stepVariable(request, response, 1.0, false);
}

void decrementVariable(obs_data_t *request, obs_data_t *response, void *)
{
	stepVariable(request, response, -1.0, false);
}

void resetVariable(obs_data_t *request, obs_data_t *response, void *)
{
	stepVariable(request, response, 1.0, true);
}

void clearVariables(obs_data_t *request, obs_data_t *response, void *)
{
	if (!*obs_data_get_string(request, "sourceName")) {
		GlobalVariables::clear();
		return succeed(response);
	}

	simpleProc(request, response, "clear_variables");
}

#define SLUGGED_SIMPLE_REQUEST(function, proc)                                     \
	void function(obs_data_t *request, obs_data_t *response, void *)          \
	{                                                                          \
		simpleProc(request, response, proc);                               \
	}

SLUGGED_SIMPLE_REQUEST(playMotion, "replay_motion")
SLUGGED_SIMPLE_REQUEST(nextMessage, "next_message")
SLUGGED_SIMPLE_REQUEST(timerStart, "timer_start")
SLUGGED_SIMPLE_REQUEST(timerPause, "timer_pause")
SLUGGED_SIMPLE_REQUEST(timerToggle, "timer_toggle")
SLUGGED_SIMPLE_REQUEST(timerReset, "timer_reset")

#undef SLUGGED_SIMPLE_REQUEST

} // namespace

void registerVendor()
{
	proc_handler_t *procs = websocketProcs();

	if (!procs)
		return;

	calldata_t call = {};
	calldata_set_string(&call, "name", "slugged");
	proc_handler_call(procs, "vendor_register", &call);

	void *vendor = calldata_ptr(&call, "vendor");
	calldata_free(&call);

	if (!vendor) {
		slugged_log(LOG_WARNING, "obs-websocket did not register the Slugged vendor");
		return;
	}

	const std::pair<const char *, RequestCallback> requests[] = {
		{"SetVariable", setVariable},
		{"GetVariable", getVariable},
		{"IncrementVariable", incrementVariable},
		{"DecrementVariable", decrementVariable},
		{"ResetVariable", resetVariable},
		{"ClearVariables", clearVariables},
		{"PlayMotion", playMotion},
		{"NextMessage", nextMessage},
		{"TimerStart", timerStart},
		{"TimerPause", timerPause},
		{"TimerToggle", timerToggle},
		{"TimerReset", timerReset},
	};

	for (const auto &[type, callback] : requests) {
		// obs-websocket copies the callback struct, so it may live here.
		VendorRequestCallback cb = {callback, nullptr};

		calldata_t request = {};
		calldata_set_string(&request, "type", type);
		calldata_set_ptr(&request, "callback", &cb);
		calldata_set_ptr(&request, "vendor", vendor);

		proc_handler_call(procs, "vendor_request_register", &request);

		if (!calldata_bool(&request, "success"))
			slugged_log(LOG_WARNING, "obs-websocket refused the Slugged request %s", type);

		calldata_free(&request);
	}

	slugged_log(LOG_INFO, "Slugged requests registered with obs-websocket");
}

} // namespace websocket
} // namespace slugged
