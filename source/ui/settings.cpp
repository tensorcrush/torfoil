#include "ui/settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ui {

namespace {

bool parse_bool(const std::string& value) {
    return value == "1" || value == "oui" || value == "true";
}

}  // namespace

const std::vector<Toggle>& toggles() {
    // L'ordre compte : du plus protecteur au plus anecdotique. Les deux premiers
    // sont ceux qui servent vraiment dans un pays qui surveille sa ligne.
    static const std::vector<Toggle> list = {
        {Str::TglVpn, Str::TglVpnEffect, &Settings::require_vpn, false},
        {Str::TglHttps, Str::TglHttpsEffect, &Settings::https_trackers_only, false},
        {Str::TglNoDht, Str::TglNoDhtEffect, &Settings::enable_dht, true},
        {Str::TglNoPex, Str::TglNoPexEffect, &Settings::enable_pex, true},
        {Str::TglNoUpload, Str::TglNoUploadEffect, &Settings::no_upload, false},
        {Str::TglRemote, Str::TglRemoteEffect, &Settings::remote_enabled, false},
    };
    return list;
}

Lang Settings::effective_language() const {
    Lang chosen = Lang::En;
    if (language != "auto" && lang_from_code(language, chosen)) return chosen;
    return console_language();
}

bool Settings::load(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    char line[256];
    while (std::fgets(line, sizeof(line), fp)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();

        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = text.substr(0, eq);
        const std::string raw = text.substr(eq + 1);
        const bool value = parse_bool(raw);

        if (key == "require_vpn") require_vpn = value;
        else if (key == "https_trackers_only") https_trackers_only = value;
        else if (key == "enable_dht") enable_dht = value;
        else if (key == "enable_pex") enable_pex = value;
        else if (key == "no_upload") no_upload = value;
        else if (key == "remote_enabled") remote_enabled = value;
        else if (key == "max_active") {
            const int parsed = std::atoi(raw.c_str());
            max_active = (parsed < 0 || parsed > 8) ? 2 : parsed;
        }
        else if (key == "language") {
            // Un code inconnu — faute de frappe, ou fichier venu d'une version
            // qui parlait une langue de plus — retombe sur « auto » plutôt que
            // de figer l'interface dans un texte vide.
            Lang parsed = Lang::En;
            language = (raw == "auto" || lang_from_code(raw, parsed)) ? raw : "auto";
        }
    }
    std::fclose(fp);
    return true;
}

bool Settings::save(const std::string& path) const {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    std::fprintf(fp, "%s\n", tr(Str::SettingsFileHead));
    std::fprintf(fp, "# language: auto, de, en, es, fr, ja, ru, zh\n");
    std::fprintf(fp, "language=%s\n", language.c_str());
    std::fprintf(fp, "require_vpn=%d\n", require_vpn ? 1 : 0);
    std::fprintf(fp, "https_trackers_only=%d\n", https_trackers_only ? 1 : 0);
    std::fprintf(fp, "enable_dht=%d\n", enable_dht ? 1 : 0);
    std::fprintf(fp, "enable_pex=%d\n", enable_pex ? 1 : 0);
    std::fprintf(fp, "no_upload=%d\n", no_upload ? 1 : 0);
    std::fprintf(fp, "remote_enabled=%d\n", remote_enabled ? 1 : 0);
    std::fprintf(fp, "# max_active: 0 = sans limite\n");
    std::fprintf(fp, "max_active=%d\n", max_active);
    std::fclose(fp);
    return true;
}

}  // namespace ui
