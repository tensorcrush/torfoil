#include "ui/settings.hpp"

#include <cstdio>
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
        {"Exiger le VPN",
         "rien ne sort si le tunnel tombe · aucun risque de fuite, arrêt total sinon",
         &Settings::require_vpn, false},
        {"Trackers chiffrés seulement",
         "refuse les annonces en clair qui révèlent ce que vous téléchargez · moins de trackers",
         &Settings::https_trackers_only, false},
        {"Désactiver le DHT",
         "plus aucune requête UDP en clair · beaucoup moins de pairs trouvés",
         &Settings::enable_dht, true},
        {"Désactiver PEX",
         "les pairs n'apprennent plus ce que vous connaissez · découverte plus lente",
         &Settings::enable_pex, true},
        {"Ne rien partager",
         "aucun envoi vers les autres · vous serez étranglé plus souvent, débit réduit",
         &Settings::no_upload, false},
    };
    return list;
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
        const bool value = parse_bool(text.substr(eq + 1));

        if (key == "require_vpn") require_vpn = value;
        else if (key == "https_trackers_only") https_trackers_only = value;
        else if (key == "enable_dht") enable_dht = value;
        else if (key == "enable_pex") enable_pex = value;
        else if (key == "no_upload") no_upload = value;
    }
    std::fclose(fp);
    return true;
}

bool Settings::save(const std::string& path) const {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    std::fprintf(fp, "# Réglages Torfoil — modifiables ici ou depuis l'onglet Réglages\n");
    std::fprintf(fp, "require_vpn=%d\n", require_vpn ? 1 : 0);
    std::fprintf(fp, "https_trackers_only=%d\n", https_trackers_only ? 1 : 0);
    std::fprintf(fp, "enable_dht=%d\n", enable_dht ? 1 : 0);
    std::fprintf(fp, "enable_pex=%d\n", enable_pex ? 1 : 0);
    std::fprintf(fp, "no_upload=%d\n", no_upload ? 1 : 0);
    std::fclose(fp);
    return true;
}

}  // namespace ui
