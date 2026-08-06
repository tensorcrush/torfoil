#include "install/keys.hpp"

#include <cstdio>
#include <cstring>

#include "util/bytes.hpp"

namespace install {

namespace {

const char* kCandidatePaths[] = {
    "sdmc:/switch/prod.keys",
    "sdmc:/switch/keys.dat",
    "sdmc:/prod.keys",
    "sdmc:/atmosphere/prod.keys",
    "sdmc:/switch/.overlays/prod.keys",
};

// Les clés indexées se terminent par « _XX » en hexadécimal.
bool suffix_index(const std::string& name, const std::string& prefix, int& index_out) {
    if (name.size() != prefix.size() + 2) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;

    uint8_t value = 0;
    if (!util::from_hex(name.substr(prefix.size()), &value, 1)) return false;
    index_out = value;
    return true;
}

}  // namespace

bool KeySet::parse_file(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    char line[512];
    while (std::fgets(line, sizeof(line), fp)) {
        std::string text(line);
        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;

        const std::string name = util::trim(text.substr(0, eq));
        const std::string value = util::trim(text.substr(eq + 1));
        if (name.empty() || value.empty()) continue;

        if (name == "header_key" && value.size() == 64) {
            has_header_key_ = util::from_hex(value, header_key_, sizeof(header_key_));
            continue;
        }

        int index = 0;
        if (value.size() == 32) {
            if (suffix_index(name, "key_area_key_application_", index) && index < kGenerations) {
                has_kaek_app_[index] = util::from_hex(value, kaek_app_[index], 16);
            } else if (suffix_index(name, "titlekek_", index) && index < kGenerations) {
                has_titlekek_[index] = util::from_hex(value, titlekek_[index], 16);
            }
        }
    }

    std::fclose(fp);
    return has_header_key_;
}

bool KeySet::load_from(const std::string& path, std::string* err) {
    if (!parse_file(path)) {
        if (err) *err = "fichier de clés illisible : " + path;
        return false;
    }
    source_path_ = path;
    loaded_ = true;
    return true;
}

bool KeySet::load(std::string* err) {
    for (const char* path : kCandidatePaths) {
        if (!parse_file(path)) continue;
        source_path_ = path;
        loaded_ = true;
        return true;
    }

    if (err) {
        *err =
            "prod.keys introuvable — le placer dans sdmc:/switch/prod.keys "
            "(extractible avec Lockpick_RCM)";
    }
    return false;
}

const uint8_t* KeySet::key_area_key_application(uint8_t generation) const {
    if (generation >= kGenerations || !has_kaek_app_[generation]) return nullptr;
    return kaek_app_[generation];
}

const uint8_t* KeySet::titlekek(uint8_t generation) const {
    if (generation >= kGenerations || !has_titlekek_[generation]) return nullptr;
    return titlekek_[generation];
}

}  // namespace install
