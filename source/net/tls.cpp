#include "net/tls.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cstdio>
#include <cstring>

#include "net/ca_bundle.hpp"
#include "util/bytes.hpp"
#include "util/clock.hpp"

namespace net {

namespace {

struct BioContext {
    Transport* transport = nullptr;
    TcpSocket* socket = nullptr;
};

// mbedtls réclame des rappels d'E/S ; on lui donne accès à la socket du
// Transport, en traduisant « réessayer plus tard » dans son vocabulaire.
int bio_send(void* ctx, const unsigned char* buf, size_t len) {
    auto* bio = static_cast<BioContext*>(ctx);
    const int n = bio->socket->send(buf, len);
    if (n > 0) return n;
    if (n == kWouldBlock) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int bio_recv(void* ctx, unsigned char* buf, size_t len) {
    auto* bio = static_cast<BioContext*>(ctx);
    const int n = bio->socket->recv(buf, len);
    if (n > 0) return n;
    if (n == kWouldBlock) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n == kClosed) return 0;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

bool wait_io(Transport& transport, TcpSocket& socket, bool for_write, int timeout_ms) {
    PollItem item;
    item.tcp = &socket;
    item.want_read = !for_write;
    item.want_write = for_write;
    const int rc = transport.poll(&item, 1, timeout_ms);
    if (rc < 0 || item.has_error) return false;
    return true;  // une expiration partielle n'est pas fatale : on retentera
}

int random_source(void* ctx, unsigned char* out, size_t len) {
    (void)ctx;
    util::random_bytes(out, len);
    return 0;
}

}  // namespace

bool https_request(Transport& transport, const std::string& url, const char* method,
                   const std::string& body, const std::string& bearer, HttpResponse& out,
                   std::string* err, int timeout_ms) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };

    Url parsed;
    if (!parse_url(url, parsed)) return fail("URL invalide");
    if (!parsed.secure()) return fail("https attendu");
    if (!transport.ready()) return fail("réseau indisponible");

    uint32_t ip = 0;
    if (!transport.resolve(parsed.host, ip)) return fail("résolution DNS impossible");

    auto socket = transport.tcp();
    if (!socket) return fail("socket indisponible");

    const Endpoint endpoint{ip, parsed.port};
    if (!connect_wait(transport, *socket, endpoint, timeout_ms)) {
        return fail("connexion à " + parsed.host + " impossible");
    }

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&ca);

    auto cleanup = [&]() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&ca);
    };

    if (mbedtls_x509_crt_parse(&ca, reinterpret_cast<const unsigned char*>(kCaBundlePem),
                               sizeof(kCaBundlePem)) != 0) {
        cleanup();
        return fail("racines de confiance illisibles");
    }

    if (mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        cleanup();
        return fail("configuration TLS impossible");
    }

    // Vérification stricte : un certificat invalide doit faire échouer l'appel,
    // pas déclencher un avertissement qu'on ignorerait.
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config, &ca, nullptr);
    mbedtls_ssl_conf_rng(&config, random_source, nullptr);

    if (mbedtls_ssl_setup(&ssl, &config) != 0) {
        cleanup();
        return fail("initialisation TLS impossible");
    }
    if (mbedtls_ssl_set_hostname(&ssl, parsed.host.c_str()) != 0) {
        cleanup();
        return fail("SNI impossible");
    }

    BioContext bio{&transport, socket.get()};
    mbedtls_ssl_set_bio(&ssl, &bio, bio_send, bio_recv, nullptr);

    const uint64_t deadline = util::now_ms() + static_cast<uint64_t>(timeout_ms);

    int rc;
    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "poignée de main TLS échouée (-0x%04x)",
                          static_cast<unsigned>(-rc));
            cleanup();
            return fail(buf);
        }
        if (util::now_ms() >= deadline) {
            cleanup();
            return fail("expiration pendant la poignée de main TLS");
        }
        wait_io(transport, *socket, rc == MBEDTLS_ERR_SSL_WANT_WRITE, 200);
    }

    if (mbedtls_ssl_get_verify_result(&ssl) != 0) {
        cleanup();
        return fail("certificat serveur refusé");
    }

    // --- requête ---
    std::string request = std::string(method) + " " + parsed.path;
    if (!parsed.query.empty()) request += "?" + parsed.query;
    request += " HTTP/1.1\r\nHost: " + parsed.host + "\r\n";
    request += "User-Agent: Torfoil/0.1\r\nAccept: application/json\r\n";
    if (!bearer.empty()) request += "Authorization: Bearer " + bearer + "\r\n";
    if (!body.empty()) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    request += body;

    size_t written = 0;
    while (written < request.size()) {
        rc = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(request.data()) + written,
                               request.size() - written);
        if (rc > 0) {
            written += static_cast<size_t>(rc);
            continue;
        }
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            cleanup();
            return fail("envoi TLS impossible");
        }
        if (util::now_ms() >= deadline) {
            cleanup();
            return fail("expiration à l'envoi");
        }
        wait_io(transport, *socket, rc == MBEDTLS_ERR_SSL_WANT_WRITE, 200);
    }

    // --- réponse ---
    std::string raw;
    unsigned char chunk[4096];
    while (raw.size() < 8 * 1024 * 1024) {
        rc = mbedtls_ssl_read(&ssl, chunk, sizeof(chunk));
        if (rc > 0) {
            raw.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(rc));
            continue;
        }
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (util::now_ms() >= deadline) {
            cleanup();
            return fail("expiration à la lecture");
        }
        wait_io(transport, *socket, rc == MBEDTLS_ERR_SSL_WANT_WRITE, 200);
    }

    mbedtls_ssl_close_notify(&ssl);
    cleanup();

    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return fail("réponse HTTPS malformée");

    const std::string head = raw.substr(0, header_end);
    out.body = raw.substr(header_end + 4);

    const size_t space = head.find(' ');
    if (space == std::string::npos) return fail("statut illisible");
    out.status = std::atoi(head.c_str() + space + 1);

    // L'API Mullvad répond en HTTP/1.1 avec « Connection: close » : le corps va
    // jusqu'au FIN, pas de découpage en morceaux à recomposer.
    return true;
}

}  // namespace net
