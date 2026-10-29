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

#include "harness.hpp"

#include <cstring>

int main(int argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : nullptr;

	int run = 0;

	for (const harness::Test &test : harness::registry()) {
		if (filter && !std::strstr(test.name, filter))
			continue;

		const int before = harness::failures();

		test.fn();
		run++;

		std::printf("%s %s\n", harness::failures() == before ? "ok  " : "FAIL", test.name);
	}

	std::printf("\n%d tests, %d failed checks\n", run, harness::failures());

	return harness::failures() == 0 ? 0 : 1;
}
