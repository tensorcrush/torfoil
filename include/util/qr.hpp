// Générateur de codes QR, mode octet, correction de niveau M.
//
// Il n'y a qu'un usage ici : afficher à l'écran l'adresse du petit serveur web
// local pour que l'appareil photo d'un téléphone l'ouvre sans rien taper. Une
// URL de réseau local tient en une trentaine de caractères, donc les versions 1
// à 6 (106 octets) suffisent largement — et s'arrêter à la version 6 évite le
// bloc d'information de version, qui n'apparaît qu'à partir de la 7.
//
// Aucune dépendance : compilable et testable sur PC comme sur la console.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace util {

struct QrCode {
    int version = 0;               // 1 à 6
    int size = 0;                  // côté en modules (17 + 4 × version)
    std::vector<uint8_t> modules;  // size × size, 1 = module sombre

    bool at(int x, int y) const {
        if (x < 0 || y < 0 || x >= size || y >= size) return false;
        return modules[static_cast<size_t>(y) * size + x] != 0;
    }
};

// Encode `text` en octets bruts (UTF-8 tel quel). Renvoie false au-delà de
// 106 octets, ce qu'aucune URL de réseau local n'atteint.
bool qr_encode(const std::string& text, QrCode& out);

// Codewords de correction Reed-Solomon d'un bloc. Exposé pour les tests : c'est
// la partie qu'on ne peut pas valider à l'œil, et il existe des vecteurs de
// référence publiés pour elle.
std::vector<uint8_t> qr_reed_solomon(const std::vector<uint8_t>& data, int ec_len);

}  // namespace util
