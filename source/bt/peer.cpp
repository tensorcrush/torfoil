#include "bt/peer.hpp"

#include <algorithm>
#include <cstring>

#include "bt/bencode.hpp"
#include "util/clock.hpp"

namespace bt {

namespace {

const char kProtocol[] = "BitTorrent protocol";
constexpr size_t kProtocolLen = 19;
constexpr size_t kHandshakeLen = 1 + kProtocolLen + 8 + 20 + 20;  // 68

// Un message « piece » transporte au plus 16 Ko + 9 octets d'en-tête. On refuse
// tout ce qui dépasse largement : un pair hostile pourrait annoncer 4 Go et
// nous faire allouer dans le vide.
constexpr uint32_t kMaxMessageLen = 64 * 1024;

constexpr uint64_t kKeepAliveMs = 100 * 1000;
constexpr uint64_t kIdleTimeoutMs = 180 * 1000;
constexpr uint64_t kHandshakeTimeoutMs = 20 * 1000;
// Un pair joignable répond au SYN en bien moins que ça, même à l'autre bout du
// monde. Au-delà, il est derrière un NAT ou éteint : on passe au suivant.
constexpr uint64_t kConnectTimeoutMs = 7 * 1000;
// Doit rester égal au kBlockTimeoutMs de la session : les deux comptabilités
// doivent oublier le même bloc au même moment.
constexpr uint64_t kBlockTimeoutMs = 45 * 1000;

}  // namespace

PeerConnection::PeerConnection(net::Transport& transport, PeerHandler& handler,
                               const net::Endpoint& endpoint, const util::Hash160& info_hash,
                               const std::array<uint8_t, 20>& my_peer_id, bool advertise_pex)
    : transport_(transport),
      handler_(handler),
      endpoint_(endpoint),
      info_hash_(info_hash),
      my_peer_id_(my_peer_id),
      advertise_pex_(advertise_pex) {
    in_.reserve(kMaxMessageLen);
}

PeerConnection::~PeerConnection() {
    if (sock_) sock_->close();
}

bool PeerConnection::start() {
    if (!transport_.ready()) return false;

    sock_ = transport_.tcp();
    if (!sock_) return false;
    if (!sock_->start_connect(endpoint_)) {
        sock_.reset();
        return false;
    }

    state_ = State::Connecting;
    connected_at_ms_ = util::now_ms();
    last_activity_ms_ = connected_at_ms_;
    last_keepalive_ms_ = connected_at_ms_;
    return true;
}

void PeerConnection::close(const char* reason) {
    if (state_ == State::Closed) return;
    state_ = State::Closed;
    if (sock_) {
        sock_->close();
        sock_.reset();
    }
    handler_.on_peer_closed(*this, reason);
}

// ---------------------------------------------------------------------------
// Émission
// ---------------------------------------------------------------------------

void PeerConnection::queue(const uint8_t* data, size_t len) {
    out_.insert(out_.end(), data, data + len);
}

void PeerConnection::queue_message(uint8_t id, const uint8_t* payload, size_t len) {
    uint8_t header[5];
    util::wr_be32(header, static_cast<uint32_t>(len + 1));
    header[4] = id;
    queue(header, 5);
    if (len > 0) queue(payload, len);
}

void PeerConnection::flush() {
    if (!sock_ || out_pos_ >= out_.size()) return;

    while (out_pos_ < out_.size()) {
        const int n = sock_->send(out_.data() + out_pos_, out_.size() - out_pos_);
        if (n > 0) {
            out_pos_ += static_cast<size_t>(n);
            continue;
        }
        if (n == net::kWouldBlock) break;
        close("émission impossible");
        return;
    }

    if (out_pos_ >= out_.size()) {
        out_.clear();
        out_pos_ = 0;
    } else if (out_pos_ > 64 * 1024) {
        out_.erase(out_.begin(), out_.begin() + static_cast<long>(out_pos_));
        out_pos_ = 0;
    }
}

void PeerConnection::send_handshake() {
    uint8_t hs[kHandshakeLen];
    hs[0] = static_cast<uint8_t>(kProtocolLen);
    std::memcpy(hs + 1, kProtocol, kProtocolLen);
    std::memset(hs + 20, 0, 8);
    hs[20 + 5] = 0x10;  // BEP 10 : messagerie étendue
    std::memcpy(hs + 28, info_hash_.data(), 20);
    std::memcpy(hs + 48, my_peer_id_.data(), 20);
    queue(hs, kHandshakeLen);
}

void PeerConnection::send_extended_handshake() {
    Value m = Value::dict();
    m.d.emplace("ut_metadata", Value::from_int(kExtMetadata));
    // ut_pex : les pairs s'échangent leurs carnets d'adresses. Sur un magnet
    // dont les trackers sont morts, c'est souvent ce qui sauve le téléchargement.
    if (advertise_pex_) m.d.emplace("ut_pex", Value::from_int(kExtPex));

    Value root = Value::dict();
    root.d.emplace("m", std::move(m));
    root.d.emplace("v", Value::from_str("Torfoil 0.1"));
    root.d.emplace("reqq", Value::from_int(250));

    const std::string payload = bencode(root);

    std::vector<uint8_t> buf;
    buf.reserve(payload.size() + 1);
    buf.push_back(0);  // identifiant étendu 0 = poignée de main
    buf.insert(buf.end(), payload.begin(), payload.end());
    queue_message(kExtended, buf.data(), buf.size());
}

void PeerConnection::send_interested(bool yes) {
    if (am_interested_ == yes) return;
    am_interested_ = yes;
    queue_message(yes ? kInterested : kNotInterested, nullptr, 0);
    flush();
}

void PeerConnection::send_request(const BlockRequest& req) {
    uint8_t payload[12];
    util::wr_be32(payload, req.piece);
    util::wr_be32(payload + 4, req.offset);
    util::wr_be32(payload + 8, req.length);
    queue_message(kRequest, payload, sizeof(payload));
    requested_.push_back(OutstandingRequest{req, util::now_ms()});
    flush();
}

std::vector<BlockRequest> PeerConnection::outstanding() const {
    std::vector<BlockRequest> out;
    out.reserve(requested_.size());
    for (const OutstandingRequest& r : requested_) out.push_back(r.req);
    return out;
}

void PeerConnection::send_cancel(const BlockRequest& req) {
    uint8_t payload[12];
    util::wr_be32(payload, req.piece);
    util::wr_be32(payload + 4, req.offset);
    util::wr_be32(payload + 8, req.length);
    queue_message(kCancel, payload, sizeof(payload));

    requested_.erase(std::remove_if(requested_.begin(), requested_.end(),
                                    [&](const OutstandingRequest& r) {
                                        return r.req.piece == req.piece &&
                                               r.req.offset == req.offset;
                                    }),
                     requested_.end());
    flush();
}

void PeerConnection::send_have(uint32_t piece) {
    uint8_t payload[4];
    util::wr_be32(payload, piece);
    queue_message(kHave, payload, sizeof(payload));
    flush();
}

void PeerConnection::send_bitfield(const Bitfield& bits) {
    if (bits.size() == 0) return;
    queue_message(kBitfield, bits.bytes().data(), bits.bytes().size());
    flush();
}

void PeerConnection::send_choke(bool choked) {
    if (am_choking_ == choked) return;
    am_choking_ = choked;
    queue_message(choked ? kChoke : kUnchoke, nullptr, 0);
    flush();
}

void PeerConnection::send_block(uint32_t piece, uint32_t offset, const uint8_t* data,
                                uint32_t len) {
    uint8_t header[8];
    util::wr_be32(header, piece);
    util::wr_be32(header + 4, offset);

    std::vector<uint8_t> payload;
    payload.reserve(8 + len);
    payload.insert(payload.end(), header, header + 8);
    payload.insert(payload.end(), data, data + len);

    queue_message(kPiece, payload.data(), payload.size());
    bytes_up_ += len;
    flush();
}

void PeerConnection::request_metadata_piece(uint32_t index) {
    if (ut_metadata_id_ == 0) return;

    Value req = Value::dict();
    req.d.emplace("msg_type", Value::from_int(0));  // 0 = request
    req.d.emplace("piece", Value::from_int(index));
    const std::string payload = bencode(req);

    std::vector<uint8_t> buf;
    buf.reserve(payload.size() + 1);
    buf.push_back(ut_metadata_id_);
    buf.insert(buf.end(), payload.begin(), payload.end());
    queue_message(kExtended, buf.data(), buf.size());
    flush();
}

// ---------------------------------------------------------------------------
// Réception
// ---------------------------------------------------------------------------

void PeerConnection::compact_in() {
    if (in_pos_ == 0) return;
    if (in_pos_ >= in_.size()) {
        in_.clear();
        in_pos_ = 0;
        return;
    }
    // On ne recopie que si le gâchis devient significatif.
    if (in_pos_ > 32 * 1024) {
        in_.erase(in_.begin(), in_.begin() + static_cast<long>(in_pos_));
        in_pos_ = 0;
    }
}

bool PeerConnection::consume_handshake() {
    if (in_.size() - in_pos_ < kHandshakeLen) return false;

    const uint8_t* hs = in_.data() + in_pos_;
    if (hs[0] != kProtocolLen || std::memcmp(hs + 1, kProtocol, kProtocolLen) != 0) {
        close("protocole inconnu");
        return false;
    }
    if (std::memcmp(hs + 28, info_hash_.data(), 20) != 0) {
        close("info_hash différent");
        return false;
    }

    const bool extended = (hs[20 + 5] & 0x10) != 0;
    std::memcpy(their_peer_id_.data(), hs + 48, 20);
    in_pos_ += kHandshakeLen;

    state_ = State::Ready;
    // Un pair nous étrangle tant qu'il ne dit pas le contraire : le chronomètre
    // part dès la poignée de main, sinon un pair qui ne dit jamais rien du tout
    // ne serait jamais considéré comme improductif.
    choked_since_ms_ = util::now_ms();
    if (extended) send_extended_handshake();
    flush();

    handler_.on_peer_ready(*this);
    return true;
}

void PeerConnection::handle_extended(const uint8_t* data, uint32_t len) {
    if (len < 1) return;

    const uint8_t ext_id = data[0];
    const std::string payload(reinterpret_cast<const char*>(data + 1), len - 1);

    if (ext_id == 0) {
        // Poignée de main étendue : on y apprend l'identifiant que CE pair
        // attribue à ut_metadata, et la taille des métadonnées.
        Value root;
        if (!bdecode(payload, root, nullptr) || !root.is_dict()) return;

        if (const Value* m = root.find_dict("m")) {
            if (const Value* id = m->find_int("ut_metadata")) {
                ut_metadata_id_ = static_cast<uint8_t>(id->i);
            }
            if (const Value* id = m->find_int("ut_pex")) {
                ut_pex_id_ = static_cast<uint8_t>(id->i);
            }
        }
        if (const Value* size = root.find_int("metadata_size")) {
            if (size->i > 0 && size->i < 16 * 1024 * 1024) {
                metadata_size_ = static_cast<uint32_t>(size->i);
                handler_.on_peer_metadata_size(*this, metadata_size_);
            }
        }
        if (const Value* v = root.find_str("v")) client_name_ = v->s;
        return;
    }

    // L'identifiant est celui que NOUS avons annoncé dans notre poignée de main
    // étendue : le pair s'en sert pour nous adresser ses messages.
    if (ext_id == kExtPex) {
        Value dict;
        if (!bdecode(payload, dict, nullptr) || !dict.is_dict()) return;

        // « added » : suite de blocs de 6 octets, IPv4 puis port en gros-boutiste
        // (le même format compact que les trackers, BEP 23).
        const Value* added = dict.find_str("added");
        if (!added) return;

        std::vector<net::Endpoint> found;
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(added->s.data());
        const size_t count = added->s.size() / 6;
        found.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            net::Endpoint ep;
            ep.ipv4 = util::rd_be32(raw + i * 6);
            ep.port = util::rd_be16(raw + i * 6 + 4);
            if (ep.valid()) found.push_back(ep);
        }
        if (!found.empty()) handler_.on_peers_discovered(*this, found);
        return;
    }

