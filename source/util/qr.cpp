#include "util/qr.hpp"

#include <algorithm>
#include <cstring>

namespace util {

namespace {

// ---------------------------------------------------------------------------
// Corps de Galois GF(256), polynôme primitif 0x11D (celui imposé par la norme)
// ---------------------------------------------------------------------------

struct GaloisTables {
    uint8_t exp[512];
    uint8_t log[256];

    GaloisTables() {
        std::memset(log, 0, sizeof(log));
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11d;
        }
        // Doublé pour que exp[a + b] n'ait jamais besoin d'un modulo.
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
};

const GaloisTables& gf() {
    static const GaloisTables tables;
    return tables;
}

uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    const GaloisTables& t = gf();
    return t.exp[static_cast<int>(t.log[a]) + static_cast<int>(t.log[b])];
}

// g(x) = (x - α⁰)(x - α¹)…(x - α^(n-1))
std::vector<uint8_t> generator_poly(int ec_len) {
    std::vector<uint8_t> poly{1};
    const GaloisTables& t = gf();
    for (int i = 0; i < ec_len; ++i) {
        // Multiplication par (x - α^i).
        poly.push_back(0);
        for (size_t j = poly.size() - 1; j > 0; --j) {
            poly[j] ^= gf_mul(poly[j - 1], t.exp[i]);
        }
    }
    return poly;
}

// ---------------------------------------------------------------------------
// Table des versions, niveau de correction M
//
// Arrêt volontaire à la version 6 : jusque-là tous les blocs d'une version ont
// la même taille, ce qui supprime tout le cas « deux groupes de blocs » de la
// norme. Le gain de code est net et la capacité (106 octets) dépasse de loin
// « http://192.168.1.100:8080/ ».
// ---------------------------------------------------------------------------

struct VersionSpec {
    int total_codewords;
    int ec_per_block;
    int blocks;
};

constexpr VersionSpec kVersions[6] = {
    {26, 10, 1},   // 1-M : 16 octets de données
    {44, 16, 1},   // 2-M : 28
    {70, 26, 1},   // 3-M : 44
    {100, 18, 2},  // 4-M : 64
    {134, 24, 2},  // 5-M : 86
    {172, 16, 4},  // 6-M : 108
};

constexpr int kMaxVersion = 6;

int data_codewords(int version) {
    const VersionSpec& v = kVersions[version - 1];
    return v.total_codewords - v.ec_per_block * v.blocks;
}

// En-tête = 4 bits de mode + 8 bits de longueur (mode octet, versions 1 à 9).
int byte_capacity(int version) { return (data_codewords(version) * 8 - 12) / 8; }

// ---------------------------------------------------------------------------
// Matrice
// ---------------------------------------------------------------------------

class Matrix {
public:
    explicit Matrix(int version)
        : version_(version),
          size_(17 + 4 * version),
          modules_(static_cast<size_t>(size_) * size_, 0),
          function_(static_cast<size_t>(size_) * size_, 0) {}

    int size() const { return size_; }
    bool get(int x, int y) const { return modules_[idx(x, y)] != 0; }
    bool is_function(int x, int y) const { return function_[idx(x, y)] != 0; }

    void set_function(int x, int y, bool dark) {
        if (x < 0 || y < 0 || x >= size_ || y >= size_) return;
        modules_[idx(x, y)] = dark ? 1 : 0;
        function_[idx(x, y)] = 1;
    }

    void set_data(int x, int y, bool dark) { modules_[idx(x, y)] = dark ? 1 : 0; }
    void flip(int x, int y) { modules_[idx(x, y)] ^= 1; }

