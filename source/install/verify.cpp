// Vérification d'un paquet, sans rien installer.
//
// Volontairement dans son propre fichier : contrairement à l'installation, rien
// ici ne touche à libnx. Le code se compile donc aussi sur PC, et se teste avec
// un NSP fabriqué pour l'occasion — ce qui est précieux, puisque c'est la seule
// partie de la chaîne d'installation qu'on peut éprouver hors console.
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "install/archive.hpp"
#include "install/installer.hpp"
#include "install/keys.hpp"
#include "install/nca.hpp"
#include "util/bytes.hpp"

namespace install {

namespace {

constexpr size_t kReadChunk = 1 * 1024 * 1024;

Outcome verify_failure(const std::string& message) {
    Outcome r;
    r.ok = false;
    r.message = message;
    return r;
}

const ArchiveEntry* lookup(const std::vector<ArchiveEntry>& entries, const std::string& name) {
    for (const ArchiveEntry& e : entries) {
        if (util::iequals(e.name, name)) return &e;
    }
    return nullptr;
}

}  // namespace

Outcome verify_package(const std::string& path, bool deep, const ProgressFn& progress) {
    std::string err;

    KeySet keys;
    if (!keys.load(&err)) return verify_failure(err);
    return verify_package_with_keys(path, keys, deep, progress);
}

Outcome verify_package_with_keys(const std::string& path, const KeySet& keys, bool deep,
                                 const ProgressFn& progress) {
    std::string err;

    FileReader file;
    if (!file.open(path)) return verify_failure("fichier introuvable : " + path);

    std::vector<ArchiveEntry> entries;
    if (!list_installable_contents(file, entries, &err)) return verify_failure(err);

    const ArchiveEntry* meta_entry = nullptr;
    for (const ArchiveEntry& e : entries) {
        if (e.name.size() > 9 && e.name.compare(e.name.size() - 9, 9, ".cnmt.nca") == 0) {
            meta_entry = &e;
            break;
        }
    }
    if (!meta_entry) return verify_failure("aucun NCA meta (.cnmt.nca) dans le paquet");

    NcaInfo meta_nca;
    if (!nca_read_header(file, meta_entry->offset, keys, meta_nca, &err)) {
        return verify_failure(err);
    }

    std::vector<uint8_t> cnmt_blob;
    if (!nca_extract_cnmt(file, meta_entry->offset, meta_nca, cnmt_blob, &err)) {
        return verify_failure(err);
    }

    CnmtInfo cnmt;
    if (!parse_cnmt(cnmt_blob, cnmt, &err)) return verify_failure(err);

    // Chaque contenu annoncé doit être présent, à la bonne taille, et tenir
    // entièrement dans le fichier. Un téléchargement tronqué se trahit ici, avant
    // d'avoir touché quoi que ce soit.
    uint64_t total_bytes = meta_entry->size;
    for (const ContentRecord& record : cnmt.contents) {
        const std::string name = util::to_hex(record.nca_id, 16) + ".nca";
        const ArchiveEntry* entry = lookup(entries, name);
        if (!entry) return verify_failure("contenu manquant : " + name);
        if (entry->size != record.size) {
            return verify_failure("taille incohérente pour " + name + " : " +
                                  util::human_size(entry->size) + " présents, " +
                                  util::human_size(record.size) + " annoncés");
        }
        if (entry->offset + entry->size > file.size()) {
            return verify_failure("paquet tronqué : " + name + " dépasse la fin du fichier");
        }
        total_bytes += record.size;
    }

    if (deep) {
        // Le CNMT porte le SHA-256 de chaque NCA. Le recalculer est la seule
        // preuve qu'aucun octet n'a été altéré — un bit retourné sur la carte SD
        // produit sinon une installation « réussie » et un jeu qui plante.
        uint64_t done = 0;
        std::vector<uint8_t> buffer(kReadChunk);

        for (const ContentRecord& record : cnmt.contents) {
            const std::string name = util::to_hex(record.nca_id, 16) + ".nca";
            const ArchiveEntry* entry = lookup(entries, name);
            if (!entry) continue;

            mbedtls_sha256_context sha;
            mbedtls_sha256_init(&sha);
            mbedtls_sha256_starts_ret(&sha, 0);

            uint64_t read = 0;
            while (read < entry->size) {
                const size_t chunk =
                    static_cast<size_t>(std::min<uint64_t>(kReadChunk, entry->size - read));
                if (!file.read(entry->offset + read, buffer.data(), chunk)) {
                    mbedtls_sha256_free(&sha);
                    return verify_failure("lecture impossible dans " + name);
                }
                mbedtls_sha256_update_ret(&sha, buffer.data(), chunk);
                read += chunk;
                done += chunk;

                if (progress) {
                    Progress p;
                    p.step = "Vérification de " + name.substr(0, 8) + "…";
                    p.done = done;
                    p.total = total_bytes;
                    if (!progress(p)) {
                        mbedtls_sha256_free(&sha);
                        return verify_failure("vérification annulée");
                    }
                }
            }

            uint8_t digest[32];
            mbedtls_sha256_finish_ret(&sha, digest);
            mbedtls_sha256_free(&sha);

            if (std::memcmp(digest, record.hash, 32) != 0) {
                return verify_failure("contenu corrompu : " + name +
                                      " ne correspond pas à son empreinte SHA-256");
            }
        }
    }

    Outcome result;
    result.ok = true;
    result.title_id = cnmt.application_id();
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(result.title_id));
    result.title_id_hex = hex;
    result.message = std::string(deep ? "Paquet vérifié, empreintes exactes — "
                                      : "Paquet cohérent — ") +
                     std::to_string(cnmt.contents.size()) + " contenus, " +
                     util::human_size(total_bytes);
    return result;
}

}  // namespace install
