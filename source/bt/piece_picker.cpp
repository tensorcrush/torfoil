#include "bt/piece_picker.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "util/bytes.hpp"

namespace bt {

// --------------------------------------------------------------------------
// Bitfield
// --------------------------------------------------------------------------

void Bitfield::resize(uint32_t bits) {
    bits_ = bits;
    data_.assign((bits + 7) / 8, 0);
    count_ = 0;
}

void Bitfield::clear() {
    std::fill(data_.begin(), data_.end(), 0);
    count_ = 0;
}

bool Bitfield::get(uint32_t index) const {
    if (index >= bits_) return false;
    // Bit de poids fort en premier, comme le message « bitfield » du protocole.
    return (data_[index >> 3] >> (7 - (index & 7))) & 1;
}

void Bitfield::set(uint32_t index, bool value) {
    if (index >= bits_) return;
    const uint8_t mask = static_cast<uint8_t>(1 << (7 - (index & 7)));
    const bool was = (data_[index >> 3] & mask) != 0;
    if (value == was) return;

    if (value) {
        data_[index >> 3] |= mask;
        ++count_;
    } else {
        data_[index >> 3] &= static_cast<uint8_t>(~mask);
        --count_;
    }
}

bool Bitfield::from_bytes(const uint8_t* data, size_t len, uint32_t bits) {
    const size_t expected = (bits + 7) / 8;
    if (len != expected) return false;

    // Les bits de bourrage du dernier octet doivent être à zéro.
    const uint32_t spare = static_cast<uint32_t>(expected * 8 - bits);
    if (spare > 0 && (data[expected - 1] & ((1u << spare) - 1)) != 0) return false;

    resize(bits);
    std::memcpy(data_.data(), data, len);

    count_ = 0;
    for (const uint8_t byte : data_) {
        for (int b = 0; b < 8; ++b) {
            if (byte & (1 << b)) ++count_;
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// PiecePicker
// --------------------------------------------------------------------------

void PiecePicker::init(const MetaInfo& meta) {
    meta_ = meta;
    have_.resize(meta.piece_count());
    availability_.assign(meta.piece_count(), 0);
    progress_.clear();
    pending_.clear();
    bytes_done_ = 0;
    pick_cursor_ = 0;
}

float PiecePicker::progress() const {
    if (meta_.total_size == 0) return 0.0f;
    return static_cast<float>(static_cast<double>(bytes_done_) /
                              static_cast<double>(meta_.total_size));
}

uint32_t PiecePicker::blocks_in_piece(uint32_t piece) const {
    const uint32_t size = meta_.size_of_piece(piece);
    return (size + kBlockSize - 1) / kBlockSize;
}

PiecePicker::PieceProgress& PiecePicker::progress_for(uint32_t piece) {
    auto it = progress_.find(piece);
    if (it != progress_.end()) return it->second;

    PieceProgress fresh;
    fresh.blocks.assign(blocks_in_piece(piece), BlockState::Free);
    return progress_.emplace(piece, std::move(fresh)).first->second;
}

void PiecePicker::set_have(uint32_t piece) {
    if (piece >= meta_.piece_count() || have_.get(piece)) return;
    have_.set(piece, true);
    bytes_done_ += meta_.size_of_piece(piece);
    progress_.erase(piece);
}

void PiecePicker::peer_added(const Bitfield& pieces) {
    const uint32_t n = std::min<uint32_t>(pieces.size(), meta_.piece_count());
    for (uint32_t i = 0; i < n; ++i) {
        if (pieces.get(i) && availability_[i] < 0xffff) ++availability_[i];
    }
}

void PiecePicker::peer_removed(const Bitfield& pieces) {
    const uint32_t n = std::min<uint32_t>(pieces.size(), meta_.piece_count());
    for (uint32_t i = 0; i < n; ++i) {
        if (pieces.get(i) && availability_[i] > 0) --availability_[i];
    }
}

void PiecePicker::peer_has(uint32_t piece) {
    if (piece < availability_.size() && availability_[piece] < 0xffff) ++availability_[piece];
}

bool PiecePicker::endgame() const {
    // Fin de partie : moins de 2 % restant. On autorise alors les requêtes en
    // double pour ne pas dépendre d'un seul pair lent sur les derniers blocs.
    const uint32_t total = meta_.piece_count();
    return total > 0 && (total - have_.count()) * 50 < total;
}

uint32_t PiecePicker::pick(const Bitfield& peer_pieces, BlockRequest* out, uint32_t max,
                           uint64_t now_ms) {
    if (max == 0 || meta_.piece_count() == 0) return 0;

    uint32_t picked = 0;
    const bool in_endgame = endgame();

    auto take_from = [&](uint32_t piece) {
        PieceProgress& prog = progress_for(piece);
        const uint32_t piece_size = meta_.size_of_piece(piece);

        for (uint32_t b = 0; b < prog.blocks.size() && picked < max; ++b) {
            const bool free = prog.blocks[b] == BlockState::Free;
            const bool retryable = in_endgame && prog.blocks[b] == BlockState::Pending;
            if (!free && !retryable) continue;

            const uint32_t offset = b * kBlockSize;
            const uint32_t length = std::min(kBlockSize, piece_size - offset);

            out[picked++] = BlockRequest{piece, offset, length};
            if (free) {
                prog.blocks[b] = BlockState::Pending;
                ++prog.pending;
            }
            pending_[block_key(piece, offset)] = now_ms;
        }
    };

    // 1) Terminer les pièces déjà entamées.
    for (auto& [piece, prog] : progress_) {
        if (picked >= max) break;
        if (have_.get(piece) || !peer_pieces.get(piece)) continue;
        if (prog.done + prog.pending >= prog.blocks.size() && !in_endgame) continue;
        take_from(piece);
    }

    // 2) Ouvrir une nouvelle pièce, la plus rare d'abord.
    while (picked < max) {
        uint32_t best = UINT32_MAX;
        uint16_t best_avail = 0xffff;

        const uint32_t total = meta_.piece_count();
        uint32_t weighed = 0;
        for (uint32_t step = 0; step < total && weighed < kRarestSample; ++step) {
            const uint32_t i = (pick_cursor_ + step) % total;
            if (have_.get(i) || !peer_pieces.get(i)) continue;
            if (progress_.count(i)) continue;  // déjà traitée en phase 1
            if (availability_[i] == 0) continue;

            // Le compteur ne monte que sur les pièces réellement candidates :
            // borner les tours de boucle bruts ferait renoncer un torrent
            // presque terminé, dont les rares pièces manquantes sont noyées
            // dans des milliers de pièces déjà acquises.
            ++weighed;
            if (availability_[i] < best_avail) {
                best_avail = availability_[i];
                best = i;
                if (best_avail == 1) break;  // on ne fera pas plus rare
            }
        }

        if (best == UINT32_MAX) break;
        pick_cursor_ = (best + 1) % meta_.piece_count();

        const uint32_t before = picked;
        take_from(best);
        if (picked == before) break;  // rien à prendre, on évite la boucle infinie
    }

    return picked;
}

bool PiecePicker::on_block_received(uint32_t piece, uint32_t offset, uint32_t length) {
    if (piece >= meta_.piece_count() || have_.get(piece)) return false;
    if (offset % kBlockSize != 0) return false;

    const uint32_t index = offset / kBlockSize;
    auto it = progress_.find(piece);
    if (it == progress_.end()) return false;

    PieceProgress& prog = it->second;
    if (index >= prog.blocks.size()) return false;

    // Retire la requête en attente correspondante.
    pending_.erase(block_key(piece, offset));

    if (prog.blocks[index] == BlockState::Done) return false;  // doublon (endgame)
    if (prog.blocks[index] == BlockState::Pending && prog.pending > 0) --prog.pending;

    prog.blocks[index] = BlockState::Done;
    ++prog.done;
    (void)length;

    return prog.done == prog.blocks.size();
}

void PiecePicker::on_request_dropped(uint32_t piece, uint32_t offset, uint32_t length) {
    (void)length;
    pending_.erase(block_key(piece, offset));

    auto it = progress_.find(piece);
    if (it == progress_.end()) return;

    const uint32_t index = offset / kBlockSize;
    if (index >= it->second.blocks.size()) return;
    if (it->second.blocks[index] == BlockState::Pending) {
        it->second.blocks[index] = BlockState::Free;
        if (it->second.pending > 0) --it->second.pending;
    }
}

void PiecePicker::on_piece_verified(uint32_t piece, bool ok) {
    if (ok) {
        set_have(piece);
        return;
    }

    // SHA-1 faux : un pair a menti ou la SD a fauté. On repart de zéro sur cette
    // pièce (le pair fautif est traité côté session, via le compteur d'erreurs).
    auto it = progress_.find(piece);
    if (it != progress_.end()) {
        std::fill(it->second.blocks.begin(), it->second.blocks.end(), BlockState::Free);
        it->second.done = 0;
        it->second.pending = 0;
    }

    for (auto p = pending_.begin(); p != pending_.end();) {
        p = (static_cast<uint32_t>(p->first >> 32) == piece) ? pending_.erase(p) : std::next(p);
    }
}

uint32_t PiecePicker::expire_requests(uint64_t now_ms, uint64_t timeout_ms) {
    uint32_t freed = 0;
    for (auto p = pending_.begin(); p != pending_.end();) {
        if (now_ms - p->second < timeout_ms) {
            ++p;
            continue;
        }

        const uint32_t piece = static_cast<uint32_t>(p->first >> 32);
        const uint32_t offset = static_cast<uint32_t>(p->first);
        p = pending_.erase(p);

        auto it = progress_.find(piece);
        if (it != progress_.end()) {
            const uint32_t index = offset / kBlockSize;
            if (index < it->second.blocks.size() &&
                it->second.blocks[index] == BlockState::Pending) {
                it->second.blocks[index] = BlockState::Free;
                if (it->second.pending > 0) --it->second.pending;
            }
        }
        ++freed;
    }
    return freed;
}

}  // namespace bt