    if (ext_id != kExtMetadata) return;

    // Message ut_metadata : dictionnaire bencode suivi, pour msg_type=1, des
    // octets bruts de la pièce.
    Value dict;
    std::string err;
    if (!bdecode(payload, dict, &err) || !dict.is_dict()) return;

    const int64_t msg_type = dict.int_or("msg_type", -1);
    const int64_t piece = dict.int_or("piece", -1);
    if (piece < 0) return;

    if (msg_type == 1) {
        const size_t header_len = dict.raw_end - dict.raw_begin;
        if (payload.size() <= header_len) return;
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(payload.data()) + header_len;
        const uint32_t raw_len = static_cast<uint32_t>(payload.size() - header_len);

        if (const Value* total = dict.find_int("total_size")) {
            if (total->i > 0 && metadata_size_ == 0) {
                metadata_size_ = static_cast<uint32_t>(total->i);
                handler_.on_peer_metadata_size(*this, metadata_size_);
            }
        }
        handler_.on_peer_metadata_piece(*this, static_cast<uint32_t>(piece), raw, raw_len);
    }
    // msg_type 0 (request) et 2 (reject) : rien à faire tant qu'on ne sert pas
    // les métadonnées.
}

void PeerConnection::handle_message(const uint8_t* data, uint32_t len, uint64_t now_ms) {
    if (len == 0) return;  // keep-alive

    const uint8_t id = data[0];
    const uint8_t* payload = data + 1;
    const uint32_t plen = len - 1;

    switch (id) {
        case kChoke:
            if (!peer_choking_) choked_since_ms_ = now_ms;
            peer_choking_ = true;
            // Le pair ne servira aucune des requêtes en vol. On prévient AVANT
            // de vider la liste : la session en a besoin pour rendre les blocs
            // au picker tout de suite, plutôt que d'attendre leur expiration.
            handler_.on_peer_choke(*this, true);
            requested_.clear();
            break;

        case kUnchoke:
            peer_choking_ = false;
            choked_since_ms_ = 0;
            handler_.on_peer_choke(*this, false);
            break;

        case kInterested:
            peer_interested_ = true;
            break;

        case kNotInterested:
            peer_interested_ = false;
            break;

        case kHave: {
            if (plen < 4) break;
            const uint32_t piece = util::rd_be32(payload);
            if (piece_count_ == 0) {
                // Métadonnées pas encore connues : on mémorise dans le bitfield
                // brut, qui sera décodé par set_piece_count().
                const size_t byte = piece >> 3;
                if (byte < 1024 * 1024) {
                    if (pending_bitfield_.size() <= byte) pending_bitfield_.resize(byte + 1, 0);
                    pending_bitfield_[byte] |= static_cast<uint8_t>(1 << (7 - (piece & 7)));
                }
            } else {
                pieces_.set(piece, true);
            }
            handler_.on_peer_have(*this, piece);
            break;
        }

        case kBitfield:
            if (piece_count_ == 0) {
                pending_bitfield_.assign(payload, payload + plen);
            } else if (!pieces_.from_bytes(payload, plen, piece_count_)) {
                close("bitfield invalide");
                return;
            }
            handler_.on_peer_bitfield(*this);
            break;

        case kRequest: {
            if (plen < 12) break;
            handler_.on_peer_request(*this, util::rd_be32(payload), util::rd_be32(payload + 4),
                                     util::rd_be32(payload + 8));
            break;
        }

        case kPiece: {
            if (plen < 8) break;
            const uint32_t piece = util::rd_be32(payload);
            const uint32_t offset = util::rd_be32(payload + 4);
            const uint32_t block_len = plen - 8;

            requested_.erase(std::remove_if(requested_.begin(), requested_.end(),
                                            [&](const OutstandingRequest& r) {
                                                return r.req.piece == piece &&
                                                       r.req.offset == offset;
                                            }),
                             requested_.end());

            bytes_down_ += block_len;
            handler_.on_peer_block(*this, piece, offset, payload + 8, block_len);
            break;
        }

        case kCancel:
        case kPort:
            break;

        case kExtended:
            handle_extended(payload, plen);
            break;

        default:
            break;
    }

    (void)now_ms;
}

