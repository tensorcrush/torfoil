#include "bt/bencode.hpp"

#include <limits>

namespace bt {

namespace {

constexpr int kMaxDepth = 32;
// Un .torrent légitime dépasse rarement quelques Mo ; on refuse les longueurs
// délirantes avant d'allouer quoi que ce soit.
constexpr int64_t kMaxStringLen = 64 * 1024 * 1024;

class Parser {
public:
    Parser(const std::string& buf, std::string* err) : b_(buf), err_(err) {}

    bool parse(Value& v, int depth);
    size_t pos() const { return p_; }

private:
    bool fail(const char* msg) {
        if (err_ && err_->empty()) *err_ = msg;
        return false;
    }

    bool parse_int(Value& v);
    bool parse_str(Value& v);
    bool parse_list(Value& v, int depth);
    bool parse_dict(Value& v, int depth);

    // Lit un entier décimal jusqu'au délimiteur, avec garde-fou anti-débordement.
    bool read_number(char terminator, int64_t& out, bool allow_negative);

    const std::string& b_;
    std::string* err_;
    size_t p_ = 0;
};

bool Parser::read_number(char terminator, int64_t& out, bool allow_negative) {
    bool negative = false;
    if (p_ < b_.size() && b_[p_] == '-') {
        if (!allow_negative) return fail("bencode : nombre négatif interdit ici");
        negative = true;
        ++p_;
    }

    const size_t digits_begin = p_;
    int64_t value = 0;
    while (p_ < b_.size() && b_[p_] >= '0' && b_[p_] <= '9') {
        const int digit = b_[p_] - '0';
        if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
            return fail("bencode : entier hors bornes");
        }
        value = value * 10 + digit;
        ++p_;
    }

    if (p_ == digits_begin) return fail("bencode : entier attendu");
    // Forme canonique : pas de zéro en tête, pas de "-0".
    if (b_[digits_begin] == '0' && p_ - digits_begin > 1) {
        return fail("bencode : zéro en tête interdit");
    }
    if (negative && value == 0) return fail("bencode : -0 interdit");
    if (p_ >= b_.size() || b_[p_] != terminator) return fail("bencode : délimiteur attendu");
    ++p_;

    out = negative ? -value : value;
    return true;
}

bool Parser::parse_int(Value& v) {
    ++p_;  // 'i'
    v.type = Value::Type::Int;
    return read_number('e', v.i, /*allow_negative=*/true);
}

bool Parser::parse_str(Value& v) {
    int64_t len = 0;
    if (!read_number(':', len, /*allow_negative=*/false)) return false;
    if (len > kMaxStringLen) return fail("bencode : chaîne démesurée");
    if (b_.size() - p_ < static_cast<uint64_t>(len)) return fail("bencode : chaîne tronquée");

    v.type = Value::Type::Str;
    v.s.assign(b_, p_, static_cast<size_t>(len));
    p_ += static_cast<size_t>(len);
    return true;
}

bool Parser::parse_list(Value& v, int depth) {
    ++p_;  // 'l'
    v.type = Value::Type::List;
    while (true) {
        if (p_ >= b_.size()) return fail("bencode : liste non terminée");
        if (b_[p_] == 'e') {
            ++p_;
            return true;
        }
        v.l.emplace_back();
        if (!parse(v.l.back(), depth + 1)) return false;
    }
}

bool Parser::parse_dict(Value& v, int depth) {
    ++p_;  // 'd'
    v.type = Value::Type::Dict;
    while (true) {
        if (p_ >= b_.size()) return fail("bencode : dictionnaire non terminé");
        if (b_[p_] == 'e') {
            ++p_;
            return true;
        }

        Value key;
        if (b_[p_] < '0' || b_[p_] > '9') return fail("bencode : clé non textuelle");
        if (!parse_str(key)) return false;

        Value val;
        if (!parse(val, depth + 1)) return false;
        // En cas de clé dupliquée on garde la première : ré-encoder une clé
        // écrasée changerait l'info_hash.
        v.d.emplace(std::move(key.s), std::move(val));
    }
}

bool Parser::parse(Value& v, int depth) {
    if (depth > kMaxDepth) return fail("bencode : imbrication trop profonde");
    if (p_ >= b_.size()) return fail("bencode : entrée tronquée");

    v.raw_begin = p_;
    bool ok;
    switch (b_[p_]) {
        case 'i': ok = parse_int(v); break;
        case 'l': ok = parse_list(v, depth); break;
        case 'd': ok = parse_dict(v, depth); break;
        default:
            if (b_[p_] >= '0' && b_[p_] <= '9') {
                ok = parse_str(v);
            } else {
                return fail("bencode : octet inattendu");
            }
            break;
    }
    if (!ok) return false;
    v.raw_end = p_;
    return true;
}

void encode_into(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Type::Int:
            out += 'i';
            out += std::to_string(v.i);
            out += 'e';
            break;
        case Value::Type::Str:
            out += std::to_string(v.s.size());
            out += ':';
            out += v.s;
            break;
        case Value::Type::List:
            out += 'l';
            for (const Value& child : v.l) encode_into(child, out);
            out += 'e';
            break;
        case Value::Type::Dict:
            out += 'd';
            for (const auto& [key, child] : v.d) {
                out += std::to_string(key.size());
                out += ':';
                out += key;
                encode_into(child, out);
            }
            out += 'e';
            break;
    }
}

}  // namespace

const Value* Value::find(const std::string& key) const {
    if (type != Type::Dict) return nullptr;
    const auto it = d.find(key);
    return it == d.end() ? nullptr : &it->second;
}

const Value* Value::find_int(const std::string& key) const {
    const Value* v = find(key);
    return v && v->is_int() ? v : nullptr;
}

const Value* Value::find_str(const std::string& key) const {
    const Value* v = find(key);
    return v && v->is_str() ? v : nullptr;
}

const Value* Value::find_list(const std::string& key) const {
    const Value* v = find(key);
    return v && v->is_list() ? v : nullptr;
}

const Value* Value::find_dict(const std::string& key) const {
    const Value* v = find(key);
    return v && v->is_dict() ? v : nullptr;
}

int64_t Value::int_or(const std::string& key, int64_t fallback) const {
    const Value* v = find_int(key);
    return v ? v->i : fallback;
}

std::string Value::str_or(const std::string& key, const std::string& fallback) const {
    const Value* v = find_str(key);
    return v ? v->s : fallback;
}

Value Value::from_int(int64_t v) {
    Value out;
    out.type = Type::Int;
    out.i = v;
    return out;
}

Value Value::from_str(std::string v) {
    Value out;
    out.type = Type::Str;
    out.s = std::move(v);
    return out;
}

Value Value::list() {
    Value out;
    out.type = Type::List;
    return out;
}

Value Value::dict() {
    Value out;
    out.type = Type::Dict;
    return out;
}

bool bdecode(const std::string& buf, Value& out, std::string* err) {
    std::string local_err;
    Parser parser(buf, err ? err : &local_err);
    if (err) err->clear();
    return parser.parse(out, 0);
    // Note : on tolère des octets résiduels après la valeur. Certains trackers
    // ajoutent un \r\n, et refuser rendrait l'annonce inutilisable.
}

std::string bencode(const Value& v) {
    std::string out;
    encode_into(v, out);
    return out;
}

}  // namespace bt
