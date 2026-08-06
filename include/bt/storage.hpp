// Écriture des pièces sur la carte SD.
//
// Un torrent est un flux d'octets continu découpé en pièces ; les fichiers, eux,
// sont des tranches arbitraires de ce flux. Une pièce peut donc chevaucher
// plusieurs fichiers, et c'est ici qu'on fait la conversion.
//
// Contraintes Switch prises en compte :
//   * peu de descripteurs disponibles → cache LRU de handles ouverts ;
//   * heap réduit en mode applet → vérification SHA-1 par blocs de 64 Ko, jamais
//     de pièce entière en mémoire.
#pragma once

#include <cstdio>
#include <functional>
#include <mutex>
#include <utility>
#include <string>
#include <vector>

#include "bt/metainfo.hpp"

namespace bt {

class Storage {
public:
    // Taille maximale d'un fichier sur FAT32 : 4 Go moins un octet.
    static constexpr uint64_t kFat32FileLimit = 0xFFFFFFFFull;

    Storage() = default;
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Crée l'arborescence sous `base_dir` et prépare les fichiers.
    bool open(const MetaInfo& meta, const std::string& base_dir, std::string* err = nullptr);
    void close();

    bool write_block(uint32_t piece, uint32_t offset, const uint8_t* data, size_t len,
                     std::string* err = nullptr);
    bool read_block(uint32_t piece, uint32_t offset, uint8_t* data, size_t len);

    // Compare la pièce au SHA-1 du .torrent, depuis la mémoire si elle y est
    // encore, depuis la carte sinon.
    bool verify_piece(uint32_t piece);

    // Écrit la pièce validée sur la carte, d'un seul tenant, et libère son
    // tampon. À appeler après un verify_piece() concluant.
    bool commit_piece(uint32_t piece, std::string* err = nullptr);
    // Abandonne une pièce refusée : elle sera redemandée.
    void discard_piece(uint32_t piece);

    // --- chemin asynchrone -------------------------------------------------
    //
    // Hacher 2 Mio puis les écrire sur la carte prend des dizaines de
    // millisecondes. Fait sur le fil réseau, ce temps est du silence : rien ne
    // lit les sockets pendant ce temps, elles débordent, et TCP prend ces
    // pertes pour de la congestion. Ces trois méthodes permettent de confier le
    // travail à un autre fil.

    // Retire de la mémoire une pièce ENTIÈREMENT présente et en cède le
    // contenu. Échoue si elle est partielle : ce cas reste synchrone, il
    // suppose de relire la carte.
    bool take_complete_piece(uint32_t piece, std::vector<uint8_t>& out);

    // Compare un tampon fourni au SHA-1 du .torrent. Ne touche à aucun état :
    // appelable depuis n'importe quel fil.
    bool hash_matches(uint32_t piece, const uint8_t* data, size_t len) const;

    // Écrit une pièce complète. Prend le verrou des fichiers.
    bool write_piece(uint32_t piece, const uint8_t* data, size_t len, std::string* err);

    // Passe tout le contenu en revue au démarrage pour reprendre un
    // téléchargement interrompu. `out_have` reçoit un bit par pièce validée.
    //
    // Sur un torrent de 35 Go cette relecture dure un quart d'heure : le rappel
    // peut renvoyer false pour l'interrompre. Ce qui a déjà été vérifié reste
    // acquis, le reste sera simplement redemandé. Renvoie false si on s'est
    // arrêté en chemin.
    bool scan_existing(std::vector<bool>& out_have,
                       const std::function<bool(uint32_t done, uint32_t total)>& progress);

    // Vrai si tous les fichiers viennent d'être créés par ce open() : il n'y a
    // alors rien à relire, et sur un torrent de 35 Go ça épargne une
    // vérification interminable.
    bool created_fresh() const { return created_fresh_; }

    // Vrai si un fichier a dû être recréé sous une autre forme : ce qui était
    // téléchargé est perdu, et tout point de reprise devient un mensonge.
    bool recreated() const { return recreated_; }

    // Seuil au-delà duquel un fichier réclame un traitement spécial. Vaut la
    // limite de FAT32 en usage réel ; les tests l'abaissent pour éprouver ce
    // chemin sans avoir à fabriquer un fichier de 4 Go.
    void set_large_file_threshold(uint64_t bytes) { large_file_threshold_ = bytes; }

