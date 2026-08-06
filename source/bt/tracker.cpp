#include "bt/tracker.hpp"

#include <cstring>

#include "bt/bencode.hpp"
#include "net/io.hpp"
#include "net/tls.hpp"
#include "util/clock.hpp"

namespace bt {

namespace {

const char* event_name(AnnounceEvent e) {
    switch (e) {
        case AnnounceEvent::Started: return "started";
        case AnnounceEvent::Completed: return "completed";
        case AnnounceEvent::Stopped: return "stopped";
        default: return "";
    }
}

uint32_t event_code(AnnounceEvent e) {
    switch (e) {
        case AnnounceEvent::Completed: return 1;
        case AnnounceEvent::Started: return 2;
        case AnnounceEvent::Stopped: return 3;
        default: return 0;
    }
}

AnnounceResult error(const std::string& msg) {
    AnnounceResult r;
    r.ok = false;
    r.failure = msg;
    return r;
}

// Format compact BEP 23 : 6 octets par pair (4 IP + 2 port), en big-endian.
void parse_compact_peers(const std::string& blob, std::vector<net::Endpoint>& out) {
    for (size_t i = 0; i + 6 <= blob.size(); i += 6) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(blob.data()) + i;
        net::Endpoint ep;
        ep.ipv4 = util::rd_be32(p);
        ep.port = util::rd_be16(p + 4);
        if (ep.valid()) out.push_back(ep);
    }
}

}  // namespace

std::array<uint8_t, 20> make_peer_id() {
    std::array<uint8_t, 20> id{};
    const char prefix[] = "-TF0100-";
    std::memcpy(id.data(), prefix, 8);
    util::random_bytes(id.data() + 8, 12);
    // On garde des octets imprimables : certains trackers anciens s'étranglent
    // sur des binaires bruts dans le peer_id.
    for (size_t i = 8; i < 20; ++i) {
        id[i] = static_cast<uint8_t>('0' + (id[i] % 36));
        if (id[i] > '9') id[i] = static_cast<uint8_t>('a' + (id[i] - '9' - 1));
    }
    return id;
}

AnnounceResult TrackerClient::announce(const std::string& url, const AnnounceRequest& req,
                                       int timeout_ms) {
    if (!transport_.ready()) return error("réseau indisponible (VPN coupé ?)");

    if (url.rfind("udp://", 0) == 0) return announce_udp(url, req, timeout_ms);
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        return announce_http(url, req, timeout_ms);
    }
    return error("schéma de tracker non géré : " + url.substr(0, 12));
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

AnnounceResult TrackerClient::announce_http(const std::string& url, const AnnounceRequest& req,
                                            int timeout_ms) {
    std::string full = url;
    full += (url.find('?') == std::string::npos) ? '?' : '&';
    full += "info_hash=" + util::url_encode(req.info_hash.data(), req.info_hash.size());
    full += "&peer_id=" + util::url_encode(req.peer_id.data(), req.peer_id.size());
    full += "&port=" + std::to_string(req.port);
    full += "&uploaded=" + std::to_string(req.uploaded);
    full += "&downloaded=" + std::to_string(req.downloaded);
    full += "&left=" + std::to_string(req.left);
    full += "&numwant=" + std::to_string(req.num_want);
    full += "&key=" + std::to_string(req.key);
    full += "&compact=1&no_peer_id=1";

    const char* ev = event_name(req.event);
    if (ev[0] != '\0') full += std::string("&event=") + ev;

    // De plus en plus de trackers n'écoutent qu'en HTTPS ; http_get refuse ce
    // schéma par construction, il faut passer par la couche TLS.
    net::HttpResponse http;
    std::string err;
    const bool secure = url.rfind("https://", 0) == 0;
    const bool sent = secure
                          ? net::https_request(transport_, full, "GET", "", "", http, &err,
                                               timeout_ms)
                          : net::http_get(transport_, full, http, &err, timeout_ms);
    if (!sent) return error(err);
    if (http.status != 200) return error("tracker HTTP " + std::to_string(http.status));

    Value root;
    std::string decode_err;
    if (!bdecode(http.body, root, &decode_err)) return error("réponse illisible : " + decode_err);

    if (const Value* failure = root.find_str("failure reason")) return error(failure->s);

    AnnounceResult result;
    result.ok = true;
    result.interval_s = static_cast<uint32_t>(root.int_or("interval", 1800));
    result.seeders = static_cast<uint32_t>(root.int_or("complete", 0));
    result.leechers = static_cast<uint32_t>(root.int_or("incomplete", 0));

    if (const Value* peers = root.find("peers")) {
        if (peers->is_str()) {
            parse_compact_peers(peers->s, result.peers);
        } else if (peers->is_list()) {
            // Ancien format : liste de dictionnaires {ip, port}.
            for (const Value& p : peers->l) {
                const Value* ip = p.find_str("ip");
                const Value* port = p.find_int("port");
                if (!ip || !port) continue;

                uint32_t addr = 0;
                if (!transport_.resolve(ip->s, addr)) continue;
                net::Endpoint ep{addr, static_cast<uint16_t>(port->i)};
                if (ep.valid()) result.peers.push_back(ep);
            }
        }
    }

    if (result.interval_s < 60) result.interval_s = 60;
    return result;
}

