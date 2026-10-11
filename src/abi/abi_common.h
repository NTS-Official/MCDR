// abi_common.h — shared helpers for the MCDR C-ABI adapter layer.
//
// Internal to the DLL: never installed, never included by the Java side.
//
// Its whole job is to make the exported wrappers uniformly thin. Under JNI the
// six jni_bridge_*.cpp files each carried their own toPtr<>()/logFail()/
// throwJava() trio; here there is exactly one implementation. The only real
// behavioural change is the error channel: JNI could raise a Java exception,
// while a C ABI cannot, so every failure lands in a thread-local last-error
// string that the Java side polls with mcdrLastError().
#pragma once

#ifndef MCDR_EXPORTS
#define MCDR_EXPORTS 1
#endif

#include "mcdr_abi.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace mcdr::abi {

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

// Records "<what>: <message>" as the calling thread's last error. Thread-local
// on purpose: the async render thread and the FFM upcall thread both report
// failures, and neither may clobber the main thread's message.
void setLastError(const char* what, const std::string& message);
void setLastErrorf(const char* what, const char* fmt, ...);

// The calling thread's pending message, for mcdrLastError(). Never cleared on
// read: the Java side only asks after a wrapper has already reported failure.
const std::string& lastErrorText();

// The two shapes every wrapper needs: record + log + return a failure value.
// Logging goes to stderr exactly like the old logFail(), so existing
// diagnostics keep working unchanged.
int32_t failWith(const char* what, const std::string& err);
void failVoid(const char* what, const std::string& err);

template <typename T>
inline T* failWithPtr(const char* what, const std::string& err) {
    failVoid(what, err);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Handle <-> pointer
//
// The only reinterpret_casts in the adapter layer. `mcdr_object*` and friends
// are typedefs of the real classes, so typed parameters need no cast at all;
// these exist for the integer-valued handles (HWND, recorded bundle list, raw
// GPU descriptor handles, user callback handles).
// ---------------------------------------------------------------------------

template <typename T>
inline T* fromHandle(uint64_t h) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(h));
}

template <typename T>
inline uint64_t toHandle(const T* p) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
}

// ---------------------------------------------------------------------------
// String marshalling
// ---------------------------------------------------------------------------

// Copies `s` into `buf` as UTF-8 with a NUL terminator. Always returns the
// required size including the terminator, even when the buffer is too small, so
// a caller can probe with (nullptr, 0) and then size exactly. Returns 0 only
// when there is genuinely nothing to write.
int32_t writeString(const std::string& s, char* buf, int32_t cap);

}  // namespace mcdr::abi