    // Remplace le test « ce fichier peut-il atteindre sa taille finale ? ».
    // On ne peut pas fabriquer une carte FAT32 dans un test, mais on peut
    // vérifier ce qu'on fait de son refus — et c'est là qu'était le bug.
    using GrowthProbe = std::function<bool(const std::string& path, uint64_t target)>;
    void set_growth_probe(GrowthProbe probe) { growth_probe_ = std::move(probe); }

    const std::string& base_dir() const { return base_dir_; }
    // Chemin absolu du fichier principal (le plus gros) — sert à l'installeur.
    std::string largest_file_path() const;

private:
    struct FileSlot {
        std::string abs_path;
        uint64_t offset = 0;
        uint64_t length = 0;
        std::FILE* handle = nullptr;
        uint64_t last_used = 0;
        // Taille réellement allouée sur la carte. Écrire au-delà sans avoir
        // agrandi le fichier échoue sur le système de fichiers de la console.
        uint64_t allocated = 0;
    };

    std::FILE* acquire(FileSlot& slot);
    void evict_if_needed();
    bool ensure_directories(const std::string& abs_path, std::string* err);
    // Agrandit le fichier si l'écriture demandée dépasse ce qui est alloué.
    bool ensure_allocated(FileSlot& slot, uint64_t needed);
    // Crée en « fichier concaténé » ce qui dépasse la limite de FAT32.
    bool prepare_large_file(FileSlot& slot, std::string* err);
    bool can_grow_to_full_size(FileSlot& slot);
    // Crée le fichier volumineux par le moyen propre à la plateforme.
    bool create_large_file(FileSlot& slot, std::string* err);

    // Applique `fn` à chaque tranche (fichier, décalage local, longueur)
    // recouvrant [abs_offset, abs_offset+len).
    template <typename Fn>
    bool for_each_span(uint64_t abs_offset, size_t len, Fn fn);

    MetaInfo meta_;
    std::string base_dir_;
    std::vector<FileSlot> files_;
    uint64_t clock_ = 0;
    size_t open_handles_ = 0;
    // Dernière erreur système rencontrée, pour un message utile plutôt qu'un
    // « écriture impossible » qui n'apprend rien.
    std::string last_error_;
    bool created_fresh_ = true;
    bool recreated_ = false;
    uint64_t large_file_threshold_ = kFat32FileLimit;
    GrowthProbe growth_probe_;

    // Une pièce en cours d'assemblage, gardée en mémoire.
    //
    // L'intérêt est double, et décisif sur une console : la vérification SHA-1
    // se fait sans relire la carte (une pièce peut peser 16 Mo), et la pièce
    // validée part en une seule écriture séquentielle au lieu de mille écritures
    // de 16 Ko éparpillées. Une carte SD déteste le second régime.
    struct PieceBuffer {
        uint32_t index = 0;
        std::vector<uint8_t> data;
        std::vector<bool> have;  // un bit par bloc de 16 Ko
        uint32_t blocks_present = 0;
        uint64_t last_used = 0;
    };

    PieceBuffer* buffer_for(uint32_t piece, bool create);
    bool flush_buffer(PieceBuffer& buf, std::string* err);
    bool write_through(uint32_t piece, uint32_t offset, const uint8_t* data, size_t len,
                       std::string* err);

    std::vector<PieceBuffer> buffers_;

    // Protège les descripteurs et le cache LRU, seules ressources réellement
    // partagées avec le fil d'écriture. Les tampons de pièces, eux, restent la
    // propriété du fil moteur.
    mutable std::recursive_mutex file_mutex_;

    static constexpr size_t kMaxOpenHandles = 8;
    static constexpr size_t kHashChunk = 64 * 1024;
    static constexpr uint32_t kBlockSize = 16 * 1024;
    // Au-delà, une pièce coûterait trop cher en mémoire pour le mode applet :
    // on retombe alors sur l'écriture directe.
    static constexpr uint64_t kMaxBufferedPiece = 16ull * 1024 * 1024;
    static constexpr uint64_t kBufferBudget = 48ull * 1024 * 1024;
};

}  // namespace bt
