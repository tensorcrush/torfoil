#include "util/bytes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <random>
#endif

// statvfs existe sur Horizon comme sous Linux ; seule la compilation croisée
// sous Windows (vérifications rapides hors console) ne l'a pas.
#ifndef _WIN32
#include <sys/statvfs.h>
#endif

namespace util {

namespace {

const char kHexDigits[] = "0123456789abcdef";

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Formate avec la virgule décimale française.
std::string fmt_decimal(double value, const char* unit) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %s", value, unit);
    std::string s(buf);
    const size_t dot = s.find('.');
    if (dot != std::string::npos) s[dot] = ',';
    return s;
}

}  // namespace

std::string to_hex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHexDigits[data[i] >> 4]);
        out.push_back(kHexDigits[data[i] & 0x0f]);
    }
    return out;
}

bool from_hex(const std::string& hex, uint8_t* out, size_t out_len) {
    if (hex.size() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        const int hi = hex_val(hex[i * 2]);
        const int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

bool from_base32(const std::string& in, uint8_t* out, size_t out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t written = 0;

    for (const char raw : in) {
        if (raw == '=') break;  // padding, on s'arrête là
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        int v;
        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= '2' && c <= '7') {
            v = c - '2' + 26;
        } else {
            return false;
        }

        acc = acc << 5 | static_cast<uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (written >= out_len) return false;
            out[written++] = static_cast<uint8_t>(acc >> bits);
        }
    }
    return written == out_len;
}

std::string url_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHexDigits[c >> 4]);
            out.push_back(kHexDigits[c & 0x0f]);
        }
    }
    return out;
}

std::string url_encode(const std::string& s) {
    return url_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_val(s[i + 1]);
            const int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi << 4 | lo));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

void random_bytes(uint8_t* out, size_t len) {
#ifdef __SWITCH__
    randomGet(out, len);
#else
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(dist(gen));
#endif
}

uint64_t disk_free(const std::string& path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    struct statvfs vfs{};
    if (::statvfs(path.c_str(), &vfs) != 0) return 0;
    // Le « f_blocks > 0 » n'est pas décoratif : quand la console ne renseigne
    // pas ces champs, tout vaut zéro, et sans ce test on refuserait un torrent
    // en annonçant une carte pleine qui ne l'est pas. Mieux vaut ne pas savoir
    // que se tromper.
    if (vfs.f_frsize == 0 || vfs.f_blocks == 0) return 0;
    return static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
#endif
}

std::string human_size(uint64_t bytes) {
    static const char* units[] = {"o", "Ko", "Mo", "Go", "To"};
    if (bytes < 1024) return std::to_string(bytes) + " o";

    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 4) {
        v /= 1024.0;
        ++unit;
    }
    return fmt_decimal(v, units[unit]);
}

std::string human_rate(uint64_t bytes_per_sec) {
    if (bytes_per_sec == 0) return "—";
    return human_size(bytes_per_sec) + "/s";
}

std::string human_duration(uint64_t seconds) {
    if (seconds == 0) return "—";
    char buf[64];
    if (seconds < 60) {
        std::snprintf(buf, sizeof(buf), "%llu s", static_cast<unsigned long long>(seconds));
    } else if (seconds < 3600) {
        std::snprintf(buf, sizeof(buf), "%llu min %02llu s",
                      static_cast<unsigned long long>(seconds / 60),
                      static_cast<unsigned long long>(seconds % 60));
    } else if (seconds < 86400) {
        std::snprintf(buf, sizeof(buf), "%llu h %02llu min",
                      static_cast<unsigned long long>(seconds / 3600),
                      static_cast<unsigned long long>((seconds % 3600) / 60));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu j %02llu h",
                      static_cast<unsigned long long>(seconds / 86400),
                      static_cast<unsigned long long>((seconds % 86400) / 3600));
    }
    return buf;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string sanitize_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        // Interdits exFAT/FAT32 + caractères de contrôle.
        if (u < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    // Un nom se terminant par un point ou une espace est refusé par le FS.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = "_";
    // Marge pour le chemin complet : la Switch plafonne à 255 octets par composant.
    if (out.size() > 200) out.resize(200);
    return out;
}

}  // namespace util
