// REAPER calls the extension through C function pointers: an exception escaping into it terminates
// REAPER, losing the user's unsaved project. Every entry point runs its work through guarded(),
// which reports what escaped instead.
#pragma once

#include <exception>
#include <new>

// Shows "<extension>: <where> failed: <what>" in REAPER's console, once per distinct message
// (implemented in each extension's main.cpp; main thread only). Must not throw: guarded() is noexcept.
void ReportFailure(const char *where, const char *what) noexcept;

// Runs f() and returns its result; if it throws, reports the exception and returns fallback.
template <class R, class F> R guarded(const char *where, R fallback, F &&f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc &) {
        ReportFailure(where, "out of memory");
    } catch (const std::exception &e) {
        ReportFailure(where, e.what());
    } catch (...) {
        ReportFailure(where, "unknown error");
    }
    return fallback;
}

// guarded() for work with no result.
template <class F> void guarded(const char *where, F &&f) noexcept {
    guarded(where, 0, [&] {
        f();
        return 0;
    });
}