// ---------------------------------------------------------------------------
// UDP (BEP 15)
// ---------------------------------------------------------------------------

AnnounceResult TrackerClient::announce_udp(const std::string& url, const AnnounceRequest& req,
                                           int timeout_ms) {
    net::Url parsed;
    if (!net::parse_url(url, parsed) || parsed.port == 0) return error("URL UDP invalide");

    uint32_t ip = 0;
    if (!transport_.resolve(parsed.host, ip)) return error("résolution DNS impossible");
    const net::Endpoint tracker{ip, parsed.port};

    auto sock = transport_.udp();
    if (!sock || !sock->open()) return error("socket UDP indisponible");

    uint32_t tid = 0;
    util::random_bytes(reinterpret_cast<uint8_t*>(&tid), sizeof(tid));

    // Attend un datagramme de ce tracker portant le bon identifiant de
    // transaction. Les autres sont ignorés (usurpation ou paquet en retard).
    auto await_reply = [&](uint8_t* buf, size_t cap, uint32_t expect_action,
                           int wait_ms) -> int {
        const uint64_t deadline = util::now_ms() + static_cast<uint64_t>(wait_ms);
        while (true) {
            const uint64_t now = util::now_ms();
            if (now >= deadline) return -1;

            net::PollItem item;
            item.udp = sock.get();
            item.want_read = true;
            const int rc = transport_.poll(&item, 1, static_cast<int>(deadline - now));
            if (rc <= 0 || !item.can_read) return -1;

            net::Endpoint from;
            const int n = sock->recv_from(from, buf, cap);
            if (n < 16) continue;
            if (from.ipv4 != tracker.ipv4) continue;
            if (util::rd_be32(buf) != expect_action) continue;
            if (util::rd_be32(buf + 4) != tid) continue;
            return n;
        }
    };

    // --- étape 1 : connect ---
    uint8_t connect_req[16];
    util::wr_be64(connect_req, 0x41727101980ULL);  // magic protocole BEP 15
    util::wr_be32(connect_req + 8, 0);             // action = connect
    util::wr_be32(connect_req + 12, tid);

    uint8_t reply[2048];
    uint64_t connection_id = 0;
    bool connected = false;

    // BEP 15 recommande 15 s puis doublement ; on reste plus court, un tracker
    // muet ne mérite pas d'immobiliser la session.
    for (int attempt = 0; attempt < 2 && !connected; ++attempt) {
        if (sock->send_to(tracker, connect_req, sizeof(connect_req)) < 0) {
            return error("envoi UDP impossible");
        }
        const int n = await_reply(reply, sizeof(reply), 0, timeout_ms / 2);
        if (n >= 16) {
            connection_id = util::rd_be64(reply + 8);
            connected = true;
        }
    }
    if (!connected) return error("tracker UDP muet");

    // --- étape 2 : announce ---
    uint8_t ann[98];
    std::memset(ann, 0, sizeof(ann));
    util::wr_be64(ann, connection_id);
    util::wr_be32(ann + 8, 1);  // action = announce
    util::wr_be32(ann + 12, tid);
    std::memcpy(ann + 16, req.info_hash.data(), 20);
    std::memcpy(ann + 36, req.peer_id.data(), 20);
    util::wr_be64(ann + 56, req.downloaded);
    util::wr_be64(ann + 64, req.left);
    util::wr_be64(ann + 72, req.uploaded);
    util::wr_be32(ann + 80, event_code(req.event));
    util::wr_be32(ann + 84, 0);  // IP : 0 = « celle d'où vient le paquet »
    util::wr_be32(ann + 88, req.key);
    util::wr_be32(ann + 92, req.num_want);
    util::wr_be16(ann + 96, req.port);

    if (sock->send_to(tracker, ann, sizeof(ann)) < 0) return error("envoi UDP impossible");

    const int n = await_reply(reply, sizeof(reply), 1, timeout_ms);
    if (n < 20) return error("réponse UDP tronquée");

    AnnounceResult result;
    result.ok = true;
    result.interval_s = util::rd_be32(reply + 8);
    result.leechers = util::rd_be32(reply + 12);
    result.seeders = util::rd_be32(reply + 16);

    parse_compact_peers(std::string(reinterpret_cast<const char*>(reply + 20),
                                    static_cast<size_t>(n - 20)),
                        result.peers);

    if (result.interval_s < 60) result.interval_s = 60;
    return result;
}

}  // namespace bt
