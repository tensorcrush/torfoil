// La moitié réseau de la recherche, séparée pour que les analyseurs restent
// éprouvables sans pile TCP ni certificats.
#include <algorithm>
#include <string>
#include <vector>

#include "bt/search.hpp"
#include "net/io.hpp"
#include "util/log.hpp"

namespace bt {

bool run_search(net::Transport& transport, const std::vector<SearchProvider>& providers,
                const std::string& query, std::vector<SearchResult>& out, std::string* err) {
    out.clear();
    int queried = 0;
    std::string last_error;

    for (const SearchProvider& p : providers) {
        if (!p.enabled || p.url.empty()) continue;
        ++queried;

        net::HttpResponse response;
        std::string request_error;
        const std::string url = search_url(p, query);
        if (!net::http_get(transport, url, response, &request_error)) {
            last_error = p.name + " : " + request_error;
            util::log_line("recherche : " + last_error);
            continue;
        }
        if (response.status != 200) {
            last_error = p.name + " : HTTP " + std::to_string(response.status);
            continue;
        }

        const size_t before = out.size();
        if (p.kind == "json") parse_json_results(response.body, p, out);
        else parse_torznab(response.body, p.name, out);
        util::log_fmt("recherche « %s » chez %s : %u résultat(s)", query.c_str(), p.name.c_str(),
                      static_cast<unsigned>(out.size() - before));
    }

    // Les plus partagés d'abord : c'est le seul critère qui prédit un peu le
    // temps de téléchargement.
    std::sort(out.begin(), out.end(),
              [](const SearchResult& a, const SearchResult& b) { return a.seeders > b.seeders; });

    if (queried == 0) {
        if (err) *err = "aucune source active dans search.json";
        return false;
    }
    if (out.empty() && !last_error.empty()) {
        if (err) *err = last_error;
        return false;
    }
    return true;
}

}  // namespace bt
