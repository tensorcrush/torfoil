// Connexion à un pair : « peer wire protocol » (BEP 3), messagerie étendue
// (BEP 10) et récupération des métadonnées ut_metadata (BEP 9).
//
// Entièrement non bloquant : la session appelle fill_poll() puis on_poll().
// Aucun thread par pair — une quarantaine de connexions sur un seul poll.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bt/piece_picker.hpp"
#include "net/transport.hpp"
#include "util/bytes.hpp"

namespace bt {

class PeerConnection;

class PeerHandler {
public:
    virtual ~PeerHandler() = default;

    virtual void on_peer_ready(PeerConnection&) {}
    virtual void on_peer_bitfield(PeerConnection&) {}
    virtual void on_peer_have(PeerConnection&, uint32_t piece) {}
    virtual void on_peer_choke(PeerConnection&, bool choked) {}
    virtual void on_peer_block(PeerConnection&, uint32_t piece, uint32_t offset,
                               const uint8_t* data, uint32_t len) {}
    virtual void on_peer_metadata_size(PeerConnection&, uint32_t total_size) {}
    virtual void on_peer_metadata_piece(PeerConnection&, uint32_t index, const uint8_t* data,
                                        uint32_t len) {}
    virtual void on_peer_request(PeerConnection&, uint32_t piece, uint32_t offset,
                                 uint32_t len) {}
    // Adresses reçues d'un pair via ut_pex (BEP 11).
    virtual void on_peers_discovered(PeerConnection&, const std::vector<net::Endpoint>&) {}
    virtual void on_peer_closed(PeerConnection&, const char* reason) {}
};

class PeerConnection {
public:
    enum class State { Connecting, Handshaking, Ready, Closed };

    // Taille d'une pièce de métadonnées, fixée par BEP 9.
    static constexpr uint32_t kMetadataPieceSize = 16 * 1024;

    // Identifiants étendus que nous annonçons (BEP 10) : ce sont ceux que les
    // pairs emploieront pour nous écrire.
    static constexpr uint8_t kExtMetadata = 1;
    static constexpr uint8_t kExtPex = 2;

    // `advertise_pex` : annoncer ou non ut_pex dans la poignée de main étendue.
    // Se contenter d'ignorer les messages reçus ne suffirait pas — annoncer la
    // prise en charge fait déjà savoir au pair qu'on participe à l'échange.
    PeerConnection(net::Transport& transport, PeerHandler& handler, const net::Endpoint& endpoint,
                   const util::Hash160& info_hash, const std::array<uint8_t, 20>& my_peer_id,
                   bool advertise_pex = true);
    ~PeerConnection();

    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    bool start();
    void close(const char* reason);

    void fill_poll(net::PollItem& item);
    void on_poll(const net::PollItem& item, uint64_t now_ms);
    void tick(uint64_t now_ms);

    State state() const { return state_; }
    bool closed() const { return state_ == State::Closed; }
    const net::Endpoint& endpoint() const { return endpoint_; }

    // Le nombre de pièces n'est connu qu'une fois les métadonnées obtenues :
    // en mode magnet, le bitfield arrive avant. On le conserve brut et on le
    // décode ici, après coup.
    void set_piece_count(uint32_t count);
    const Bitfield& pieces() const { return pieces_; }
    bool has_piece(uint32_t index) const;

    bool peer_choking() const { return peer_choking_; }
    // Depuis quand ce pair refuse de nous servir (0 s'il ne nous étrangle pas).
    uint64_t choking_since_ms() const { return choked_since_ms_; }
    bool am_interested() const { return am_interested_; }
    uint32_t in_flight() const { return static_cast<uint32_t>(requested_.size()); }

    // Blocs demandés et non encore reçus. Sert à les rendre au picker quand la
    // connexion se ferme ou que le pair nous étrangle.
    std::vector<BlockRequest> outstanding() const;

    void send_interested(bool yes);
    void send_request(const BlockRequest& req);
    void send_cancel(const BlockRequest& req);
    void send_have(uint32_t piece);
    void send_bitfield(const Bitfield& bits);
    void send_choke(bool choked);
    void send_block(uint32_t piece, uint32_t offset, const uint8_t* data, uint32_t len);

    bool supports_metadata() const { return ut_metadata_id_ != 0; }
    uint32_t metadata_size() const { return metadata_size_; }
    void request_metadata_piece(uint32_t index);

    uint64_t bytes_downloaded() const { return bytes_down_; }
    uint64_t bytes_uploaded() const { return bytes_up_; }
    // Débit lissé en octets/s, recalculé par tick().
    uint32_t rate_down() const { return rate_down_; }

    const std::string& client_name() const { return client_name_; }

private:
    enum MsgId : uint8_t {
        kChoke = 0,
        kUnchoke = 1,
        kInterested = 2,
        kNotInterested = 3,
        kHave = 4,
        kBitfield = 5,
        kRequest = 6,
        kPiece = 7,
        kCancel = 8,
        kPort = 9,
        kExtended = 20,
    };

    void send_handshake();
    void send_extended_handshake();
    void queue(const uint8_t* data, size_t len);
    void queue_message(uint8_t id, const uint8_t* payload, size_t len);
    void flush();

    bool consume_handshake();
    bool consume_messages(uint64_t now_ms);
    void handle_message(const uint8_t* data, uint32_t len, uint64_t now_ms);
    void handle_extended(const uint8_t* data, uint32_t len);

    void compact_in();

    net::Transport& transport_;
    PeerHandler& handler_;
    net::Endpoint endpoint_;
    util::Hash160 info_hash_;
    std::array<uint8_t, 20> my_peer_id_;
    std::array<uint8_t, 20> their_peer_id_{};

    std::unique_ptr<net::TcpSocket> sock_;
    State state_ = State::Connecting;

    std::vector<uint8_t> in_;
    size_t in_pos_ = 0;
    std::vector<uint8_t> out_;
    size_t out_pos_ = 0;

    Bitfield pieces_;
    std::vector<uint8_t> pending_bitfield_;  // reçu avant de connaître le total
    uint32_t piece_count_ = 0;

    bool peer_choking_ = true;
    uint64_t choked_since_ms_ = 0;
    bool am_interested_ = false;
    bool am_choking_ = true;
    bool peer_interested_ = false;

    uint8_t ut_metadata_id_ = 0;  // identifiant étendu annoncé par le pair
    uint8_t ut_pex_id_ = 0;
    bool advertise_pex_ = true;
    uint32_t metadata_size_ = 0;
    std::string client_name_;

    // Chaque requête porte l'instant où elle est partie. Sans cette date, un
    // bloc jamais servi resterait compté « en vol » pour toujours : le pair
    // paraîtrait saturé et on ne lui redemanderait plus rien, alors même que le
    // picker a depuis longtemps confié le bloc à quelqu'un d'autre.
    struct OutstandingRequest {
        BlockRequest req;
        uint64_t sent_ms = 0;
    };
    std::vector<OutstandingRequest> requested_;

    uint64_t bytes_down_ = 0;
    uint64_t bytes_up_ = 0;
    uint64_t last_rate_sample_ms_ = 0;
    uint64_t bytes_at_last_sample_ = 0;
    uint32_t rate_down_ = 0;

    uint64_t connected_at_ms_ = 0;
    uint64_t last_activity_ms_ = 0;
    uint64_t last_keepalive_ms_ = 0;
};

}  // namespace bt
