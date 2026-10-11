// abi_common.cpp — the single implementation of the error channel, string
// marshalling and logging shared by all six adapter translation units.

#include "abi_common.h"
#include "../dx12/dx12_device.h"

namespace mcdr::abi {

namespace {

// One error slot per thread. The async render thread, the descriptor workers
// and an FFM upcall arriving from a native thread each report independently,
// so a naive file-scope std::string would let one thread's failure overwrite
// another's before Java ever reads it.
thread_local std::string gLastError;

}  // namespace

const std::string& lastErrorText() { return gLastError; }

void setLastError(const char* what, const std::string& message) {
    std::string& slot = gLastError;
    slot.clear();
    if (what && *what) {
        slot.append(what);
        slot.append(": ");
    }
    slot.append(message);
}

void setLastErrorf(const char* what, const char* fmt, ...) {
    char scratch[1024];
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(scratch, sizeof(scratch), fmt, args);
    va_end(args);
    if (written < 0) {
        scratch[0] = '\0';
    }
    setLastError(what, std::string(scratch));
}

int32_t failWith(const char* what, const std::string& err) {
    failVoid(what, err);
    return 0;
}

void failVoid(const char* what, const std::string& err) {
    setLastError(what, err);
    // Same stderr diagnostics the JNI adapters emitted, so an existing log
    // capture still shows which entry point failed and why.
    std::fprintf(stderr, "[mcdr] %s: %s\n", what ? what : "?", err.c_str());
    std::fflush(stderr);
}

int32_t writeString(const std::string& s, char* buf, int32_t cap) {
    const int32_t required = static_cast<int32_t>(s.size()) + 1;  // includes NUL
    if (buf && cap > 0) {
        const size_t room = static_cast<size_t>(cap - 1);
        const size_t n = s.size() < room ? s.size() : room;
        if (n > 0) {
            std::memcpy(buf, s.data(), n);
        }
        buf[n] = '\0';
    }
    return required;
}

}  // namespace mcdr::abi
