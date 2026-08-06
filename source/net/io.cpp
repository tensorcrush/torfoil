#include "net/io.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "util/bytes.hpp"
#include "util/clock.hpp"

namespace net {

namespace {

// Attend qu'une socket TCP soit lisible/écrivable. Renvoie false sur expiration.
bool wait_socket(Transport& t, TcpSocket& sock, bool for_write, int timeout_ms) {
    PollItem item;
    item.tcp = &sock;
    item.want_read = !for_write;
    item.want_write = for_write;

    const int rc = t.poll(&item, 1, timeout_ms);
    if (rc <= 0) return false;
    if (item.has_error) return false;
    return for_write ? item.can_write : item.can_read;
}

int remaining_ms(uint64_t deadline) {
    const uint64_t now = util::now_ms();
    if (now >= deadline) return 0;
    const uint64_t left = deadline - now;
    return left > 60000 ? 60000 : static_cast<int>(left);
}

}  // namespace

bool parse_url(const std::string& raw, Url& out) {
    const size_t scheme_end = raw.find("://");
    if (scheme_end == std::string::npos) return false;

    out.scheme = raw.substr(0, scheme_end);
    for (char& c : out.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    size_t cursor = scheme_end + 3;
    const size_t path_start = raw.find('/', cursor);
    std::string authority =
        path_start == std::string::npos ? raw.substr(cursor) : raw.substr(cursor, path_start - cursor);

    if (path_start != std::string::npos) {
        const std::string rest = raw.substr(path_start);
        const size_t q = rest.find('?');
        if (q == std::string::npos) {
            out.path = rest;
        } else {
            out.path = rest.substr(0, q);
            out.query = rest.substr(q + 1);
        }
    } else {
        out.path = "/";
    }

    // On ignore un éventuel « user:pass@ » : aucun tracker sérieux n'en met.
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = static_cast<uint16_t>(std::atoi(authority.c_str() + colon + 1));
    } else {
        out.host = authority;
        out.port = 0;
    }

    if (out.port == 0) {
        if (out.scheme == "http") out.port = 80;
        else if (out.scheme == "https") out.port = 443;
        else return false;  // udp:// sans port explicite n'a pas de valeur par défaut
    }

    return !out.host.empty();
}

bool connect_wait(Transport& t, TcpSocket& sock, const Endpoint& ep, int timeout_ms) {
    if (!t.ready()) return false;
    if (!sock.start_connect(ep)) return false;
    if (!wait_socket(t, sock, /*for_write=*/true, timeout_ms)) {
        sock.close();
        return false;
    }
    if (!sock.finish_connect()) {
        sock.close();
        return false;
    }
    return true;
}

bool send_all(Transport& t, TcpSocket& sock, const uint8_t* data, size_t len, int timeout_ms) {
    const uint64_t deadline = util::now_ms() + static_cast<uint64_t>(timeout_ms);
    size_t sent = 0;

    while (sent < len) {
        const int n = sock.send(data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n == kWouldBlock) {
            const int left = remaining_ms(deadline);
            if (left == 0 || !wait_socket(t, sock, /*for_write=*/true, left)) return false;
            continue;
        }
        return false;
    }
    return true;
}

int recv_wait(Transport& t, TcpSocket& sock, uint8_t* data, size_t len, int timeout_ms) {
    const uint64_t deadline = util::now_ms() + static_cast<uint64_t>(timeout_ms);

    while (true) {
        const int n = sock.recv(data, len);
        if (n > 0) return n;
        if (n == kClosed) return 0;
        if (n == kWouldBlock) {
            const int left = remaining_ms(deadline);
            if (left == 0 || !wait_socket(t, sock, /*for_write=*/false, left)) return -1;
            continue;
        }
        return -1;
    }
}

bool recv_until_close(Transport& t, TcpSocket& sock, std::string& out, size_t max_bytes,
                      int timeout_ms) {
    const uint64_t deadline = util::now_ms() + static_cast<uint64_t>(timeout_ms);
    uint8_t chunk[4096];

    while (out.size() < max_bytes) {
        const int left = remaining_ms(deadline);
        if (left == 0) return false;

        const int n = recv_wait(t, sock, chunk, sizeof(chunk), left);
        if (n == 0) return true;   // fermeture propre
        if (n < 0) return false;
        out.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
    }
    return true;
}

bool http_get(Transport& t, const std::string& url, HttpResponse& out, std::string* err,
              int timeout_ms) {
    auto fail = [&](const char* msg) {
        if (err) *err = msg;
        return false;
    };

    Url parsed;
    if (!parse_url(url, parsed)) return fail("URL invalide");
    if (parsed.secure()) return fail("HTTPS pas encore branché (arrive avec la couche TLS)");
    if (parsed.scheme != "http") return fail("schéma non géré");
    if (!t.ready()) return fail("aucun transport réseau disponible");

    uint32_t ip = 0;
    if (!t.resolve(parsed.host, ip)) return fail("résolution DNS impossible");

    auto sock = t.tcp();
    if (!sock) return fail("création de socket impossible");

    const Endpoint ep{ip, parsed.port};
    if (!connect_wait(t, *sock, ep, timeout_ms)) return fail("connexion refusée ou expirée");

    std::string request = "GET " + parsed.path;
    if (!parsed.query.empty()) request += "?" + parsed.query;
    request +=
        " HTTP/1.1\r\n"
        "Host: " + parsed.host + "\r\n"
        "User-Agent: Torfoil/0.1\r\n"
        "Accept: */*\r\n"
        // Fermeture immédiate : on lit jusqu'au FIN, pas de découpage en chunks
        // à gérer.
        "Connection: close\r\n\r\n";

    if (!send_all(t, *sock, reinterpret_cast<const uint8_t*>(request.data()), request.size(),
                  timeout_ms)) {
        return fail("envoi de la requête impossible");
    }

    std::string raw;
    if (!recv_until_close(t, *sock, raw, 4 * 1024 * 1024, timeout_ms)) {
        return fail("réponse incomplète ou expirée");
    }

    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return fail("réponse HTTP malformée");

    const std::string head = raw.substr(0, header_end);
    out.body = raw.substr(header_end + 4);

    // « HTTP/1.1 200 OK »
    const size_t sp = head.find(' ');
    if (sp == std::string::npos) return fail("statut HTTP illisible");
    out.status = std::atoi(head.c_str() + sp + 1);

    const size_t ct = head.find("Content-Type:");
    if (ct != std::string::npos) {
        const size_t eol = head.find("\r\n", ct);
        out.content_type = util::trim(head.substr(ct + 13, eol - ct - 13));
    }

    return true;
}

}  // namespace net