    void draw_function_patterns() {
        // Motifs de synchronisation : posés en premier, les motifs de repère
        // les recouvrent ensuite là où ils se chevauchent.
        for (int i = 0; i < size_; ++i) {
            set_function(6, i, i % 2 == 0);
            set_function(i, 6, i % 2 == 0);
        }

        draw_finder(3, 3);
        draw_finder(size_ - 4, 3);
        draw_finder(3, size_ - 4);

        // Versions 2 à 6 : un unique motif d'alignement, au centre bas-droite.
        if (version_ >= 2) {
            const int c = 4 * version_ + 10;
            draw_alignment(c, c);
        }

        // Réservation des emplacements de l'information de format. On y écrit
        // une valeur provisoire plutôt que de balayer une zone rectangulaire :
        // les deux zones se referment exactement sur les modules concernés, et
        // un balayage écraserait au passage deux modules de synchronisation —
        // ceux en (6,8) et (8,6), que la norme laisse justement intacts.
        draw_format(0);
    }

    // Renvoie le nombre de modules libres rencontrés — donc la capacité réelle
    // de la zone de données telle que la produit ce code. L'appelant la compare
    // à ce que dit la norme : c'est la seule façon simple de s'apercevoir qu'un
    // motif de service est mal posé, faute de quoi le code sort décalé d'un
    // module et aucun téléphone ne le lit, sans le moindre message d'erreur.
    size_t draw_codewords(const std::vector<uint8_t>& data) {
        size_t bit = 0;
        size_t free_modules = 0;
        const size_t total_bits = data.size() * 8;

        for (int right = size_ - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;  // la colonne 6 est celle de synchronisation
            for (int vert = 0; vert < size_; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size_ - 1 - vert : vert;
                    if (is_function(x, y)) continue;
                    ++free_modules;
                    if (bit >= total_bits) continue;
                    const uint8_t byte = data[bit >> 3];
                    set_data(x, y, (byte >> (7 - (bit & 7))) & 1);
                    ++bit;
                }
            }
        }
        // Les bits restants (bits de remplissage de la norme) restent clairs.
        return free_modules;
    }

    void apply_mask(int mask) {
        for (int y = 0; y < size_; ++y) {
            for (int x = 0; x < size_; ++x) {
                if (is_function(x, y)) continue;
                if (mask_condition(mask, x, y)) flip(x, y);
            }
        }
    }

    void draw_format(int mask) {
        // Niveau M = 0b00, puis les 3 bits de masque, puis BCH(15,5).
        const int data = (0 << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const int bits = ((data << 10) | rem) ^ 0x5412;

        auto bit_at = [&](int i) { return ((bits >> i) & 1) != 0; };

        for (int i = 0; i <= 5; ++i) set_function(8, i, bit_at(i));
        set_function(8, 7, bit_at(6));
        set_function(8, 8, bit_at(7));
        set_function(7, 8, bit_at(8));
        for (int i = 9; i < 15; ++i) set_function(14 - i, 8, bit_at(i));

        for (int i = 0; i < 8; ++i) set_function(size_ - 1 - i, 8, bit_at(i));
        for (int i = 8; i < 15; ++i) set_function(8, size_ - 15 + i, bit_at(i));
        set_function(8, size_ - 8, true);
    }

    // Somme des quatre pénalités de la norme. Le masque retenu est celui qui la
    // minimise : c'est ce qui évite les grandes plages uniformes et les motifs
    // qu'un décodeur confondrait avec un repère.
    int penalty() const {
        int score = 0;

        // Règle 1 : suites de 5 modules identiques ou plus.
        for (int y = 0; y < size_; ++y) {
            int run = 1;
            for (int x = 1; x < size_; ++x) {
                if (get(x, y) == get(x - 1, y)) {
                    ++run;
                    if (run == 5) score += 3;
                    else if (run > 5) score += 1;
                } else {
                    run = 1;
                }
            }
        }
        for (int x = 0; x < size_; ++x) {
            int run = 1;
            for (int y = 1; y < size_; ++y) {
                if (get(x, y) == get(x, y - 1)) {
                    ++run;
                    if (run == 5) score += 3;
                    else if (run > 5) score += 1;
                } else {
                    run = 1;
                }
            }
        }

        // Règle 2 : blocs 2×2 de même couleur.
        for (int y = 0; y + 1 < size_; ++y) {
            for (int x = 0; x + 1 < size_; ++x) {
                const bool c = get(x, y);
                if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1)) {
                    score += 3;
                }
            }
        }

