// Tests exécutables sur PC.
//
// Une bonne partie du projet ne dépend ni de libnx ni du matériel : parsing,
// sélection de pièces, et surtout toute la crypto WireGuard. Ces morceaux-là se
// compilent avec g++ et se confrontent aux vecteurs officiels — c'est la seule
// vérification réelle possible sans console sous la main.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "bt/bencode.hpp"
#include "bt/magnet.hpp"
#include "bt/metainfo.hpp"
#include "bt/piece_picker.hpp"
#include "net/http_parse.hpp"
#include "ui/lang.hpp"
#include "net/http_server.hpp"
#include "bt/storage.hpp"
#include <sys/resource.h>

#include <csignal>

#include "net/wg/blake2s.h"
#include "net/wg/chacha20poly1305.h"
#include "net/wg/wireguard.hpp"
#include "net/wg/x25519.h"
#include "util/bytes.hpp"
#include "util/qr.hpp"
#include "util/sha1.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
const char* g_section = "";

void section(const char* name) {
    g_section = name;
    std::printf("\n\033[1;36m%s\033[0m\n", name);
}

void check(bool ok, const std::string& label) {
    if (ok) {
        ++g_pass;
        std::printf("  \033[32m✓\033[0m %s\n", label.c_str());
    } else {
        ++g_fail;
        std::printf("  \033[31m✗ %s\033[0m\n", label.c_str());
    }
}

void check_hex(const std::vector<uint8_t>& actual, const std::string& expected_hex,
               const std::string& label) {
    const std::string got = util::to_hex(actual.data(), actual.size());
    check(got == expected_hex, label);
    if (got != expected_hex) {
        std::printf("      attendu : %s\n      obtenu  : %s\n", expected_hex.c_str(),
                    got.c_str());
    }
}

std::vector<uint8_t> unhex(const std::string& hex) {
    std::vector<uint8_t> out(hex.size() / 2);
    util::from_hex(hex, out.data(), out.size());
    return out;
}

// ---------------------------------------------------------------------------

