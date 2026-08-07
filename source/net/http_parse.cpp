#include "net/http_parse.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "util/bytes.hpp"

namespace net {

namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim_ws(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) --end;
    return s.substr(begin, end - begin);
}

// Valeur d'un paramètre d'en-tête : « ...; name="valeur" » ou « ...; name=valeur ».
std::string header_param(const std::string& value, const std::string& key) {
    const std::string haystack = lower(value);
    const std::string needle = lower(key) + "=";

    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        // Doit être précédé d'un début de chaîne, d'un « ; » ou d'un espace,
        // sinon « filename= » se laisserait trouver dans « xfilename= ».
        const bool boundary_ok =
            pos == 0 || haystack[pos - 1] == ';' || haystack[pos - 1] == ' ';
        if (!boundary_ok) {
            pos += needle.size();
            continue;
        }

        size_t start = pos + needle.size();
        if (start < value.size() && value[start] == '"') {
            ++start;
            const size_t end = value.find('"', start);
            if (end == std::string::npos) return {};
            return value.substr(start, end - start);
        }
        size_t end = value.find(';', start);
        if (end == std::string::npos) end = value.size();
        return trim_ws(value.substr(start, end - start));
    }
    return {};
}

}  // namespace

std::string HttpRequest::header(const std::string& name) const {
    const std::string key = lower(name);
    for (const auto& [k, v] : headers) {
        if (lower(k) == key) return v;
    }
    return {};
}

long long HttpRequest::content_length() const {
    const std::string value = header("content-length");
    if (value.empty()) return -1;
    char* end = nullptr;
    const long long n = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || n < 0) return -1;
    return n;
}

bool parse_http_headers(const std::string& raw, HttpRequest& out, size_t* body_offset,
                        bool* complete) {
    if (complete) *complete = false;

    const size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos) {
        // Pas encore tout reçu — sauf si le client envoie n'importe quoi et ne
        // s'arrêtera jamais. Au-delà de 16 Ko d'en-têtes, ce n'est plus un
        // navigateur.
        if (raw.size() > 16 * 1024) {
            if (complete) *complete = true;
            return false;
        }
        return false;
    }
    if (complete) *complete = true;
    if (body_offset) *body_offset = split + 4;

    const size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos || line_end > split) return false;

    // Ligne de requête : MÉTHODE cible VERSION
    const std::string line = raw.substr(0, line_end);
    const size_t first = line.find(' ');
    if (first == std::string::npos) return false;
    const size_t second = line.find(' ', first + 1);
    if (second == std::string::npos) return false;

    out.method = line.substr(0, first);
    out.target = line.substr(first + 1, second - first - 1);
    if (out.method.empty() || out.target.empty()) return false;

    const size_t qmark = out.target.find('?');
    if (qmark == std::string::npos) {
        out.path = util::url_decode(out.target);
    } else {
        out.path = util::url_decode(out.target.substr(0, qmark));
        out.query = out.target.substr(qmark + 1);
    }

    out.headers.clear();
    size_t pos = line_end + 2;
    while (pos < split) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos || end > split) end = split;

        const std::string field = raw.substr(pos, end - pos);
        const size_t colon = field.find(':');
        if (colon != std::string::npos) {
            out.headers.emplace_back(trim_ws(field.substr(0, colon)),
                                     trim_ws(field.substr(colon + 1)));
        }
        pos = end + 2;
    }
    return true;
}

std::string form_field(const std::string& encoded, const std::string& name) {
    size_t pos = 0;
    while (pos <= encoded.size()) {
        size_t end = encoded.find('&', pos);
        if (end == std::string::npos) end = encoded.size();

        const std::string pair = encoded.substr(pos, end - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, name) == 0) {
            return util::url_decode(pair.substr(eq + 1));
        }
        if (end == encoded.size()) break;
        pos = end + 1;
    }
    return {};
}

bool parse_multipart(const std::string& content_type, const std::string& body,
                     std::vector<MultipartPart>& out) {
    const std::string boundary = header_param(content_type, "boundary");
    if (boundary.empty()) return false;

    const std::string sep = "--" + boundary;
    size_t pos = body.find(sep);
    if (pos == std::string::npos) return false;
    pos += sep.size();

    while (pos < body.size()) {
        // Après la frontière : soit « -- » (fin), soit CRLF puis une partie.
        if (body.compare(pos, 2, "--") == 0) break;
        if (body.compare(pos, 2, "\r\n") != 0) return false;
        pos += 2;

        const size_t head_end = body.find("\r\n\r\n", pos);
        if (head_end == std::string::npos) return false;

        MultipartPart part;
        size_t line = pos;
        while (line < head_end) {
            size_t line_end = body.find("\r\n", line);
            if (line_end == std::string::npos || line_end > head_end) line_end = head_end;

            const std::string field = body.substr(line, line_end - line);
            const size_t colon = field.find(':');
            if (colon != std::string::npos) {
                const std::string key = lower(trim_ws(field.substr(0, colon)));
                const std::string value = trim_ws(field.substr(colon + 1));
                if (key == "content-disposition") {
                    part.name = header_param(value, "name");
                    part.filename = header_param(value, "filename");
                } else if (key == "content-type") {
                    part.content_type = value;
                }
            }
            line = line_end + 2;
        }

        const size_t data_begin = head_end + 4;
        const size_t next = body.find("\r\n" + sep, data_begin);
        if (next == std::string::npos) return false;

        part.data = body.substr(data_begin, next - data_begin);
        out.push_back(std::move(part));

        pos = next + 2 + sep.size();
    }
    return true;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

}  // namespace net
