// Analyseur JSON minimal, juste ce qu'il faut pour l'API Mullvad.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }

    const Value* find(const std::string& key) const;
    std::string string_or(const std::string& key, const std::string& fallback = {}) const;
    double number_or(const std::string& key, double fallback = 0) const;
};

bool parse(const std::string& text, Value& out, std::string* err = nullptr);

}  // namespace json
