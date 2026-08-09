#include "diag/selftest.hpp"

#include <sys/statvfs.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "bt/session.hpp"
#include "net/tls.hpp"
#include "net/transport.hpp"
#include "util/bytes.hpp"
#include "util/log.hpp"

namespace diag {

namespace {

Check pass(const std::string& name, const std::string& detail) {
    Check c;
    c.name = name;
    c.ran = true;
    c.ok = true;
    c.detail = detail;
    return c;
}

Check fail(const std::string& name, const std::string& detail) {
    Check c;
    c.name = name;
    c.ran = true;
    c.ok = false;
    c.detail = detail;
    return c;
}

// Motif reconnaissable : des zéros passeraient inaperçus si rien n'était écrit.
void fill_pattern(uint8_t* buf, size_t len, uint64_t seed) {
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>((seed + i * 31 + (i >> 8)) & 0xff);
    }
}

}  // namespace

Check check_large_file(const std::string& dir, const StepFn& step) {
    const std::string name = "Carte SD : fichier de plus de 4 Go";

    const std::string path = dir + "/.torfoil-selftest.bin";
    const uint64_t beyond = 0x100000000ull + 4096;  // 4 Go + un peu
    constexpr size_t kSize = 64 * 1024;
    const uint64_t needed = beyond + kSize;

    // Ce test réserve réellement plus de 4 Go : sans place, il échouerait en
    // accusant le mécanisme de découpage alors que seule la carte est pleine.
    struct statvfs vfs{};
    if (::statvfs(dir.c_str(), &vfs) == 0 && vfs.f_frsize > 0 && vfs.f_blocks > 0) {
        const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        if (free_bytes < needed + 64 * 1024 * 1024) {
            Check c;
            c.name = name;
            c.ran = false;
            c.detail = "non exécuté : il faut environ 4,2 Go libres sur la carte, il en reste " +
                       util::human_size(free_bytes);
            return c;
        }
    }

    if (step) step(name + " — réservation de 4 Go, cela peut prendre une minute");

    std::remove(path.c_str());

#ifdef __SWITCH__
    // Même mécanisme que pour un vrai téléchargement : sur FAT32 un fichier
    // ordinaire ne peut pas dépasser 4 Go, il faut un fichier concaténé.
    const Result rc = fsdevCreateFile(path.c_str(), needed, FsCreateOption_BigFile);
    if (R_FAILED(rc)) {
        char code[24];
        std::snprintf(code, sizeof(code), "0x%x", rc);
        return fail(name, std::string("création du fichier découpé refusée [") + code +
                              "] — la carte ne gère pas le découpage au-delà de 4 Go");
    }
#else
    if (std::FILE* create = std::fopen(path.c_str(), "wb")) std::fclose(create);
#endif

    std::FILE* fp = std::fopen(path.c_str(), "r+b");
    if (!fp) {
        std::remove(path.c_str());
        return fail(name, "ouverture impossible après création");
    }

    std::vector<uint8_t> written(kSize);
    fill_pattern(written.data(), written.size(), 0xC0FFEE);

    bool ok = fseeko(fp, static_cast<off_t>(beyond), SEEK_SET) == 0;
    if (ok) ok = std::fwrite(written.data(), 1, written.size(), fp) == written.size();
    if (ok) ok = std::fflush(fp) == 0;

    if (!ok) {
        const int code = errno;
        std::fclose(fp);
        std::remove(path.c_str());
        return fail(name, "écriture au-delà de 4 Go refusée (erreur système " +
                              std::to_string(code) + ")");
    }

    // Relecture : écrire sans erreur ne prouve pas que les octets sont arrivés.
    std::vector<uint8_t> read_back(kSize, 0);
    ok = fseeko(fp, static_cast<off_t>(beyond), SEEK_SET) == 0;
    if (ok) ok = std::fread(read_back.data(), 1, read_back.size(), fp) == read_back.size();
    std::fclose(fp);

    if (!ok) {
        std::remove(path.c_str());
        return fail(name, "relecture au-delà de 4 Go impossible");
    }
    if (std::memcmp(written.data(), read_back.data(), kSize) != 0) {
        std::remove(path.c_str());
        return fail(name, "les octets relus diffèrent de ceux écrits — carte défaillante");
    }

#ifdef __SWITCH__
    // Un fichier concaténé est un dossier : le retirer demande le bon appel.
    if (R_FAILED(fsdevDeleteDirectoryRecursively(path.c_str()))) std::remove(path.c_str());
#else
    std::remove(path.c_str());
#endif

    return pass(name, "écriture et relecture à 4 Go + 4 Ko : identiques");
}

Check check_vpn_exit(bt::Session& session, const StepFn& step) {
    const std::string name = "VPN : sortie réelle du trafic";
    if (step) step(name);

    auto transport = session.transport_for_diagnostics();
    if (!transport || !transport->ready()) {
        return fail(name, "aucun transport réseau disponible");
    }

    // VPN inactif : interroger Mullvad ne prouverait rien et l'échec serait
    // trompeur. On le dit, plutôt que d'accuser un tunnel qui n'existe pas.
    const std::string transport_name = transport->name();
    if (transport_name.find("direct") != std::string::npos) {
        Check c;
        c.name = name;
        c.ran = false;
        c.detail = "non exécuté : le VPN n'est pas actif (onglet VPN, touche A), "
                   "le trafic passe en direct";
        return c;
    }

    net::HttpResponse response;
    std::string err;
    if (!net::https_request(*transport, "https://am.i.mullvad.net/json", "GET", "", "", response,
                            &err, 20000)) {
        return fail(name, "requête impossible : " + err);
    }
    if (response.status != 200) {
        return fail(name, "réponse HTTP " + std::to_string(response.status));
    }

    // On lit la réponse sans le parseur maison : la mesure doit être
    // indépendante du code qu'elle sert à valider.
    const std::string& body = response.body;
    auto field = [&](const std::string& key) {
        const std::string needle = "\"" + key + "\"";
        size_t pos = body.find(needle);
        if (pos == std::string::npos) return std::string{};
        pos = body.find(':', pos + needle.size());
        if (pos == std::string::npos) return std::string{};
        ++pos;
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '"')) ++pos;
        size_t end = pos;
        while (end < body.size() && body[end] != '"' && body[end] != ',' && body[end] != '}') {
            ++end;
        }
        return body.substr(pos, end - pos);
    };

    const std::string ip = field("ip");
    const bool is_mullvad = field("mullvad_exit_ip") == "true";

    if (ip.empty()) return fail(name, "réponse illisible");
    if (!is_mullvad) {
        return fail(name, "le trafic sort en clair par " + ip +
                              " — Mullvad ne reconnaît pas cette adresse");
    }
    return pass(name, "Mullvad confirme la sortie par " + ip);
}

Report run_all(const std::string& download_dir, bt::Session& session, const StepFn& step) {
    Report report;
    util::log_line("auto-diagnostic : début");

    report.checks.push_back(check_large_file(download_dir, step));
    report.checks.push_back(check_vpn_exit(session, step));

    for (const Check& c : report.checks) {
        const char* tag = !c.ran ? "IGNORÉ" : (c.ok ? "OK" : "ÉCHEC");
        util::log_line(std::string("auto-diagnostic ") + tag + " · " + c.name + " · " + c.detail);
    }
    return report;
}

}  // namespace diag
