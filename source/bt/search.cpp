#include "bt/search.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "net/json.hpp"
#include "util/log.hpp"

namespace bt {

namespace {

std::string url_encode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0f];
        }
    }
    return out;
}

// Décodage des entités que produisent les indexeurs. Cinq suffisent : le reste
// n'apparaît pas dans un titre de torrent.
std::string xml_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; continue; }
            if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 4; continue; }
            if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 4; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; continue; }
            if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; continue; }
        }
        out += s[i++];
    }
    return out;
}

// Contenu du premier <tag>…</tag> trouvé à partir de `from`.
std::string tag_text(const std::string& xml, const std::string& tag, size_t from, size_t until) {
    const std::string open = "<" + tag;
    size_t start = xml.find(open, from);
    if (start == std::string::npos || start >= until) return {};
    start = xml.find('>', start);
    if (start == std::string::npos) return {};
    const size_t end = xml.find("</" + tag + ">", start);
    if (end == std::string::npos || end > until) return {};
    return xml_unescape(xml.substr(start + 1, end - start - 1));
}

// Valeur d'un attribut torznab : <torznab:attr name="seeders" value="12" />
std::string attr_value(const std::string& xml, const std::string& name, size_t from, size_t until) {
    const std::string needle = "name=\"" + name + "\"";
    size_t pos = xml.find(needle, from);
    if (pos == std::string::npos || pos >= until) return {};
    pos = xml.find("value=\"", pos);
    if (pos == std::string::npos || pos >= until) return {};
    pos += 7;
    const size_t end = xml.find('"', pos);
    if (end == std::string::npos) return {};
    return xml.substr(pos, end - pos);
}

std::string attribute(const std::string& xml, const std::string& tag, const std::string& attr,
                      size_t from, size_t until) {
    size_t pos = xml.find("<" + tag, from);
    if (pos == std::string::npos || pos >= until) return {};
    pos = xml.find(attr + "=\"", pos);
    if (pos == std::string::npos || pos >= until) return {};
    pos += attr.size() + 2;
    const size_t end = xml.find('"', pos);
    if (end == std::string::npos) return {};
    return xml_unescape(xml.substr(pos, end - pos));
}

uint64_t to_u64(const std::string& s) {
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}

std::string magnet_from_hash(const std::string& hash, const std::string& name) {
    if (hash.size() != 40) return {};
    return "magnet:?xt=urn:btih:" + hash + (name.empty() ? "" : "&dn=" + url_encode(name));
}

}  // namespace

std::string search_url(const SearchProvider& provider, const std::string& query) {
    if (provider.kind == "torznab") {
        std::string url = provider.url;
        url += url.find('?') == std::string::npos ? "?" : "&";
        url += "t=search";
        if (!provider.api_key.empty()) url += "&apikey=" + url_encode(provider.api_key);
        url += "&q=" + url_encode(query);
        return url;
    }

    std::string url = provider.url;
    const size_t slot = url.find("{q}");
    if (slot != std::string::npos) url.replace(slot, 3, url_encode(query));
    else url += (url.find('?') == std::string::npos ? "?q=" : "&q=") + url_encode(query);
    return url;
}

bool parse_torznab(const std::string& xml, const std::string& source,
                   std::vector<SearchResult>& out) {
    size_t pos = 0;
    while (true) {
        const size_t start = xml.find("<item", pos);
        if (start == std::string::npos) break;
        const size_t end = xml.find("</item>", start);
        if (end == std::string::npos) break;

        SearchResult r;
        r.source = source;
        r.name = tag_text(xml, "title", start, end);
        r.size = to_u64(tag_text(xml, "size", start, end));
        if (r.size == 0) r.size = to_u64(attr_value(xml, "size", start, end));
        r.seeders = static_cast<uint32_t>(to_u64(attr_value(xml, "seeders", start, end)));
        r.leechers = static_cast<uint32_t>(to_u64(attr_value(xml, "peers", start, end)));
        if (r.leechers >= r.seeders) r.leechers -= r.seeders;

        // Le lien peut se trouver à trois endroits selon l'indexeur.
        std::string link = attribute(xml, "enclosure", "url", start, end);
        if (link.compare(0, 7, "magnet:") != 0) {
            const std::string direct = attr_value(xml, "magneturl", start, end);
            if (!direct.empty()) link = direct;
        }
        if (link.compare(0, 7, "magnet:") != 0) {
            const std::string guid = tag_text(xml, "guid", start, end);
            if (guid.compare(0, 7, "magnet:") == 0) link = guid;
        }
        if (link.compare(0, 7, "magnet:") != 0) {
            link = magnet_from_hash(attr_value(xml, "infohash", start, end), r.name);
        }
        r.magnet = link.compare(0, 7, "magnet:") == 0 ? link : std::string();

        if (!r.name.empty() && !r.magnet.empty()) out.push_back(r);
        pos = end + 7;
    }
    return true;
}

