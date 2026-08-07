// Analyse des requêtes HTTP entrantes — la moitié du petit serveur local qui
// ne touche pas aux sockets.
//
// Séparée exprès : c'est la partie où se cachent les erreurs (frontières
// multipart, en-têtes tronqués, corps qui n'arrive pas d'un bloc), et c'est
// aussi la seule qu'on puisse éprouver sur PC. Le fichier voisin, http_server,
// ne fait plus qu'accepter des connexions et appeler ceci.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace net {

struct HttpRequest {
    std::string method;
    std::string target;  // brut, tel que reçu (chemin + query)
    std::string path;    // chemin seul, pourcent-décodé
    std::string query;   // après le « ? », brut
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // Recherche insensible à la casse, comme l'exige la norme.
    std::string header(const std::string& name) const;
    std::string content_type() const { return header("content-type"); }
    // -1 si l'en-tête est absent ou illisible.
    long long content_length() const;
};

// Analyse la ligne de requête et les en-têtes de `raw`.
// `body_offset` reçoit la position du premier octet du corps.
// Renvoie false si les en-têtes ne sont pas encore complets OU sont malformés ;
// `complete` distingue les deux — sans quoi le serveur fermerait la connexion
// d'un client qui a simplement envoyé ses en-têtes en deux paquets.
bool parse_http_headers(const std::string& raw, HttpRequest& out, size_t* body_offset,
                        bool* complete);

// Valeur d'un champ dans un corps ou une query « a=1&b=2 », pourcent-décodée.
std::string form_field(const std::string& encoded, const std::string& name);

struct MultipartPart {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string data;
};

// Découpe un corps multipart/form-data ; la frontière est lue dans `content_type`.
bool parse_multipart(const std::string& content_type, const std::string& body,
                     std::vector<MultipartPart>& out);

// Échappements de sortie.
std::string json_escape(const std::string& s);
std::string html_escape(const std::string& s);

}  // namespace net