        // Règle 3 : motif 1:1:3:1:1 suivi ou précédé de 4 modules clairs, qu'un
        // décodeur prendrait pour un motif de repère.
        static const bool kPattern[7] = {true, false, true, true, true, false, true};
        for (int y = 0; y < size_; ++y) {
            for (int x = 0; x + 6 < size_; ++x) {
                bool match = true;
                for (int k = 0; k < 7 && match; ++k) match = get(x + k, y) == kPattern[k];
                if (!match) continue;
                if (clear_run_h(x - 4, y, 4) || clear_run_h(x + 7, y, 4)) score += 40;
            }
        }
        for (int x = 0; x < size_; ++x) {
            for (int y = 0; y + 6 < size_; ++y) {
                bool match = true;
                for (int k = 0; k < 7 && match; ++k) match = get(x, y + k) == kPattern[k];
                if (!match) continue;
                if (clear_run_v(x, y - 4, 4) || clear_run_v(x, y + 7, 4)) score += 40;
            }
        }

        // Règle 4 : déséquilibre entre modules sombres et clairs.
        int dark = 0;
        for (uint8_t m : modules_) dark += m;
        const int total = size_ * size_;
        const int percent = dark * 100 / total;
        const int deviation = std::abs(percent - 50) / 5;
        score += deviation * 10;

        return score;
    }

private:
    size_t idx(int x, int y) const { return static_cast<size_t>(y) * size_ + x; }

    // Une plage hors matrice compte comme claire : c'est ce que fait la norme,
    // et l'ignorer sous-estime la pénalité près des bords.
    bool clear_run_h(int x, int y, int len) const {
        for (int k = 0; k < len; ++k) {
            const int xx = x + k;
            if (xx < 0 || xx >= size_) continue;
            if (get(xx, y)) return false;
        }
        return true;
    }

    bool clear_run_v(int x, int y, int len) const {
        for (int k = 0; k < len; ++k) {
            const int yy = y + k;
            if (yy < 0 || yy >= size_) continue;
            if (get(x, yy)) return false;
        }
        return true;
    }

    void draw_finder(int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int dist = std::max(std::abs(dx), std::abs(dy));
                set_function(cx + dx, cy + dy, dist != 2 && dist != 4);
            }
        }
    }

    void draw_alignment(int cx, int cy) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                set_function(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
            }
        }
    }

    static bool mask_condition(int mask, int x, int y) {
        switch (mask) {
            case 0: return (x + y) % 2 == 0;
            case 1: return y % 2 == 0;
            case 2: return x % 3 == 0;
            case 3: return (x + y) % 3 == 0;
            case 4: return (y / 2 + x / 3) % 2 == 0;
            case 5: return (x * y) % 2 + (x * y) % 3 == 0;
            case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
            default: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
        }
    }

    int version_;
    int size_;
    std::vector<uint8_t> modules_;
    std::vector<uint8_t> function_;
};

}  // namespace

std::vector<uint8_t> qr_reed_solomon(const std::vector<uint8_t>& data, int ec_len) {
    if (ec_len <= 0) return {};

    const std::vector<uint8_t> gen = generator_poly(ec_len);
    std::vector<uint8_t> rem(static_cast<size_t>(ec_len), 0);

    for (uint8_t byte : data) {
        const uint8_t factor = byte ^ rem[0];
        rem.erase(rem.begin());
        rem.push_back(0);
        for (size_t i = 0; i < rem.size(); ++i) {
            // gen[0] vaut toujours 1 ; on saute donc le premier coefficient.
            rem[i] ^= gf_mul(gen[i + 1], factor);
        }
    }
    return rem;
}