bool PeerConnection::consume_messages(uint64_t now_ms) {
    while (true) {
        const size_t available = in_.size() - in_pos_;
        if (available < 4) return true;

        const uint32_t msg_len = util::rd_be32(in_.data() + in_pos_);
        if (msg_len > kMaxMessageLen) {
            close("message démesuré");
            return false;
        }
        if (available < 4 + msg_len) return true;  // incomplet, on attend

        handle_message(in_.data() + in_pos_ + 4, msg_len, now_ms);
        if (state_ == State::Closed) return false;

        in_pos_ += 4 + msg_len;
        compact_in();
    }
}

// ---------------------------------------------------------------------------
// Boucle
// ---------------------------------------------------------------------------

void PeerConnection::fill_poll(net::PollItem& item) {
    item = net::PollItem{};
    if (!sock_) return;

    item.tcp = sock_.get();
    item.want_read = state_ != State::Connecting;
    item.want_write = state_ == State::Connecting || out_pos_ < out_.size();
}

void PeerConnection::on_poll(const net::PollItem& item, uint64_t now_ms) {
    if (state_ == State::Closed || !sock_) return;

    if (item.has_error) {
        close("socket en erreur");
        return;
    }

    if (state_ == State::Connecting) {
        if (!item.can_write) return;
        if (!sock_->finish_connect()) {
            close("connexion refusée");
            return;
        }
        state_ = State::Handshaking;
        send_handshake();
        flush();
        last_activity_ms_ = now_ms;
        return;
    }

    if (item.can_write) flush();
    if (state_ == State::Closed) return;

    if (item.can_read) {
        uint8_t chunk[16 * 1024];
        for (int i = 0; i < 8; ++i) {  // plusieurs lectures par tour, sans famine
            const int n = sock_->recv(chunk, sizeof(chunk));
            if (n > 0) {
                in_.insert(in_.end(), chunk, chunk + n);
                last_activity_ms_ = now_ms;
                if (in_.size() > 4 * 1024 * 1024) {
                    close("tampon de réception saturé");
                    return;
                }
                continue;
            }
            if (n == net::kWouldBlock) break;
            close(n == net::kClosed ? "pair déconnecté" : "erreur de lecture");
            return;
        }

        if (state_ == State::Handshaking) {
            if (!consume_handshake()) {
                if (state_ == State::Closed) return;
                return;  // poignée de main incomplète, on attend la suite
            }
        }
        if (state_ == State::Ready) {
            if (!consume_messages(now_ms)) return;
        }
    }
}