void test_sha1() {
    section("SHA-1");
    check(util::to_hex(util::Sha1::of(std::string("abc"))) ==
              "a9993e364706816aba3e25717850c26c9cd0d89d",
          "\"abc\"");
    check(util::to_hex(util::Sha1::of(std::string(""))) ==
              "da39a3ee5e6b4b0d3255bfef95601890afd80709",
          "chaîne vide");
    check(util::to_hex(util::Sha1::of(std::string(1000, 'a'))) ==
              "291e9a6c66994949b57ba5e650361e98fc36b1ba",
          "1000 × 'a' (multi-blocs)");
    check(util::to_hex(util::Sha1::of(std::string(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
          "vecteur FIPS 180-2");
}

void test_bencode() {
    section("bencode");
    std::string err;

    const std::string sample = "d3:cow3:moo4:spam4:eggse";
    bt::Value v;
    check(bt::bdecode(sample, v, &err), "décodage");
    check(v.str_or("cow", "") == "moo", "lecture de clé");
    check(bt::bencode(v) == sample, "aller-retour identique");

    bt::Value nested;
    const std::string with_info = "d4:infod6:lengthi42e4:name3:abcee";
    check(bt::bdecode(with_info, nested, &err), "dictionnaire imbriqué");
    const bt::Value* info = nested.find_dict("info");
    check(info != nullptr, "sous-dictionnaire trouvé");
    if (info) {
        const std::string raw = with_info.substr(info->raw_begin, info->raw_end - info->raw_begin);
        check(raw == "d6:lengthi42e4:name3:abce", "étendue brute exacte (base de l'info_hash)");
    }

    bt::Value bad;
    check(!bt::bdecode("i03e", bad, &err), "rejet du zéro en tête");
    check(!bt::bdecode("i-0e", bad, &err), "rejet de -0");
    check(!bt::bdecode("5:abc", bad, &err), "rejet chaîne tronquée");
    check(!bt::bdecode("d1:ai1e", bad, &err), "rejet dictionnaire non fermé");
    check(!bt::bdecode("di1ei2ee", bad, &err), "rejet clé non textuelle");
}

void test_torrent() {
    section("metainfo");
    std::string err;

    // Torrent mono-fichier minimal : 3 pièces de 16 octets, 40 octets au total.
    std::string pieces;
    for (int i = 0; i < 3; ++i) pieces.append(20, static_cast<char>('A' + i));

    std::string torrent = "d8:announce18:udp://tracker:1337";
    torrent += "4:infod6:lengthi40e4:name7:jeu.bin12:piece lengthi16e6:pieces60:";
    torrent += pieces;
    torrent += "ee";

    bt::MetaInfo meta;
    check(bt::parse_torrent(torrent, meta, &err), "analyse d'un .torrent");
    check(meta.total_size == 40, "taille totale");
    check(meta.piece_count() == 3, "nombre de pièces");
    check(meta.size_of_piece(0) == 16 && meta.size_of_piece(2) == 8, "dernière pièce plus courte");
    check(meta.files.size() == 1 && meta.files[0].path == "jeu.bin", "fichier unique");
    check(meta.trackers.size() == 1, "tracker relevé");

    // Cohérence taille/pièces : un torrent qui ment doit être rejeté.
    std::string broken = torrent;
    const size_t pos = broken.find("i40e");
    broken.replace(pos, 4, "i99e");
    bt::MetaInfo bad;
    check(!bt::parse_torrent(broken, bad, &err), "rejet taille incohérente");
}

void test_magnet() {
    section("magnet");
    std::string err;

    bt::MagnetLink hex_link;
    check(bt::parse_magnet("magnet:?xt=urn:btih:c9e15763f722f23e98a29decdfae341b98d53056"
                           "&dn=Test&tr=udp%3A%2F%2Ftr.example%3A1337%2Fannounce",
                           hex_link, &err),
          "lien hexadécimal");
    check(util::to_hex(hex_link.info_hash) == "c9e15763f722f23e98a29decdfae341b98d53056",
          "info_hash");
    check(hex_link.trackers.size() == 1 &&
              hex_link.trackers[0] == "udp://tr.example:1337/announce",
          "tracker désencodé");

    bt::MagnetLink b32;
    check(bt::parse_magnet("magnet:?xt=urn:btih:ZHQVOY7XELZD5GFCTXWN7LRUDOMNKMCW", b32, &err),
          "lien base32");
    check(b32.info_hash == hex_link.info_hash, "base32 == hexadécimal");

    bt::MagnetLink v2;
    check(!bt::parse_magnet("magnet:?xt=urn:btmh:1220abcd", v2, &err), "rejet torrent v2");
    check(!bt::parse_magnet("http://example.com", v2, &err), "rejet non-magnet");
}

void test_qr() {
    section("code QR");

    // Vecteur publié : ces seize codewords de données donnent ces dix
    // codewords de correction. C'est la seule partie du codage qu'on ne peut
    // pas juger à l'œil, et celle qui décide qu'un code est lisible ou non.
    const std::vector<uint8_t> data = {32,  91, 11, 120, 209, 114, 220, 77,
                                       67,  64, 236, 17, 236, 17,  236, 17};
    const std::vector<uint8_t> expected = {196, 35, 39, 119, 235, 215, 231, 226, 93, 23};
    check(util::qr_reed_solomon(data, 10) == expected, "Reed-Solomon conforme au vecteur connu");

    // Chaque version doit passer le contrôle interne de capacité : le nombre de
    // modules libres laissés par les motifs de service est comparé à celui que
    // la norme annonce. Un motif mal posé décale tout le flux d'un module, et
    // le code sort silencieusement illisible.
    const int lengths[] = {5, 20, 40, 60, 80, 105};
    bool all_versions = true;
    int last_version = 0;
    for (int n : lengths) {
        util::QrCode code;
        if (!util::qr_encode(std::string(static_cast<size_t>(n), 'A'), code)) all_versions = false;
        else if (code.version <= last_version || code.size != 17 + 4 * code.version) {
            all_versions = false;
        } else {
            last_version = code.version;
        }
    }
    check(all_versions, "versions 1 à 6 encodées, disposition cohérente");
    check(last_version == 6, "la version croît avec la longueur");

    util::QrCode oversized;
    check(!util::qr_encode(std::string(107, 'A'), oversized), "au-delà de 106 octets, refus net");

    util::QrCode code;
    check(util::qr_encode("http://192.168.1.100:8080/", code), "URL de réseau local encodée");
    check(code.version == 2 && code.size == 25, "version 2, 25 modules de côté");

    // Motif de repère : anneau sombre, anneau clair, cœur sombre.
    check(code.at(0, 0) && code.at(6, 0) && code.at(0, 6) && !code.at(1, 1) && code.at(2, 2) &&
              code.at(4, 4) && !code.at(5, 5),
          "motif de repère bien formé");

    bool timing = true;
    for (int i = 8; i < code.size - 8; ++i) {
        if (code.at(i, 6) != (i % 2 == 0) || code.at(6, i) != (i % 2 == 0)) timing = false;
    }
    check(timing, "motifs de synchronisation alternés");

    // Alignement en version 2 : centre en (18,18), entouré d'un anneau clair.
    check(code.at(18, 18) && !code.at(17, 17) && !code.at(19, 19) && code.at(16, 16),
          "motif d'alignement en place");
    check(code.at(8, code.size - 8), "module toujours sombre présent");
}

// ---------------------------------------------------------------------------
// Analyse HTTP — la porte d'entrée du téléphone
// ---------------------------------------------------------------------------

void test_http_parse() {
    section("analyse HTTP");

    net::HttpRequest request;
    size_t body_offset = 0;
    bool complete = false;

    // Des en-têtes coupés en deux paquets sont la règle, pas l'exception : il
    // faut attendre la suite, surtout pas fermer la connexion.
    check(!net::parse_http_headers("POST /api/add HTTP/1.1\r\nHost: 192", request, &body_offset,
                                   &complete) &&
              !complete,
          "en-têtes incomplets : on attend la suite");

    const std::string raw =
        "POST /api/add?x=1 HTTP/1.1\r\n"
        "Host: 192.168.1.10:8080\r\n"
        "CONTENT-type: multipart/form-data; boundary=----abc\r\n"
        "Content-Length: 7\r\n"
        "\r\n"
        "1234567";
    check(net::parse_http_headers(raw, request, &body_offset, &complete) && complete,
          "requête complète analysée");
    check(request.method == "POST" && request.path == "/api/add" && request.query == "x=1",
          "méthode, chemin et query séparés");
    check(request.content_length() == 7, "Content-Length lu");
    check(request.content_type().find("multipart") == 0,
          "en-tête retrouvé quelle que soit la casse");
    check(raw.substr(body_offset) == "1234567", "début du corps repéré");

    // Multipart : deux fichiers et un champ texte dans le même envoi, ce que
    // fait le sélecteur de fichiers d'iOS quand on en choisit plusieurs.
    const std::string boundary = "------WebKitFormBoundaryXYZ";
    const std::string body = boundary + "\r\n" +
                             "Content-Disposition: form-data; name=\"torrent\"; "
                             "filename=\"jeu un.torrent\"\r\n"
                             "Content-Type: application/x-bittorrent\r\n\r\n" +
                             std::string("d4:infod\x00\x01", 10) + "\r\n" + boundary + "\r\n" +
                             "Content-Disposition: form-data; name=\"torrent\"; "
                             "filename=\"deux.torrent\"\r\n\r\n" +
                             "SECOND" + "\r\n" + boundary + "\r\n" +
                             "Content-Disposition: form-data; name=\"magnet\"\r\n\r\n" +
                             "magnet:?xt=urn:btih:aa\r\nmagnet:?xt=urn:btih:bb" + "\r\n" +
                             boundary + "--\r\n";

    std::vector<net::MultipartPart> parts;
    check(net::parse_multipart("multipart/form-data; boundary=" + boundary.substr(2), body, parts),
          "corps multipart découpé");
    check(parts.size() == 3, "trois parties retrouvées");
    if (parts.size() == 3) {
        check(parts[0].filename == "jeu un.torrent" && parts[0].name == "torrent",
              "nom de fichier avec espace conservé");
        check(parts[0].data == std::string("d4:infod\x00\x01", 10),
              "octets binaires intacts, zéro compris");
        check(parts[1].data == "SECOND", "deuxième fichier isolé");
        check(parts[2].name == "magnet" &&
                  parts[2].data == "magnet:?xt=urn:btih:aa\r\nmagnet:?xt=urn:btih:bb",
              "champ texte multiligne");
    } else {
        check(false, "octets binaires intacts");
        check(false, "deuxième fichier isolé");
        check(false, "champ texte multiligne");
    }

    std::vector<net::MultipartPart> none;
    check(!net::parse_multipart("text/plain", body, none), "sans frontière, refus");

    check(net::form_field("op=remove&hash=abcd", "hash") == "abcd", "champ de formulaire");
    check(net::form_field("op=remove&hash=abcd", "op") == "remove", "premier champ");
    check(net::form_field("a=1&b=2", "c").empty(), "champ absent");
    check(net::form_field("q=deux+mots%2Fun", "q") == "deux mots/un", "décodage %XX et +");

    check(net::json_escape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd", "échappement JSON");
    check(net::html_escape("<a & \"b\">") == "&lt;a &amp; &quot;b&quot;&gt;",
          "échappement HTML");

    // Une adresse hors du réseau local ne doit jamais être servie.
    check(net::is_private_ipv4(0xC0A80101u) && net::is_private_ipv4(0x0A000001u) &&
              net::is_private_ipv4(0xAC100001u),
          "adresses privées reconnues");
    check(!net::is_private_ipv4(0x08080808u) && !net::is_private_ipv4(0xAC0F0001u),
          "adresses publiques refusées");
}

// ---------------------------------------------------------------------------
// Table des traductions
//
// Sept colonnes et plus de mille quatre cents chaînes : personne ne relira ça
// à l'œil. Deux propriétés se vérifient en revanche mécaniquement, et ce sont
// justement celles dont l'absence casse quelque chose.
// ---------------------------------------------------------------------------


// Extrait les marqueurs de format d'une chaîne, « %% » exclu.
std::vector<std::string> format_specifiers(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') continue;
        if (i + 1 < text.size() && text[i + 1] == '%') {
            ++i;  // littéral, pas un argument
            continue;
        }
        size_t end = i + 1;
        while (end < text.size() && std::strchr("diouxXeEfgGcspn", text[end]) == nullptr) ++end;
        if (end < text.size()) {
            out.push_back(text.substr(i, end - i + 1));
            i = end;
        }
    }
    return out;
}

void test_translations() {
    section("traductions");

    const int langs = static_cast<int>(ui::Lang::kCount);
    const int keys = static_cast<int>(ui::Str::kCount);
    check(langs == 7, "sept langues");
    check(keys > 150, std::to_string(keys) + " textes par langue");

    // 1) Aucune case vide. Une traduction oubliée ne se voit pas à la
    // compilation si quelqu'un met "" pour aller vite ; elle se voit à
    // l'écran, sous la forme d'un libellé absent.
    int empty = 0;
    for (int l = 0; l < langs; ++l) {
        ui::set_language(static_cast<ui::Lang>(l));
        for (int k = 0; k < keys; ++k) {
            const char* text = ui::tr(static_cast<ui::Str>(k));
            if (text == nullptr || text[0] == '\0') ++empty;
        }
    }
    check(empty == 0, "aucune traduction vide (" + std::to_string(langs * keys) + " vérifiées)");

    // 2) Les marqueurs de format identiques d'une langue à l'autre. C'est LA
    // faute qui ne pardonne pas : trf() passe les mêmes arguments quelle que
    // soit la langue, donc un « %s » devenu « %d » dans une seule colonne lit
    // un pointeur comme un entier — au mieux un affichage absurde, au pire un
    // plantage, et seulement chez les utilisateurs de cette langue-là.
    int mismatches = 0;
    std::string first_bad;
    for (int k = 0; k < keys; ++k) {
        ui::set_language(ui::Lang::De);
        const std::vector<std::string> reference =
            format_specifiers(ui::tr(static_cast<ui::Str>(k)));

        for (int l = 1; l < langs; ++l) {
            ui::set_language(static_cast<ui::Lang>(l));
            const std::string text = ui::tr(static_cast<ui::Str>(k));
            if (format_specifiers(text) == reference) continue;
            ++mismatches;
            if (first_bad.empty()) first_bad = "clé " + std::to_string(k) + " : « " + text + " »";
        }
    }
    check(mismatches == 0,
          mismatches == 0 ? "marqueurs de format cohérents entre les sept langues"
                          : "marqueurs incohérents — " + first_bad);

    // 3) Les codes persistés dans settings.cfg font l'aller-retour.
    bool codes_ok = true;
    for (int l = 0; l < langs; ++l) {
        const ui::Lang lang = static_cast<ui::Lang>(l);
        ui::Lang back = ui::Lang::En;
        if (!ui::lang_from_code(ui::code_of(lang), back) || back != lang) codes_ok = false;
        if (ui::endonym(lang)[0] == '\0') codes_ok = false;
    }
    check(codes_ok, "codes de langue et noms natifs cohérents");

    ui::Lang unknown = ui::Lang::En;
    check(!ui::lang_from_code("kl", unknown), "code inconnu refusé");

    // 4) Les langues disent vraiment des choses différentes : une table remplie
    // par copier-coller passerait les trois épreuves précédentes sans broncher.
    ui::set_language(ui::Lang::Fr);
    const std::string fr = ui::tr(ui::Str::TabSettings);
    ui::set_language(ui::Lang::Ja);
    const std::string ja = ui::tr(ui::Str::TabSettings);
    ui::set_language(ui::Lang::Ru);
    const std::string ru = ui::tr(ui::Str::TabSettings);
    check(fr != ja && ja != ru && fr != ru, "les colonnes portent des textes distincts");

    // 5) trf() substitue réellement, dans la langue active.
    ui::set_language(ui::Lang::Es);
    check(ui::trf(ui::Str::ToastTorrentsAdded, 4) == "4 torrents añadidos",
          "trf remplit le marqueur dans la langue active");

    ui::set_language(ui::Lang::En);
}

void test_bitfield() {
    section("bitfield");

    bt::Bitfield bf;
    bf.resize(12);
    bf.set(0);
    bf.set(11);
    check(bf.get(0) && bf.get(11) && !bf.get(5), "set/get");
    check(bf.count() == 2, "comptage");
    bf.set(0, false);
    check(bf.count() == 1, "décomptage");

    const uint8_t good[2] = {0xff, 0xf0};
    const uint8_t bad[2] = {0xff, 0xf1};
    bt::Bitfield from_peer;
    check(from_peer.from_bytes(good, 2, 12), "bitfield valide accepté");
    check(from_peer.count() == 12 && from_peer.full(), "tout à 1");
    check(!from_peer.from_bytes(bad, 2, 12), "bourrage non nul rejeté");
    check(!from_peer.from_bytes(good, 1, 12), "longueur incorrecte rejetée");
}

void test_picker() {
    section("piece picker");

    bt::MetaInfo meta;
    meta.piece_length = 32 * 1024;   // 2 blocs par pièce
    meta.total_size = 4 * 32 * 1024;
    meta.piece_hashes.resize(4);
    meta.name = "t";
    meta.single_file = true;

    bt::PiecePicker picker;
    picker.init(meta);

    bt::Bitfield peer;
    peer.resize(4);
    for (uint32_t i = 0; i < 4; ++i) peer.set(i);
    picker.peer_added(peer);

    bt::BlockRequest picks[8];
    const uint32_t got = picker.pick(peer, picks, 8, 1000);
    check(got == 8, "8 blocs choisis (4 pièces × 2)");

    // Aucun doublon hors mode « fin de partie ».
    bool duplicate = false;
    for (uint32_t i = 0; i < got && !duplicate; ++i) {
        for (uint32_t j = i + 1; j < got; ++j) {
            if (picks[i].piece == picks[j].piece && picks[i].offset == picks[j].offset) {
                duplicate = true;
                break;
            }
        }
    }
    check(!duplicate, "pas de bloc demandé deux fois");

    const uint32_t again = picker.pick(peer, picks, 8, 1000);
    check(again == 0, "plus rien à demander tant que rien n'est reçu");

    check(picker.expire_requests(1000 + 60000, 45000) == 8, "expiration des requêtes");
    check(picker.pick(peer, picks, 8, 120000) == 8, "blocs re-proposés après expiration");

    // Réception complète d'une pièce.
    const uint32_t piece = picks[0].piece;
    check(!picker.on_block_received(piece, 0, 16384), "pièce incomplète après 1 bloc");
    check(picker.on_block_received(piece, 16384, 16384), "pièce complète après 2 blocs");
    picker.on_piece_verified(piece, true);
    check(picker.have().get(piece), "pièce validée retenue");
    check(picker.bytes_done() == 32 * 1024, "octets comptabilisés");

    picker.on_piece_verified(picks[2].piece, false);
    check(!picker.have().get(picks[2].piece), "pièce corrompue non retenue");
}

// ---------------------------------------------------------------------------
// Crypto WireGuard — vecteurs officiels
// ---------------------------------------------------------------------------

void test_blake2s() {
    section("BLAKE2s (RFC 7693)");

    std::vector<uint8_t> out(32);
    blake2s(out.data(), 32, nullptr, 0, "abc", 3);
    check_hex(out, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982",
              "BLAKE2s-256(\"abc\")");

    blake2s(out.data(), 32, nullptr, 0, "", 0);
    check_hex(out, "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9",
              "BLAKE2s-256(\"\")");

    // Message plus long qu'un bloc : vérifie la gestion du tampon interne.
    const std::string long_msg(1000, 'a');
    blake2s(out.data(), 32, nullptr, 0, long_msg.data(), long_msg.size());
    const std::string got = util::to_hex(out.data(), out.size());
    // Recalcul en alimentant octet par octet : les deux chemins doivent coïncider.
    blake2s_state s;
    blake2s_init(&s, 32);
    for (char c : long_msg) blake2s_update(&s, &c, 1);
    std::vector<uint8_t> streamed(32);
    blake2s_final(&s, streamed.data());
    check(got == util::to_hex(streamed.data(), streamed.size()),
          "identique en un bloc et en flux octet par octet");

    // HMAC : la clé longue doit être pré-hachée, pas tronquée.
    uint8_t mac_short[32];
    uint8_t mac_long[32];
    const std::string key_long(100, 'k');
    blake2s_hmac(mac_short, "cle", 3, "message", 7);
    blake2s_hmac(mac_long, key_long.data(), key_long.size(), "message", 7);
    check(std::memcmp(mac_short, mac_long, 32) != 0, "HMAC distingue les clés");
}

void test_chacha20() {
    section("ChaCha20 (RFC 8439 §2.4.2)");

    const auto key = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    // Attention : le §2.4.2 utilise un nonce commençant par quatre octets nuls.
    // Celui du §2.3.2 (00000009…) illustre la fonction de bloc, pas le
    // chiffrement — les confondre donne un résultat parfaitement cohérent mais
    // sans rapport avec le vecteur attendu.
    const auto nonce = unhex("000000000000004a00000000");
    const std::string plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";

    std::vector<uint8_t> cipher(plain.size());
    chacha20_xor(cipher.data(), reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
                 key.data(), nonce.data(), 1);

    check_hex(cipher,
              "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab"
              "8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e"
              "52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d",
              "chiffrement");

    // Déchiffrer = rechiffrer.
    std::vector<uint8_t> back(plain.size());
    chacha20_xor(back.data(), cipher.data(), cipher.size(), key.data(), nonce.data(), 1);
    check(std::memcmp(back.data(), plain.data(), plain.size()) == 0, "involution");
}

void test_aead() {
    section("ChaCha20-Poly1305 (RFC 8439 §2.8.2)");

    const auto key = unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const auto nonce = unhex("070000004041424344454647");
    const auto aad = unhex("50515253c0c1c2c3c4c5c6c7");
    const std::string plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";

    std::vector<uint8_t> cipher(plain.size());
    uint8_t tag[16];
    chacha20poly1305_encrypt(cipher.data(), tag, reinterpret_cast<const uint8_t*>(plain.data()),
                             plain.size(), aad.data(), aad.size(), key.data(), nonce.data());

    check_hex(cipher,
              "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca96712"
              "82fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58"
              "fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116",
              "texte chiffré");

    check_hex(std::vector<uint8_t>(tag, tag + 16), "1ae10b594f09e26a7e902ecbd0600691",
              "étiquette d'authentification");

    std::vector<uint8_t> back(plain.size());
    check(chacha20poly1305_decrypt(back.data(), cipher.data(), cipher.size(), tag, aad.data(),
                                   aad.size(), key.data(), nonce.data()) == 1,
          "déchiffrement authentifié");
    check(std::memcmp(back.data(), plain.data(), plain.size()) == 0, "clair restitué");

    // Un octet modifié doit faire échouer l'authentification.
    cipher[10] ^= 0x01;
    check(chacha20poly1305_decrypt(back.data(), cipher.data(), cipher.size(), tag, aad.data(),
                                   aad.size(), key.data(), nonce.data()) == 0,
          "altération détectée");
}

void test_x25519() {
    section("X25519 (RFC 7748)");

    // §5.2 — multiplication scalaire brute
    {
        const auto scalar =
            unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
        const auto point =
            unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
        std::vector<uint8_t> out(32);
        x25519_scalarmult(out.data(), scalar.data(), point.data());
        check_hex(out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
                  "vecteur 1");
    }
    {
        const auto scalar =
            unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d");
        const auto point =
            unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493");
        std::vector<uint8_t> out(32);
        x25519_scalarmult(out.data(), scalar.data(), point.data());
        check_hex(out, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
                  "vecteur 2");
    }

    // §6.1 — échange complet, c'est exactement ce que fait la poignée de main.
    const auto alice_priv =
        unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto bob_priv =
        unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

    std::vector<uint8_t> alice_pub(32);
    std::vector<uint8_t> bob_pub(32);
    x25519_public_key(alice_pub.data(), alice_priv.data());
    x25519_public_key(bob_pub.data(), bob_priv.data());

    check_hex(alice_pub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
              "clé publique d'Alice");
    check_hex(bob_pub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
              "clé publique de Bob");

    std::vector<uint8_t> shared_a(32);
    std::vector<uint8_t> shared_b(32);
    x25519_scalarmult(shared_a.data(), alice_priv.data(), bob_pub.data());
    x25519_scalarmult(shared_b.data(), bob_priv.data(), alice_pub.data());

    check_hex(shared_a, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
              "secret partagé (côté Alice)");
    check(shared_a == shared_b, "les deux côtés obtiennent le même secret");

    // Point d'ordre faible : le secret doit être nul et donc rejeté.
    const auto low_order = unhex("0000000000000000000000000000000000000000000000000000000000000000");
    std::vector<uint8_t> degenerate(32);
    x25519_scalarmult(degenerate.data(), alice_priv.data(), low_order.data());
    check(x25519_is_zero(degenerate.data()) == 1, "point dégénéré détecté");
}

void test_wireguard() {
    section("WireGuard (Noise IKpsk2)");

    // Constante du protocole : HASH("Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s").
    // Si elle tombe juste, la construction et BLAKE2s sont bons tous les deux.
    std::vector<uint8_t> ck(32);
    wg::initial_chaining_key(ck.data());
    check_hex(ck, "60e26daef327efc02ec335e2a025d2d016eb4206f87277f52d38d1988b78cd36",
              "clé de chaînage initiale");

    // Base64 : c'est sous cette forme que Mullvad échange les clés.
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i * 7 + 1);
    const std::string encoded = wg::key_to_base64(key);
    uint8_t decoded[32];
    check(encoded.size() == 44, "clé encodée sur 44 caractères");
    check(wg::key_from_base64(encoded, decoded), "décodage base64");
    check(std::memcmp(key, decoded, 32) == 0, "aller-retour base64");
    check(!wg::key_from_base64("trop court", decoded), "rejet d'une clé mal formée");

    // Poignée de main complète entre deux instances.
    uint8_t server_priv[32], server_pub[32], client_priv[32], client_pub[32];
    wg::generate_private_key(server_priv);
    wg::derive_public_key(server_pub, server_priv);
    wg::generate_private_key(client_priv);
    wg::derive_public_key(client_pub, client_priv);

    wg::Peer client;
    wg::Peer server;
    client.configure(client_priv, server_pub, nullptr);
    server.configure(server_priv, client_pub, nullptr);

    uint8_t initiation[wg::kInitiationSize];
    check(client.make_initiation(initiation, 1000, 1700000000ULL * 1000000000ULL),
          "message d'initiation produit");
    check(initiation[0] == wg::kTypeInitiation, "type = 1");

    check(server.consume_initiation_for_test(initiation, sizeof(initiation)),
          "initiation acceptée par le serveur");

    uint8_t response[wg::kResponseSize];
    check(server.make_response_for_test(response), "réponse produite");
    check(client.consume_response(response, sizeof(response), 2000),
          "réponse acceptée par le client");
    check(client.established() && server.established(), "tunnel établi des deux côtés");

    // Transfert de données dans les deux sens.
    uint8_t packet[100];
    for (int i = 0; i < 100; ++i) packet[i] = static_cast<uint8_t>(i);

    uint8_t wire[256];
    const int sent = client.encapsulate(packet, sizeof(packet), wire, sizeof(wire), 3000);
    check(sent == static_cast<int>(wg::kDataHeaderSize + sizeof(packet) + wg::kTagLen),
          "taille du message chiffré");

    uint8_t received[256];
    const int got = server.decapsulate(wire, static_cast<size_t>(sent), received,
                                       sizeof(received), 3000);
    check(got == static_cast<int>(sizeof(packet)), "paquet déchiffré côté serveur");
    check(std::memcmp(received, packet, sizeof(packet)) == 0, "contenu intact");

    uint8_t wire_back[256];
    const int sent_back =
        server.encapsulate(packet, sizeof(packet), wire_back, sizeof(wire_back), 3100);
    const int got_back = client.decapsulate(wire_back, static_cast<size_t>(sent_back), received,
                                            sizeof(received), 3100);
    check(got_back == static_cast<int>(sizeof(packet)), "sens retour");

    // Rejeu du tout premier paquet (compteur 0) : doit être refusé.
    check(server.decapsulate(wire, static_cast<size_t>(sent), received, sizeof(received),
                             3200) == -1,
          "rejeu du compteur 0 refusé");

    // Altération d'un octet chiffré.
    wire[wg::kDataHeaderSize + 5] ^= 0x01;
    check(server.decapsulate(wire, static_cast<size_t>(sent), received, sizeof(received),
                             3300) == -1,
          "paquet altéré refusé");

    // Un client configuré avec la mauvaise clé serveur ne doit pas aboutir.
    wg::Peer wrong;
    uint8_t other_priv[32], other_pub[32];
    wg::generate_private_key(other_priv);
    wg::derive_public_key(other_pub, other_priv);
    wrong.configure(client_priv, other_pub, nullptr);

    uint8_t bad_initiation[wg::kInitiationSize];
    wrong.make_initiation(bad_initiation, 4000, 1700000000ULL * 1000000000ULL);

    wg::Peer server2;
    server2.configure(server_priv, client_pub, nullptr);
    check(!server2.consume_initiation_for_test(bad_initiation, sizeof(bad_initiation)),
          "initiation adressée à une autre clé refusée");
}

// ---------------------------------------------------------------------------
// Stockage : le chemin qui a produit « écriture SD impossible » sur console.
// ---------------------------------------------------------------------------

// Fabrique un .torrent en mémoire : deux fichiers, pièces de 32 Ko.
bt::MetaInfo make_test_meta(uint64_t f1, uint64_t f2, uint32_t piece_len,
                            const std::vector<uint8_t>& content) {
    bt::MetaInfo meta;
    meta.name = "essai";
    meta.single_file = false;
    meta.piece_length = piece_len;
    meta.total_size = f1 + f2;
    meta.files.push_back(bt::FileEntry{"a.bin", f1, 0});
    meta.files.push_back(bt::FileEntry{"b.bin", f2, f1});

    const uint32_t pieces = static_cast<uint32_t>((meta.total_size + piece_len - 1) / piece_len);
    for (uint32_t i = 0; i < pieces; ++i) {
        const uint64_t begin = static_cast<uint64_t>(i) * piece_len;
        const uint32_t size =
            static_cast<uint32_t>(std::min<uint64_t>(piece_len, meta.total_size - begin));
        util::Sha1 sha;
        sha.update(content.data() + begin, size);
        meta.piece_hashes.push_back(sha.digest());
    }
    std::memset(meta.info_hash.data(), 0xAB, meta.info_hash.size());
    return meta;
}

void test_storage() {
    section("Stockage");

    const uint32_t piece_len = 32 * 1024;
    const uint64_t f1 = 40 * 1024;  // coupe une pièce en plein milieu
    const uint64_t f2 = 88 * 1024;

    std::vector<uint8_t> content(f1 + f2);
    for (size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
    }

    const bt::MetaInfo meta = make_test_meta(f1, f2, piece_len, content);
    const std::string dir = "/tmp/torfoil-storage-test";
    std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir;
    check(std::system(cmd.c_str()) == 0, "dossier de test préparé");

    bt::Storage storage;
    std::string err;
    check(storage.open(meta, dir, &err), "ouverture : " + (err.empty() ? "ok" : err));

    // On écrit les blocs dans le désordre : c'est ce que font les vrais pairs,
    // et c'est ce qui casse une écriture séquentielle naïve.
    const uint32_t pieces = meta.piece_count();
    const uint32_t block = 16 * 1024;

    std::vector<std::pair<uint32_t, uint32_t>> order;
    for (uint32_t p = 0; p < pieces; ++p) {
        for (uint32_t off = 0; off < meta.size_of_piece(p); off += block) {
            order.emplace_back(p, off);
        }
    }
    // Inversion : dernier bloc en premier.
    std::reverse(order.begin(), order.end());

    bool writes_ok = true;
    for (const auto& [p, off] : order) {
        const uint64_t abs = static_cast<uint64_t>(p) * piece_len + off;
        const uint32_t len =
            std::min<uint32_t>(block, meta.size_of_piece(p) - off);
        if (!storage.write_block(p, off, content.data() + abs, len, &err)) {
            writes_ok = false;
            break;
        }
    }
    check(writes_ok, "écriture de tous les blocs dans le désordre");

    bool verified = true;
    for (uint32_t p = 0; p < pieces; ++p) {
        if (!storage.verify_piece(p)) { verified = false; break; }
        if (!storage.commit_piece(p, &err)) { verified = false; break; }
    }
    check(verified, "chaque pièce vérifiée puis écrite");

    storage.close();

    // Relecture depuis zéro : les octets sur le disque doivent être exacts, y
    // compris à la frontière entre les deux fichiers.
    bt::Storage reopened;
    check(reopened.open(meta, dir, &err), "réouverture");

    std::vector<bool> have;
    reopened.scan_existing(have, nullptr);
    const size_t good = std::count(have.begin(), have.end(), true);
    check(good == pieces, "toutes les pièces relues et validées (" + std::to_string(good) + "/" +
                              std::to_string(pieces) + ")");

    // Une pièce corrompue doit être détectée, pas acceptée en silence.
    std::vector<uint8_t> junk(block, 0x00);
    check(reopened.write_block(0, 0, junk.data(), block, &err), "écriture d'un bloc erroné");
    reopened.commit_piece(0, &err);
    check(!reopened.verify_piece(0), "pièce corrompue rejetée");

    // Écriture hors limites : refusée plutôt que silencieusement tronquée.
    check(!reopened.write_block(pieces + 10, 0, junk.data(), block, &err),
          "pièce hors limites refusée");
    check(!reopened.write_block(0, piece_len - 16, junk.data(), block, &err),
          "bloc débordant de la pièce refusé");

    reopened.close();

    // --- reprise sur un fichier déjà présent ---
    //
    // Le cas qui a fait échouer la console : les fichiers existaient déjà, créés
    // par une version antérieure. Il faut vérifier qu'une réouverture les
    // reconnaît comme réutilisables au lieu de repartir de zéro — ou, à
    // l'inverse, qu'elle sait dire qu'elle a dû les recréer.
    {
        bt::Storage again;
        check(again.open(meta, dir, &err), "troisième ouverture");
        check(!again.created_fresh(), "fichiers existants reconnus (pas de re-création)");
        check(!again.recreated(), "aucune recréation nécessaire ici");
        again.close();
    }

    // Un dossier vidé doit au contraire être vu comme neuf : sans ça on
    // relirait 35 Go pour rien au lancement suivant.
    {
        cmd = "rm -rf " + dir + " && mkdir -p " + dir;
        (void)std::system(cmd.c_str());
        bt::Storage fresh;
        check(fresh.open(meta, dir, &err), "ouverture après effacement");
        check(fresh.created_fresh(), "dossier vide reconnu comme neuf");
        fresh.close();
    }

    cmd = "rm -rf " + dir;
    (void)std::system(cmd.c_str());
}

// Reproduit la limite de FAT32 sur PC.
//
// Impossible d'avoir une carte FAT32 ici, mais on peut mettre le processus dans
// la même situation : RLIMIT_FSIZE fait échouer toute écriture au-delà d'une
// taille donnée, avec EFBIG — exactement l'erreur que renvoie FAT32 à 4 Go.
// C'est donc bien le vrai chemin de code qui s'exécute, pas une simulation.
void test_storage_size_limit() {
    section("Refus du système de fichiers (comportement de FAT32)");

    // On ne peut pas fabriquer une carte FAT32 ici, et RLIMIT_FSIZE n'est pas
    // appliqué sous WSL. On simule donc le seul point qu'on ne maîtrise pas —
    // la réponse du système de fichiers — et on éprouve tout le reste : la
    // détection, la recréation, l'invalidation de la reprise, le message.
    auto refuse_growth = [](const std::string&, uint64_t) { return false; };

    const uint32_t piece_len = 32 * 1024;
    const uint64_t f1 = 512 * 1024;  // deux fois la limite : impossible à écrire
    const uint64_t f2 = 64 * 1024;

    std::vector<uint8_t> content(f1 + f2, 0x5A);
    const bt::MetaInfo meta = make_test_meta(f1, f2, piece_len, content);

    const std::string dir = "/tmp/torfoil-limit-test";
    std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir;
    (void)std::system(cmd.c_str());

    // 1) Fichier absent : la création doit échouer proprement, en nommant la
    //    cause, plutôt que de laisser découvrir le problème à 250 Ko.
    {
        bt::Storage storage;
        storage.set_large_file_threshold(128 * 1024);
        storage.set_growth_probe(refuse_growth);
        std::string err;
        const bool opened = storage.open(meta, dir, &err);
        check(!opened, "fichier trop grand refusé dès l'ouverture");
        check(!opened && err.find("FAT32") != std::string::npos,
              "le message désigne la vraie cause : " + err);
        check(!opened && !storage.recreated(),
              "aucune recréation annoncée pour un fichier qui n'existait pas");
    }

    // 2) LE bug de la console : un fichier ordinaire existe déjà, hérité d'une
    //    version antérieure. Il ne peut pas grandir — il faut le détecter, et
    //    non pas repartir en se disant « il existe, tout va bien ».
    {
        cmd = "rm -rf " + dir + " && mkdir -p " + dir + "/essai";
        (void)std::system(cmd.c_str());
        const std::string victim = dir + "/essai/a.bin";
        std::FILE* fp = std::fopen(victim.c_str(), "wb");
        check(fp != nullptr, "fichier ordinaire préexistant créé");
        if (fp) {
            std::vector<uint8_t> some(100 * 1024, 0x11);
            std::fwrite(some.data(), 1, some.size(), fp);
            std::fclose(fp);
        }

        bt::Storage storage;
        storage.set_large_file_threshold(128 * 1024);
        storage.set_growth_probe(refuse_growth);
        std::string err;
        const bool opened = storage.open(meta, dir, &err);
        check(!opened, "fichier préexistant incapable de grandir : détecté");
        check(!opened && err.find("FAT32") != std::string::npos,
              "cause correctement rapportée pour un fichier déjà présent");
        // LE point du correctif : le fichier existant ne doit PAS être accepté
        // tel quel. Avant, on sortait en se disant « il existe, tout va bien »,
        // et la console échouait ensuite à chaque écriture au-delà de 4 Go.
        check(storage.recreated(), "recréation déclenchée pour un fichier inadapté");
    }

    // 2 bis) Un fichier existant qui PEUT grandir doit être conservé tel quel :
    //        la détection ne doit pas devenir une destruction systématique.
    {
        cmd = "rm -rf " + dir + " && mkdir -p " + dir + "/essai";
        (void)std::system(cmd.c_str());
        std::FILE* fp = std::fopen((dir + "/essai/a.bin").c_str(), "wb");
        if (fp) {
            std::vector<uint8_t> some(100 * 1024, 0x22);
            std::fwrite(some.data(), 1, some.size(), fp);
            std::fclose(fp);
        }

        bt::Storage storage;
        storage.set_large_file_threshold(128 * 1024);
        storage.set_growth_probe([](const std::string&, uint64_t) { return true; });
        std::string err;
        check(storage.open(meta, dir, &err), "fichier existant capable de grandir : accepté");
        check(!storage.recreated(), "aucune destruction inutile");
        check(!storage.created_fresh(), "données existantes reconnues");
        storage.close();
    }

    // 3) Sous la limite, tout doit se dérouler normalement — la protection ne
    //    doit pas devenir un refus généralisé.
    {
        cmd = "rm -rf " + dir + " && mkdir -p " + dir;
        (void)std::system(cmd.c_str());

        const uint64_t s1 = 96 * 1024;
        const uint64_t s2 = 32 * 1024;
        std::vector<uint8_t> small(s1 + s2);
        for (size_t i = 0; i < small.size(); ++i) small[i] = static_cast<uint8_t>(i & 0xff);
        const bt::MetaInfo tiny = make_test_meta(s1, s2, piece_len, small);

        bt::Storage storage;
        storage.set_large_file_threshold(128 * 1024);
        std::string err;
        check(storage.open(tiny, dir, &err), "fichiers sous la limite acceptés");

        bool ok = true;
        for (uint32_t p = 0; p < tiny.piece_count(); ++p) {
            const uint32_t psize = tiny.size_of_piece(p);
            const uint64_t abs = static_cast<uint64_t>(p) * piece_len;
            if (!storage.write_block(p, 0, small.data() + abs, psize, &err) ||
                !storage.verify_piece(p) || !storage.commit_piece(p, &err)) {
                ok = false;
                break;
            }
        }
        check(ok, "écriture et vérification normales sous la limite");
        storage.close();
    }

    cmd = "rm -rf " + dir;
    (void)std::system(cmd.c_str());
}

// La géométrie exacte du torrent qui a posé problème : deux fichiers dont la
// frontière tombe EN PLEIN MILIEU d'une pièce. C'est le cas qui casse une
// écriture naïve — le bloc reçu doit être coupé et réparti sur deux fichiers,
// et la vérification SHA-1 doit ensuite le relire à cheval sur les deux.
void test_storage_multifile() {
    section("Stockage multi-fichiers (frontière au milieu d'une pièce)");

    const uint32_t piece_len = 64 * 1024;
    // 5,5 pièces puis 3,5 pièces : aucune frontière alignée, comme dans le
    // torrent réel où le premier fichier fait 11794,05 pièces.
    const uint64_t f1 = piece_len * 5 + piece_len / 2;
    const uint64_t f2 = piece_len * 3 + piece_len / 2;

    std::vector<uint8_t> content(f1 + f2);
    for (size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<uint8_t>((i * 7 + i / 251) & 0xff);
    }

    const bt::MetaInfo meta = make_test_meta(f1, f2, piece_len, content);
    const std::string dir = "/tmp/torfoil-multifile-test";
    std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir;
    (void)std::system(cmd.c_str());

    bt::Storage storage;
    std::string err;
    check(storage.open(meta, dir, &err), "ouverture : " + (err.empty() ? "ok" : err));

    const uint32_t pieces = meta.piece_count();
    check(pieces == 9, "9 pièces (" + std::to_string(pieces) + ")");

    // La pièce 5 chevauche les deux fichiers : c'est elle qu'on surveille.
    const uint32_t straddling = static_cast<uint32_t>(f1 / piece_len);
    check(static_cast<uint64_t>(straddling) * piece_len < f1 &&
              static_cast<uint64_t>(straddling + 1) * piece_len > f1,
          "la pièce " + std::to_string(straddling) + " est bien à cheval");

    const uint32_t block = 16 * 1024;
    bool writes_ok = true;
    for (uint32_t p = 0; p < pieces && writes_ok; ++p) {
        const uint32_t psize = meta.size_of_piece(p);
        // Ordre inversé dans chaque pièce, pour ne rien devoir à la chance.
        for (int32_t off = static_cast<int32_t>(((psize - 1) / block) * block); off >= 0;
             off -= static_cast<int32_t>(block)) {
            const uint32_t o = static_cast<uint32_t>(off);
            const uint32_t len = std::min<uint32_t>(block, psize - o);
            const uint64_t abs = static_cast<uint64_t>(p) * piece_len + o;
            if (!storage.write_block(p, o, content.data() + abs, len, &err)) {
                writes_ok = false;
                break;
            }
        }
    }
    check(writes_ok, "écriture de toutes les pièces : " + (err.empty() ? "ok" : err));

    bool all_valid = true;
    for (uint32_t p = 0; p < pieces; ++p) {
        if (!storage.verify_piece(p) || !storage.commit_piece(p, &err)) {
            all_valid = false;
            check(false, "pièce " + std::to_string(p) + " invalide : " + err);
            break;
        }
    }
    check(all_valid, "toutes les pièces vérifiées, y compris celle à cheval");
    storage.close();

    // Le contenu réellement écrit doit correspondre octet pour octet — c'est la
    // seule preuve que la répartition sur deux fichiers est exacte.
    auto slurp = [](const std::string& path) {
        std::string out;
        if (std::FILE* fp = std::fopen(path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
            std::fclose(fp);
        }
        return out;
    };

    const std::string a = slurp(dir + "/essai/a.bin");
    const std::string b = slurp(dir + "/essai/b.bin");
    check(a.size() == f1, "premier fichier à la bonne taille (" + std::to_string(a.size()) + ")");
    check(b.size() == f2, "second fichier à la bonne taille (" + std::to_string(b.size()) + ")");
    check(std::memcmp(a.data(), content.data(), a.size()) == 0,
          "octets du premier fichier exacts");
    check(b.size() == f2 && std::memcmp(b.data(), content.data() + f1, b.size()) == 0,
          "octets du second fichier exacts (rien de décalé à la frontière)");

    cmd = "rm -rf " + dir;
    (void)std::system(cmd.c_str());
}

}  // namespace

int main() {
    std::printf("\033[1mTorfoil — tests hôte\033[0m\n");

    test_sha1();
    test_bencode();
    test_torrent();
    test_magnet();
    test_qr();
    test_http_parse();
    test_translations();
    test_bitfield();
    test_picker();
    test_blake2s();
    test_chacha20();
    test_aead();
    test_x25519();
    test_wireguard();
    test_storage();
    test_storage_multifile();
    test_storage_size_limit();

    std::printf("\n\033[1m%d réussis, %d échoués\033[0m\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
