#include "bt/piece_writer.hpp"

#include "bt/storage.hpp"
#include "util/log.hpp"

namespace bt {

PieceWriter::~PieceWriter() { stop(); }

void PieceWriter::start(Storage& storage) {
    if (running_.load()) return;
    storage_ = &storage;
    running_.store(true);
    thread_ = std::thread([this] { loop(); });
}

void PieceWriter::stop() {
    if (!running_.load()) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    running_.store(false);
    has_work_.notify_all();
    has_room_.notify_all();
    if (thread_.joinable()) thread_.join();

    // Ce qui restait en file n'a pas été écrit. Le point de reprise ne le
    // mentionne pas non plus (il n'est mis à jour qu'après confirmation), donc
    // ces pièces seront simplement redemandées — mais autant le dire.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!jobs_.empty()) {
        util::log_fmt("arrêt : %llu pièce(s) non écrites, elles seront redemandées",
                      static_cast<unsigned long long>(jobs_.size()));
        jobs_.clear();
        queued_bytes_ = 0;
    }
}

void PieceWriter::submit(uint32_t piece, std::vector<uint8_t>&& data) {
    const uint64_t size = data.size();
    std::unique_lock<std::mutex> lock(mutex_);

    // Contre-pression. On n'attend que si la carte a pris un retard sérieux ;
    // en régime normal cette condition est fausse et submit() rend la main
    // immédiatement.
    has_room_.wait(lock, [this, size] {
        return !running_.load() || queued_bytes_ + size <= kQueueBudget || jobs_.empty();
    });
    if (!running_.load()) return;

    Job job;
    job.piece = piece;
    job.data = std::move(data);
    queued_bytes_ += size;
    jobs_.push_back(std::move(job));
    lock.unlock();
    has_work_.notify_one();
}

std::vector<PieceWriter::Result> PieceWriter::take_results() {
    std::vector<Result> out;
    std::lock_guard<std::mutex> lock(results_mutex_);
    out.swap(results_);
    return out;
}

uint64_t PieceWriter::queued_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_bytes_;
}

size_t PieceWriter::queued_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

void PieceWriter::loop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            has_work_.wait(lock, [this] { return !running_.load() || !jobs_.empty(); });
            if (jobs_.empty()) {
                if (!running_.load()) return;
                continue;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            queued_bytes_ -= job.data.size();
        }
        has_room_.notify_all();

        Result result;
        result.piece = job.piece;

        // SHA-1 d'abord : inutile d'user la carte pour des octets faux.
        if (!storage_->hash_matches(job.piece, job.data.data(), job.data.size())) {
            result.hash_failed = true;
            std::lock_guard<std::mutex> lock(results_mutex_);
            results_.push_back(std::move(result));
            continue;
        }

        std::string err;
        result.ok = storage_->write_piece(job.piece, job.data.data(), job.data.size(), &err);
        if (!result.ok) result.err = err;

        std::lock_guard<std::mutex> lock(results_mutex_);
        results_.push_back(std::move(result));
    }
}

}  // namespace bt
