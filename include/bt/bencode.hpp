// Décodeur/encodeur bencode (BEP 3).
//
// Particularité importante : chaque valeur mémorise sa position exacte dans le
// tampon d'origine (raw_begin/raw_end). C'est ce qui permet de calculer
// l'info_hash — SHA-1 des octets *bruts* du dictionnaire « info », qu'on ne peut
// pas ré-encoder soi-même sans risquer de changer un octet.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bt {

struct Value {
    enum class Type { Int, Str, List, Dict };

    Type type = Type::Int;
    int64_t i = 0;
    std::string s;
    std::vector<Value> l;
    std::map<std::string, Value> d;

    // Étendue de cette valeur dans le tampon source (valide après bdecode).
    size_t raw_begin = 0;
    size_t raw_end = 0;

    bool is_int() const { return type == Type::Int; }
    bool is_str() const { return type == Type::Str; }
    bool is_list() const { return type == Type::List; }
    bool is_dict() const { return type == Type::Dict; }

    // Recherche dans un dictionnaire. Renvoie nullptr si absent ou si `this`
    // n'est pas un dictionnaire — jamais d'exception, les .torrent sont hostiles.
    const Value* find(const std::string& key) const;

    // Recherche typée : nullptr si la clé existe mais n'a pas le bon type.
    const Value* find_int(const std::string& key) const;
    const Value* find_str(const std::string& key) const;
    const Value* find_list(const std::string& key) const;
    const Value* find_dict(const std::string& key) const;

    int64_t int_or(const std::string& key, int64_t fallback) const;
    std::string str_or(const std::string& key, const std::string& fallback) const;

    static Value from_int(int64_t v);
    static Value from_str(std::string v);
    static Value list();
    static Value dict();
};

// Décode UNE valeur bencode. `buf` est conservé tel quel (les offsets y réfèrent).
// Renvoie false et remplit `err` si l'entrée est malformée.
bool bdecode(const std::string& buf, Value& out, std::string* err = nullptr);

// Encode. Les clés de dictionnaire sortent triées par octet, comme l'exige BEP 3
// (std::map<std::string> donne déjà cet ordre).
std::string bencode(const Value& v);

}  // namespace bt
