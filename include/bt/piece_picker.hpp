// Choix des pièces et des blocs à demander.
//
// Stratégie « rarest first » : on réclame en priorité ce que peu de pairs
// possèdent, ce qui maintient l'essaim en vie et évite de finir bloqué sur une
// pièce que plus personne n'a. Les pièces déjà entamées passent avant les
// nouvelles, pour limiter le nombre de pièces en vol (donc la mémoire).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bt/metainfo.hpp"

namespace bt {

class Bitfield {
public:
    void resize(uint32_t bits);
    void clear();

    bool get(uint32_t index) const;
    void set(uint32_t index, bool value = true);

    uint32_t size() const { return bits_; }
    uint32_t count() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool full() const { return bits_ > 0 && count_ == bits_; }

    const std::vector<uint8_t>& bytes() const { return data_; }

    // Charge un message « bitfield » reçu d'un pair. Refuse un padding non nul
    // (BEP 3 l'exige, et c'est un bon indicateur de pair cassé/hostile).
    bool from_bytes(const uint8_t* data, size_t len, uint32_t bits);

private:
    std::vector<uint8_t> data_;
    uint32_t bits_ = 0;
    uint32_t count_ = 0;
};

struct BlockRequest {
    uint32_t piece = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
};

class PiecePicker {
public:
    // Taille de bloc universelle. Au-delà de 16 Ko la plupart des clients
    // refusent la requête.
    static constexpr uint32_t kBlockSize = 16 * 1024;

    void init(const MetaInfo& meta);

    // --- état local ---
    const Bitfield& have() const { return have_; }
    bool is_complete() const { return have_.full(); }
    uint64_t bytes_done() const { return bytes_done_; }
    float progress() const;

    // Marque une pièce comme acquise (reprise sur disque au démarrage).
    void set_have(uint32_t piece);

    // --- disponibilité chez les pairs ---
    void peer_added(const Bitfield& pieces);
    void peer_removed(const Bitfield& pieces);
    void peer_has(uint32_t piece);

    // --- sélection ---
    // Remplit `out` avec au plus `max` blocs à demander à ce pair.
    // Renvoie le nombre effectivement choisi.
    uint32_t pick(const Bitfield& peer_pieces, BlockRequest* out, uint32_t max, uint64_t now_ms);

    // --- retours ---
    // Renvoie true si la pièce est désormais complète et doit être vérifiée.
    bool on_block_received(uint32_t piece, uint32_t offset, uint32_t length);
    // Requête abandonnée (pair déconnecté, choke) : le bloc redevient libre.
    void on_request_dropped(uint32_t piece, uint32_t offset, uint32_t length);
    // Après relecture SHA-1 par Storage.
    void on_piece_verified(uint32_t piece, bool ok);

    // Libère les blocs demandés depuis trop longtemps.
    uint32_t expire_requests(uint64_t now_ms, uint64_t timeout_ms);

    uint32_t pieces_in_flight() const { return static_cast<uint32_t>(progress_.size()); }

private:
    enum class BlockState : uint8_t { Free = 0, Pending = 1, Done = 2 };

    struct PieceProgress {
        std::vector<BlockState> blocks;
        uint32_t done = 0;
        uint32_t pending = 0;
    };

    uint32_t blocks_in_piece(uint32_t piece) const;
    PieceProgress& progress_for(uint32_t piece);
    bool endgame() const;

    MetaInfo meta_;
    Bitfield have_;
    std::vector<uint16_t> availability_;  // saturé à 65535, largement suffisant
    std::unordered_map<uint32_t, PieceProgress> progress_;

    struct Pending {
        uint32_t piece;
        uint32_t offset;
        uint32_t length;
        uint64_t requested_ms;
    };
    std::vector<Pending> pending_;

    uint64_t bytes_done_ = 0;
    uint32_t pick_cursor_ = 0;  // évite de repartir de zéro à chaque appel
};

}  // namespace bt
