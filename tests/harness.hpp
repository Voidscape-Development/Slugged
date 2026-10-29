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

// A deliberately tiny test harness: the tests only need registration, a few
// assertions and a non-zero exit code on failure, which is not worth a
// dependency.

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace harness {

struct Test {
	const char *name;
	std::function<void()> fn;
};

inline std::vector<Test> &registry()
{
	static std::vector<Test> tests;
	return tests;
}

inline int &failures()
{
	static int count = 0;
	return count;
}

struct Register {
	Register(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

template<typename A, typename B>
void checkEqual(const A &a, const B &b, const char *ea, const char *eb, const char *file, int line)
{
	if (a == b)
		return;

	std::ostringstream os;
	os << a << " != " << b;

	std::fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: %s\n", file, line, ea, eb, os.str().c_str());
	failures()++;
}

} // namespace harness

#define SLUGGED_CAT2(a, b) a##b
#define SLUGGED_CAT(a, b) SLUGGED_CAT2(a, b)

#define TEST(name)                                                                           \
	static void name();                                                                  \
	static harness::Register SLUGGED_CAT(register_, name)(#name, name);                  \
	static void name()

#define CHECK(expr)                                                                          \
	do {                                                                                 \
		if (!(expr)) {                                                               \
			std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
			harness::failures()++;                                               \
		}                                                                            \
	} while (0)

#define CHECK_EQ(a, b) harness::checkEqual((a), (b), #a, #b, __FILE__, __LINE__)
