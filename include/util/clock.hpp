#pragma once

#include <chrono>
#include <cstdint>

namespace util {

// Horloge monotone en millisecondes. Jamais l'heure murale : la Switch peut la
// voir bouger (RTC, synchro réseau) et tous nos délais en dépendraient.
inline uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace util
