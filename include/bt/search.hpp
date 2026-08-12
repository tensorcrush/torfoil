// Recherche de torrents, avec des sources déclarées par l'utilisateur.
//
// Aucune adresse n'est livrée avec l'application : le fichier de sources est
// vide au premier lancement, et c'est à celui qui s'en sert d'y mettre ce qu'il
// veut interroger. Deux formats sont compris :
//
//   * torznab — le protocole des indexeurs (Jackett, Prowlarr). C'est le cas
//     courant : une seule adresse donne accès à tout ce que l'utilisateur a
//     configuré chez lui.
//   * json — n'importe quelle API qui rend une liste, avec les noms de champs
//     indiqués dans la configuration.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/transport.hpp"

namespace bt {

struct SearchResult {
    std::string name;
    std::string magnet;
    uint64_t size = 0;
    uint32_t seeders = 0;
    uint32_t leechers = 0;
    std::string source;
};

struct SearchProvider {
    std::string name;
    std::string kind = "torznab";  // « torznab » ou « json »
    std::string url;
    std::string api_key;
    bool enabled = true;

    // Correspondance des champs, pour le format json uniquement.
    std::string list_key = "results";
    std::string name_key = "name";
    std::string size_key = "size";
    std::string seeders_key = "seeders";
    std::string magnet_key = "magnet";
    std::string hash_key = "info_hash";
};

// Lit le fichier de sources. Absent, il est créé avec un exemple désactivé et
// la liste revient vide — ce n'est pas une erreur.
bool load_providers(const std::string& path, std::vector<SearchProvider>& out, std::string* err);
bool write_example_providers(const std::string& path);

// Analyseurs, séparés du réseau pour être éprouvés sans lui.
bool parse_torznab(const std::string& xml, const std::string& source,
                   std::vector<SearchResult>& out);
bool parse_json_results(const std::string& body, const SearchProvider& provider,
                        std::vector<SearchResult>& out);

// Construit l'adresse à interroger pour une requête donnée.
std::string search_url(const SearchProvider& provider, const std::string& query);

// Interroge chaque source active et rassemble les réponses. Bloquant.
bool run_search(net::Transport& transport, const std::vector<SearchProvider>& providers,
                const std::string& query, std::vector<SearchResult>& out, std::string* err);

}  // namespace bt
