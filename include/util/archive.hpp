// Extraction d'archives ZIP, pour que n'importe quel torrent serve à quelque
// chose et pas seulement ceux qui arrivent déjà décompressés.
//
// ZIP uniquement, et c'est assumé : RAR et 7z demandent des bibliothèques qui
// pèsent plus lourd que le reste de l'application, quand elles sont seulement
// redistribuables.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace util {

// Suivi partagé entre le fil qui extrait et l'interface qui l'affiche.
struct ExtractProgress {
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> ok{false};
    std::atomic<uint32_t> files_done{0};
    std::atomic<uint32_t> files_total{0};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};

    mutable std::mutex mutex;
    std::string current;  // fichier en cours
    std::string message;  // erreur, ou résumé une fois terminé

    std::string current_file() const;
    std::string last_message() const;
    int percent() const;
};

bool looks_like_zip(const std::string& path);

// Extrait `zip_path` dans `dest_dir`, créé au besoin. Bloquant : à appeler
// depuis un fil dédié. Renseigne `progress` au fur et à mesure.
bool extract_zip(const std::string& zip_path, const std::string& dest_dir,
                 ExtractProgress& progress);

}  // namespace util
