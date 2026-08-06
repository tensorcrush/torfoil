// Client HTTPS minimal (mbedtls) posé sur net::Transport.
//
// Passer par Transport plutôt que par les sockets système a une conséquence
// utile : quand le VPN est actif, les appels à l'API Mullvad partent eux aussi
// dans le tunnel.
#pragma once

#include <string>

#include "net/io.hpp"
#include "net/transport.hpp"

namespace net {

// `method` : "GET" ou "POST". `bearer` peut être vide.
bool https_request(Transport& transport, const std::string& url, const char* method,
                   const std::string& body, const std::string& bearer, HttpResponse& out,
                   std::string* err, int timeout_ms = 25000);

}  // namespace net
