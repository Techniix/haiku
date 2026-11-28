/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27, Steam groundwork): C++ as games use it, against the
 * Debian root's libstdc++ (GCC 14) and libgcc_s: exceptions unwinding
 * through frames (libgcc_s's unwinder and the .eh_frame_hdr lookup through
 * dl_iterate_phdr), std::thread with a mutex and a condition variable
 * (futexes), iostreams, std::string and containers, a static initialiser.
 * Prints one line per check, then "ok".
 */
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int sFailures = 0;

static void
check(const char* name, bool pass, const std::string& detail)
{
	if (pass)
		std::cout << name << " ok\n";
	else {
		std::cout << name << " FAIL: " << detail << "\n";
		sFailures++;
	}
}

struct Registry {
	std::map<std::string, int> entries;
	Registry() { entries["static"] = 42; }
};
static Registry sRegistry;

[[gnu::noinline]] static int
deep(int level)
{
	if (level == 0)
		throw std::runtime_error("from deep");
	std::string keep = "frame " + std::to_string(level);
	return deep(level - 1) + (int)keep.size();
}

int
main()
{
	check("static_init", sRegistry.entries["static"] == 42, "registry empty");

	std::string caught;
	try {
		deep(20);
	} catch (const std::exception& exception) {
		caught = exception.what();
	}
	check("exceptions", caught == "from deep", "caught [" + caught + "]");

	std::mutex lock;
	std::condition_variable changed;
	int ready = 0;
	long total = 0;
	std::vector<std::thread> threads;
	for (int i = 0; i < 4; i++) {
		threads.emplace_back([&, i] {
			long sum = 0;
			for (int j = 0; j <= 1000; j++)
				sum += j * (i + 1);
			std::unique_lock<std::mutex> guard(lock);
			total += sum;
			ready++;
			changed.notify_all();
		});
	}
	{
		std::unique_lock<std::mutex> guard(lock);
		changed.wait(guard, [&] { return ready == 4; });
	}
	for (std::thread& thread : threads)
		thread.join();
	check("threads", total == 500500L * 10, "total " + std::to_string(total));

	std::ostringstream stream;
	std::vector<int> values = { 5, 3, 9, 1 };
	std::sort(values.begin(), values.end());
	for (int value : values)
		stream << value << ',';
	check("iostream", stream.str() == "1,3,5,9,", "got " + stream.str());

	if (sFailures == 0)
		std::cout << "ok\n";
	return sFailures == 0 ? 0 : 1;
}
