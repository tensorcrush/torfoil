#include "net/json.hpp"

#include <cstdlib>

namespace json {

namespace {

class Parser {
public:
    Parser(const std::string& text, std::string* err) : s_(text), err_(err) {}

    bool parse(Value& out, int depth);
    void skip_space();
    size_t pos() const { return p_; }

private:
    bool fail(const char* msg) {
        if (err_ && err_->empty()) *err_ = msg;
        return false;
    }

    bool parse_string(std::string& out);
    bool parse_number(Value& out);
    bool literal(const char* word, size_t len);

    const std::string& s_;
    std::string* err_;
    size_t p_ = 0;
};

void Parser::skip_space() {
    while (p_ < s_.size() &&
           (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) {
        ++p_;
    }
}

bool Parser::literal(const char* word, size_t len) {
    if (s_.compare(p_, len, word) != 0) return false;
    p_ += len;
    return true;
}

bool Parser::parse_string(std::string& out) {
    if (p_ >= s_.size() || s_[p_] != '"') return fail("chaîne attendue");
    ++p_;

    out.clear();
    while (p_ < s_.size()) {
        const char c = s_[p_++];
        if (c == '"') return true;

        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (p_ >= s_.size()) break;

        const char esc = s_[p_++];
        switch (esc) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                // On ne garde que le plan latin ; le reste devient '?'. Les
                // champs qui nous intéressent sont de l'ASCII.
                if (p_ + 4 > s_.size()) return fail("échappement \\u tronqué");
                const long code = std::strtol(s_.substr(p_, 4).c_str(), nullptr, 16);
                p_ += 4;
                if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                } else {
                    out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                }
                break;
            }
            default: out.push_back(esc); break;
        }
    }
    return fail("chaîne non terminée");
}

bool Parser::parse_number(Value& out) {
    const size_t start = p_;
    if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) ++p_;
    while (p_ < s_.size() && ((s_[p_] >= '0' && s_[p_] <= '9') || s_[p_] == '.' ||
                              s_[p_] == 'e' || s_[p_] == 'E' || s_[p_] == '-' || s_[p_] == '+')) {
        ++p_;
    }
    if (p_ == start) return fail("nombre attendu");

    out.type = Value::Type::Number;
    out.number = std::strtod(s_.substr(start, p_ - start).c_str(), nullptr);
    return true;
}

bool Parser::parse(Value& out, int depth) {
    if (depth > 64) return fail("imbrication trop profonde");
    skip_space();
    if (p_ >= s_.size()) return fail("entrée tronquée");

    const char c = s_[p_];

    if (c == '"') {
        out.type = Value::Type::String;
        return parse_string(out.text);
    }
    if (c == '{') {
        ++p_;
        out.type = Value::Type::Object;
        skip_space();
        if (p_ < s_.size() && s_[p_] == '}') {
            ++p_;
            return true;
        }
        while (true) {
            skip_space();
            std::string key;
            if (!parse_string(key)) return false;
            skip_space();
            if (p_ >= s_.size() || s_[p_] != ':') return fail("':' attendu");
            ++p_;

            Value child;
            if (!parse(child, depth + 1)) return false;
            out.object.emplace_back(std::move(key), std::move(child));

            skip_space();
            if (p_ < s_.size() && s_[p_] == ',') {
                ++p_;
                continue;
            }
            if (p_ < s_.size() && s_[p_] == '}') {
                ++p_;
                return true;
            }
            return fail("',' ou '}' attendu");
        }
    }
    if (c == '[') {
        ++p_;
        out.type = Value::Type::Array;
        skip_space();
        if (p_ < s_.size() && s_[p_] == ']') {
            ++p_;
            return true;
        }
        while (true) {
            Value child;
            if (!parse(child, depth + 1)) return false;
            out.array.push_back(std::move(child));

            skip_space();
            if (p_ < s_.size() && s_[p_] == ',') {
                ++p_;
                continue;
            }
            if (p_ < s_.size() && s_[p_] == ']') {
                ++p_;
                return true;
            }
            return fail("',' ou ']' attendu");
        }
    }
    if (c == 't' && literal("true", 4)) {
        out.type = Value::Type::Bool;
        out.boolean = true;
        return true;
    }
    if (c == 'f' && literal("false", 5)) {
        out.type = Value::Type::Bool;
        out.boolean = false;
        return true;
    }
    if (c == 'n' && literal("null", 4)) {
        out.type = Value::Type::Null;
        return true;
    }
    return parse_number(out);
}

}  // namespace

const Value* Value::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& entry : object) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

std::string Value::string_or(const std::string& key, const std::string& fallback) const {
    const Value* v = find(key);
    return (v && v->type == Type::String) ? v->text : fallback;
}

double Value::number_or(const std::string& key, double fallback) const {
    const Value* v = find(key);
    return (v && v->type == Type::Number) ? v->number : fallback;
}

bool parse(const std::string& text, Value& out, std::string* err) {
    std::string local;
    Parser parser(text, err ? err : &local);
    if (err) err->clear();
    return parser.parse(out, 0);
}

}  // namespace json