void PeerConnection::tick(uint64_t now_ms) {
    if (state_ == State::Closed) return;

    // Deux délais distincts, et c'est ce qui fait la différence : la majorité des
    // adresses venues du DHT sont derrière un NAT et n'aboutiront jamais. Leur
    // laisser le délai généreux de la poignée de main, c'est immobiliser un
    // emplacement vingt secondes pour rien — le moteur passe son temps à
    // attendre des morts au lieu d'essayer les vivants suivants.
    if (state_ == State::Connecting && now_ms - connected_at_ms_ > kConnectTimeoutMs) {
        close("connexion sans réponse");
        return;
    }

    if (state_ != State::Ready && now_ms - connected_at_ms_ > kHandshakeTimeoutMs) {
        close("poignée de main trop lente");
        return;
    }

    if (state_ == State::Ready && now_ms - last_activity_ms_ > kIdleTimeoutMs) {
        close("pair silencieux");
        return;
    }

    // Purge des requêtes jamais servies, en miroir de expire_requests() côté
    // picker. Les deux doivent oublier le bloc, sinon le pair reste crédité
    // d'un travail qu'il ne fera jamais et sort du jeu pour de bon.
    if (!requested_.empty()) {
        requested_.erase(std::remove_if(requested_.begin(), requested_.end(),
                                        [&](const OutstandingRequest& r) {
                                            return now_ms - r.sent_ms > kBlockTimeoutMs;
                                        }),
                         requested_.end());
    }

    if (state_ == State::Ready && now_ms - last_keepalive_ms_ > kKeepAliveMs) {
        uint8_t keepalive[4] = {0, 0, 0, 0};
        queue(keepalive, 4);
        flush();
        last_keepalive_ms_ = now_ms;
    }

    // Débit lissé sur la dernière seconde.
    if (last_rate_sample_ms_ == 0) {
        last_rate_sample_ms_ = now_ms;
        bytes_at_last_sample_ = bytes_down_;
    } else if (now_ms - last_rate_sample_ms_ >= 1000) {
        const uint64_t delta = bytes_down_ - bytes_at_last_sample_;
        const uint64_t span = now_ms - last_rate_sample_ms_;
        rate_down_ = static_cast<uint32_t>(delta * 1000 / span);
        last_rate_sample_ms_ = now_ms;
        bytes_at_last_sample_ = bytes_down_;
    }
}

void PeerConnection::set_piece_count(uint32_t count) {
    if (count == 0 || piece_count_ == count) return;
    piece_count_ = count;
    pieces_.resize(count);

    if (!pending_bitfield_.empty()) {
        // Le bitfield reçu avant les métadonnées peut être trop court (« have »
        // accumulés) : on complète à la bonne taille avant de le décoder.
        pending_bitfield_.resize((count + 7) / 8, 0);
        if (!pieces_.from_bytes(pending_bitfield_.data(), pending_bitfield_.size(), count)) {
            pieces_.resize(count);
        }
        pending_bitfield_.clear();
        pending_bitfield_.shrink_to_fit();
        handler_.on_peer_bitfield(*this);
    }
}

bool PeerConnection::has_piece(uint32_t index) const {
    return pieces_.get(index);
}

}  // namespace bt
