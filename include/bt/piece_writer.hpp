// Vérification SHA-1 et écriture des pièces, hors du fil réseau.
//
// C'est le point d'architecture qui décide du débit sur console. Hacher 2 Mio
// puis les écrire sur une carte SD coûte des dizaines de millisecondes ; tant
// que ce travail restait dans la boucle moteur, personne ne lisait les sockets
// pendant ce temps. Elles débordaient, TCP interprétait ces pertes comme de la
// congestion et refermait sa fenêtre — un débit de quelques centaines de Ko/s
// sur une ligne qui en accepte des milliers.
//
// Le remède est celui qu'emploient les installeurs Switch qui vont vite : deux
// fils et un tampon entre eux. Le réseau dépose, le disque retire, aucun des
// deux n'attend l'autre.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bt {

class Storage;

class PieceWriter {
public:
    struct Result {
        uint32_t piece = 0;
        bool ok = false;          // écrite et vérifiée
        bool hash_failed = false; // SHA-1 faux : pièce à redemander, pas une panne
        std::string err;          // renseigné seulement en cas de panne d'écriture
    };

    PieceWriter() = default;
    ~PieceWriter();

    PieceWriter(const PieceWriter&) = delete;
    PieceWriter& operator=(const PieceWriter&) = delete;

    void start(Storage& storage);
    // Termine le travail en attente puis arrête le fil. Rien n'est perdu.
    void stop();
    bool running() const { return running_.load(); }

    // Confie une pièce complète. Bloque si le budget mémoire est dépassé —
    // c'est la contre-pression : quand la carte ne suit plus, il faut bien que
    // le réseau ralentisse, mais c'est le seul cas où il attend.
    void submit(uint32_t piece, std::vector<uint8_t>&& data);

    std::vector<Result> take_results();

    uint64_t queued_bytes() const;
    size_t queued_count() const;

private:
    void loop();

    struct Job {
        uint32_t piece = 0;
        std::vector<uint8_t> data;
    };

    Storage* storage_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    std::condition_variable has_work_;
    std::condition_variable has_room_;
    std::deque<Job> jobs_;
    uint64_t queued_bytes_ = 0;

    std::mutex results_mutex_;
    std::vector<Result> results_;

    // Assez pour absorber plusieurs secondes de réseau à pleine vitesse sans
    // jamais faire attendre le fil moteur. Un mode applet réduirait ce chiffre,
    // mais l'application dispose de la mémoire complète de la console.
    static constexpr uint64_t kQueueBudget = 96ull * 1024 * 1024;
};

}  // namespace bt
