#include "auspex/smoke.hpp"

#include <cstdlib>
#include <mutex>

namespace auspex {
namespace {

std::mutex& ledger_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::string>& ledger() {
    static std::vector<std::string> entries;
    return entries;
}

}  // namespace

bool smoke_mode() {
    // Read ONCE. A test that could turn the guards off halfway through by
    // unsetting a variable would be a test that could spawn something halfway
    // through, and the point of this is that it cannot.
    static const bool on = std::getenv("AUSPEX_SMOKE") != nullptr;
    return on;
}

bool smoke_refuse(const std::string& what) {
    if (!smoke_mode()) return false;
    {
        std::lock_guard lock(ledger_mutex());
        // Locked because the things being refused are exactly the things that run
        // on worker threads -- a crew runner, a measurement, a text-to-speech
        // call. Recording a refusal must not be the unsafe part of a safety net.
        ledger().push_back(what);
    }
    return true;
}

std::vector<std::string> smoke_refusals() {
    std::lock_guard lock(ledger_mutex());
    return ledger();
}

void smoke_reset() {
    std::lock_guard lock(ledger_mutex());
    ledger().clear();
}

}  // namespace auspex
