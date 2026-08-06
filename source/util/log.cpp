#include "util/log.hpp"

#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "util/clock.hpp"

namespace util {

namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;
uint64_t g_start_ms = 0;

// Au-delà, on repart de zéro : un journal qui grossit sans fin finirait par
// remplir la carte d'un utilisateur qui ne le regarde jamais.
constexpr long kMaxSize = 512 * 1024;

}  // namespace

void log_open(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;

    // Rotation simple : le journal précédent est conservé sous .1, ce qui permet
    // de garder la trace d'un plantage suivi d'un redémarrage.
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && st.st_size > kMaxSize) {
        const std::string previous = path + ".1";
        std::remove(previous.c_str());
        std::rename(path.c_str(), previous.c_str());
    }

    g_file = std::fopen(path.c_str(), "ab");
    g_start_ms = now_ms();
    if (!g_file) return;

    std::fprintf(g_file, "\n===== Torfoil démarre =====\n");
    std::fflush(g_file);
}

void log_close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::fprintf(g_file, "----- arrêt propre -----\n");
    std::fclose(g_file);
    g_file = nullptr;
}

void log_line(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    const uint64_t elapsed = now_ms() - g_start_ms;
    std::fprintf(g_file, "[%6llu ms] %s\n", static_cast<unsigned long long>(elapsed),
                 message.c_str());
    // Vidage immédiat : un journal perdu au moment du plantage ne sert à rien,
    // et c'est précisément là qu'on en a besoin.
    std::fflush(g_file);
}

void log_fmt(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_line(buffer);
}

}  // namespace util
