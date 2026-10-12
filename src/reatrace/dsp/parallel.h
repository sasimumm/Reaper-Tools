// Worker threads that can't take REAPER down: an exception escaping a std::thread terminates the
// process, so parallel() catches it in the worker and rethrows it in the caller once every thread
// has been joined.
#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Runs body(i) for i in [0, n) on n threads, and whileRunning() (if given; e.g. reporting
// progress, it should return once `stop` is set) on the calling thread meanwhile. A failure in any
// of them, or in starting a thread, sets `stop` so the others can give up early; the first one is
// rethrown after all threads have finished.
template <class Body>
void parallel(int n, std::atomic<bool> &stop, Body body,
              const std::function<void()> &whileRunning = nullptr) {
    std::mutex m;
    std::exception_ptr error;
    auto fail = [&] {
        std::lock_guard<std::mutex> lock(m);
        if (!error) {
            error = std::current_exception();
        }
        stop = true;
    };
    std::vector<std::thread> threads;
    try {
        threads.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            threads.emplace_back([&, i] {
                try {
                    body(i);
                } catch (...) {
                    fail();
                }
            });
        }
        if (whileRunning) {
            whileRunning();
        }
    } catch (...) {
        fail();
    }
    for (std::thread &t : threads) {
        t.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
}