bool parse_json_results(const std::string& body, const SearchProvider& provider,
                        std::vector<SearchResult>& out) {
    json::Value root;
    if (!json::parse(body, root, nullptr)) return false;

    const json::Value* list = &root;
    if (root.is_object()) {
        list = root.find(provider.list_key);
        if (!list) return false;
    }
    if (!list->is_array()) return false;

    for (const json::Value& item : list->array) {
        if (!item.is_object()) continue;
        SearchResult r;
        r.source = provider.name;
        r.name = item.string_or(provider.name_key);
        r.size = static_cast<uint64_t>(item.number_or(provider.size_key));
        if (r.size == 0) r.size = to_u64(item.string_or(provider.size_key));
        r.seeders = static_cast<uint32_t>(item.number_or(provider.seeders_key));
        if (r.seeders == 0) r.seeders = static_cast<uint32_t>(to_u64(item.string_or(provider.seeders_key)));
        r.magnet = item.string_or(provider.magnet_key);
        if (r.magnet.empty()) r.magnet = magnet_from_hash(item.string_or(provider.hash_key), r.name);
        if (!r.name.empty() && !r.magnet.empty()) out.push_back(r);
    }
    return true;
}

bool write_example_providers(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fprintf(fp, "%s",
                 R"({
  "_lisezmoi": "Aucune source n'est fournie. Ajoutez les vôtres ici, puis mettez enabled à true. Pour Jackett ou Prowlarr, kind vaut torznab et url est l'adresse torznab de l'indexeur.",
  "providers": [
    {
      "name": "Mon indexeur",
      "kind": "torznab",
      "url": "http://192.168.1.10:9117/api/v2.0/indexers/all/results/torznab/api",
      "api_key": "",
      "enabled": false
    }
  ]
}
)");
    std::fclose(fp);
    return true;
}

bool load_providers(const std::string& path, std::vector<SearchProvider>& out, std::string* err) {
    out.clear();

    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        write_example_providers(path);
        return true;
    }
    std::string text;
    char buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), fp)) > 0) text.append(buffer, got);
    std::fclose(fp);

    json::Value root;
    if (!json::parse(text, root, err)) {
        if (err && err->empty()) *err = "search.json illisible";
        return false;
    }
    const json::Value* list = root.find("providers");
    if (!list || !list->is_array()) return true;

    for (const json::Value& item : list->array) {
        if (!item.is_object()) continue;
        SearchProvider p;
        p.name = item.string_or("name", "sans nom");
        p.kind = item.string_or("kind", "torznab");
        p.url = item.string_or("url");
        p.api_key = item.string_or("api_key");
        const json::Value* enabled = item.find("enabled");
        p.enabled = !enabled || enabled->boolean;
        p.list_key = item.string_or("list_key", p.list_key);
        p.name_key = item.string_or("name_key", p.name_key);
        p.size_key = item.string_or("size_key", p.size_key);
        p.seeders_key = item.string_or("seeders_key", p.seeders_key);
        p.magnet_key = item.string_or("magnet_key", p.magnet_key);
        p.hash_key = item.string_or("hash_key", p.hash_key);
        // Une source désactivée n'est pas chargée du tout : l'appel qui suit
        // demande « y a-t-il quelque chose à interroger ? », pas « combien de
        // lignes contient le fichier ? ».
        if (!p.url.empty() && p.enabled) out.push_back(p);
    }
    return true;
}

}  // namespace bt
