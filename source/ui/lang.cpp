#include "ui/lang.hpp"

#include <cstdarg>
#include <cstdio>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace ui {

namespace {

constexpr size_t kLangCount = static_cast<size_t>(Lang::kCount);

// Une ligne = une clé, ses sept traductions dans l'ordre de l'énumération. La
// table est engendrée à partir du même fichier que les clés : une colonne
// oubliée ne compile pas, elle ne se transforme pas en texte manquant à
// l'exécution.
struct Row {
    const char* text[kLangCount];
};

const Row kTable[] = {
#define X(name, de, en, es, fr, ja, ru, zh) {{de, en, es, fr, ja, ru, zh}},
#include "ui/strings.def"
#undef X
};

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(Str::kCount),
              "la table et l'énumération des clés ont divergé");

Lang g_current = Lang::En;

}  // namespace

const char* tr(Str key) {
    const size_t index = static_cast<size_t>(key);
    if (index >= static_cast<size_t>(Str::kCount)) return "";
    return kTable[index].text[static_cast<size_t>(g_current)];
}

std::string trf(Str key, ...) {
    const char* format = tr(key);

    va_list args;
    va_start(args, key);
    va_list measure;
    va_copy(measure, args);
    const int needed = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);

    std::string out;
    if (needed > 0) {
        std::vector<char> buffer(static_cast<size_t>(needed) + 1);
        std::vsnprintf(buffer.data(), buffer.size(), format, args);
        out.assign(buffer.data(), static_cast<size_t>(needed));
    }
    va_end(args);
    return out;
}

void set_language(Lang lang) {
    if (lang < Lang::kCount) g_current = lang;
}

Lang language() {
    return g_current;
}

const char* code_of(Lang lang) {
    switch (lang) {
        case Lang::De: return "de";
        case Lang::En: return "en";
        case Lang::Es: return "es";
        case Lang::Fr: return "fr";
        case Lang::Ja: return "ja";
        case Lang::Ru: return "ru";
        case Lang::Zh: return "zh";
        default: return "en";
    }
}

bool lang_from_code(const std::string& code, Lang& out) {
    for (size_t i = 0; i < kLangCount; ++i) {
        const Lang lang = static_cast<Lang>(i);
        if (code == code_of(lang)) {
            out = lang;
            return true;
        }
    }
    return false;
}

const char* endonym(Lang lang) {
    switch (lang) {
        case Lang::De: return "Deutsch";
        case Lang::En: return "English";
        case Lang::Es: return "Español";
        case Lang::Fr: return "Français";
        case Lang::Ja: return "日本語";
        case Lang::Ru: return "Русский";
        case Lang::Zh: return "中文";
        default: return "English";
    }
}

Lang console_language() {
#ifdef __SWITCH__
    // setGetSystemLanguage rend un code brut ; setMakeLanguage le traduit en
    // valeur d'énumération. Les deux appels peuvent échouer si le service n'est
    // pas ouvert, d'où le repli.
    uint64_t code = 0;
    SetLanguage system_lang = SetLanguage_ENUS;
    if (R_FAILED(setGetSystemLanguage(&code))) return Lang::En;
    if (R_FAILED(setMakeLanguage(code, &system_lang))) return Lang::En;

    switch (system_lang) {
        case SetLanguage_DE: return Lang::De;
        case SetLanguage_ES:
        case SetLanguage_ES419: return Lang::Es;
        case SetLanguage_FR:
        case SetLanguage_FRCA: return Lang::Fr;
        case SetLanguage_JA: return Lang::Ja;
        case SetLanguage_RU: return Lang::Ru;
        case SetLanguage_ZHCN:
        case SetLanguage_ZHTW:
        case SetLanguage_ZHHANS:
        case SetLanguage_ZHHANT: return Lang::Zh;
        default:
            // Italien, néerlandais, portugais, coréen : la console est réglée
            // dans une langue que nous ne parlons pas encore. L'anglais est le
            // repli le moins mauvais, et sûrement pas le français.
            return Lang::En;
    }
#else
    return Lang::En;
#endif
}

}  // namespace ui