bool qr_encode(const std::string& text, QrCode& out) {
    int version = 0;
    for (int v = 1; v <= kMaxVersion; ++v) {
        if (static_cast<int>(text.size()) <= byte_capacity(v)) {
            version = v;
            break;
        }
    }
    if (version == 0) return false;

    const VersionSpec& spec = kVersions[version - 1];
    const int total_data = data_codewords(version);

    // --- flux de bits : mode octet, longueur, données, terminateur, remplissage
    std::vector<uint8_t> stream;
    stream.reserve(static_cast<size_t>(total_data));
    {
        std::vector<bool> bits;
        bits.reserve(static_cast<size_t>(total_data) * 8);
        auto push = [&](uint32_t value, int width) {
            for (int i = width - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
        };

        push(0b0100, 4);                                          // mode octet
        push(static_cast<uint32_t>(text.size()), 8);              // longueur
        for (unsigned char c : text) push(c, 8);

        const size_t capacity_bits = static_cast<size_t>(total_data) * 8;
        for (int i = 0; i < 4 && bits.size() < capacity_bits; ++i) bits.push_back(false);
        while (bits.size() % 8 != 0) bits.push_back(false);

        for (size_t i = 0; i < bits.size(); i += 8) {
            uint8_t byte = 0;
            for (int b = 0; b < 8; ++b) byte = static_cast<uint8_t>(byte << 1 | (bits[i + b] ? 1 : 0));
            stream.push_back(byte);
        }
        // Octets de remplissage alternés, imposés par la norme.
        bool alternate = false;
        while (static_cast<int>(stream.size()) < total_data) {
            stream.push_back(alternate ? 0x11 : 0xec);
            alternate = !alternate;
        }
    }

    // --- blocs, correction d'erreur, entrelacement
    const int per_block = total_data / spec.blocks;
    std::vector<std::vector<uint8_t>> data_blocks;
    std::vector<std::vector<uint8_t>> ec_blocks;
    data_blocks.reserve(static_cast<size_t>(spec.blocks));
    ec_blocks.reserve(static_cast<size_t>(spec.blocks));

    for (int b = 0; b < spec.blocks; ++b) {
        std::vector<uint8_t> block(stream.begin() + static_cast<long>(b) * per_block,
                                   stream.begin() + static_cast<long>(b + 1) * per_block);
        ec_blocks.push_back(qr_reed_solomon(block, spec.ec_per_block));
        data_blocks.push_back(std::move(block));
    }

    std::vector<uint8_t> final_bytes;
    final_bytes.reserve(static_cast<size_t>(spec.total_codewords));
    for (int i = 0; i < per_block; ++i) {
        for (const auto& block : data_blocks) final_bytes.push_back(block[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < spec.ec_per_block; ++i) {
        for (const auto& block : ec_blocks) final_bytes.push_back(block[static_cast<size_t>(i)]);
    }

    // Bits de remplissage de fin de zone : aucun en version 1, sept de la 2 à
    // la 6 (table 1 de la norme). La somme avec les codewords donne le nombre
    // exact de modules que la zone de données doit offrir.
    const size_t expected_free =
        static_cast<size_t>(spec.total_codewords) * 8 + (version == 1 ? 0 : 7);

    // --- matrice, puis choix du masque le moins pénalisé
    Matrix best(version);
    int best_score = -1;

    for (int mask = 0; mask < 8; ++mask) {
        Matrix m(version);
        m.draw_function_patterns();
        if (m.draw_codewords(final_bytes) != expected_free) return false;
        m.apply_mask(mask);
        m.draw_format(mask);

        const int score = m.penalty();
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best = m;
        }
    }

    out.version = version;
    out.size = best.size();
    out.modules.assign(static_cast<size_t>(out.size) * out.size, 0);
    for (int y = 0; y < out.size; ++y) {
        for (int x = 0; x < out.size; ++x) {
            out.modules[static_cast<size_t>(y) * out.size + x] = best.get(x, y) ? 1 : 0;
        }
    }
    return true;
}

}  // namespace util
