#include "loader.h"

#include <cmath>
#include <limits>

namespace ncvpp {

SliceP readSlice(const Dataset& ds, const SliceKey& key) {
    auto s = std::make_shared<Slice>();
    std::vector<size_t> shape = ds.shape(key.var);
    std::vector<size_t> start = key.pos, count(shape.size(), 1);
    start.resize(shape.size(), 0);
    if (key.xd >= 0) { start[key.xd] = 0; count[key.xd] = shape[key.xd]; }
    if (key.yd >= 0) { start[key.yd] = 0; count[key.yd] = shape[key.yd]; }
    s->nx = key.xd >= 0 ? shape[key.xd] : 1;
    s->ny = key.yd >= 0 ? shape[key.yd] : 1;

    std::vector<float> buf;
    if (!ds.read(key.var, start, count, buf, &s->error)) return s;
    if (key.yd >= 0 && key.xd >= 0 && key.xd < key.yd) {
        // Stored with x outer, y inner: transpose to [y][x] (cache-blocked).
        s->data.resize(buf.size());
        const size_t nx = s->nx, ny = s->ny, B = 64;
        for (size_t x0 = 0; x0 < nx; x0 += B)
            for (size_t y0 = 0; y0 < ny; y0 += B)
                for (size_t x = x0; x < std::min(nx, x0 + B); ++x)
                    for (size_t y = y0; y < std::min(ny, y0 + B); ++y)
                        s->data[y * nx + x] = buf[x * ny + y];
    } else {
        s->data.swap(buf);
    }
    float lo = std::numeric_limits<float>::max(), hi = -lo;
    for (float v : s->data) {
        if (std::isnan(v)) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    s->anyValid = lo <= hi;
    s->vmin = s->anyValid ? lo : 0;
    s->vmax = s->anyValid ? hi : 0;
    return s;
}

Loader::Loader(const Dataset& ds, size_t cacheBytes) : ds_(ds), cap_(cacheBytes) {
    th_ = std::thread([this] { run(); });
}

Loader::~Loader() { shutdown(); }

void Loader::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        q_.clear();
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

SliceP Loader::get(const SliceKey& key) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.second);
    return it->second.first;
}

void Loader::request(const SliceKey& key, bool urgent) {
    std::lock_guard<std::mutex> lk(m_);
    if (cache_.count(key)) return;
    auto p = pending_.find(key);
    if (p != pending_.end()) {
        if (!urgent) return;
        // Promote an already-queued prefetch to the front.
        for (auto it = q_.begin(); it != q_.end(); ++it)
            if (it->isSlice && it->key == key) { Job j = *it; q_.erase(it); j.urgent = true; q_.push_front(j); break; }
        return;
    }
    Job j;
    j.key = key;
    j.isSlice = true;
    j.urgent = urgent;
    pending_[key] = 1;
    if (urgent) q_.push_front(std::move(j));
    else q_.push_back(std::move(j));
    cv_.notify_one();
}

void Loader::dropPrefetch() {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = q_.begin(); it != q_.end();) {
        if (it->isSlice && !it->urgent) { pending_.erase(it->key); it = q_.erase(it); }
        else ++it;
    }
}

void Loader::submit(std::function<void()> job) {
    std::lock_guard<std::mutex> lk(m_);
    Job j;
    j.fn = std::move(job);
    q_.push_back(std::move(j));
    cv_.notify_one();
}

void Loader::insert(const SliceKey& key, SliceP s) {
    size_t sz = s->data.size() * sizeof(float) + 256;
    // Evict least recently used until it fits (always keep the new slice).
    while (!lru_.empty() && bytes_ + sz > cap_) {
        auto it = cache_.find(lru_.back());
        bytes_ -= it->second.first->data.size() * sizeof(float) + 256;
        cache_.erase(it);
        lru_.pop_back();
    }
    lru_.push_front(key);
    cache_[key] = {std::move(s), lru_.begin()};
    bytes_ += sz;
}

void Loader::run() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (stop_) return;
            j = std::move(q_.front());
            q_.pop_front();
            busy_ = true;
        }
        if (j.isSlice) {
            SliceP s = readSlice(ds_, j.key);
            std::lock_guard<std::mutex> lk(m_);
            pending_.erase(j.key);
            if (!cache_.count(j.key)) insert(j.key, std::move(s));
        } else if (j.fn) {
            j.fn();
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            busy_ = !q_.empty();
        }
        if (onDone) onDone();
    }
}

} // namespace ncvpp
