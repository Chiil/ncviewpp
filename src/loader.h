// ncview++ — background I/O thread with an LRU slice cache and prefetching.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dataset.h"

namespace ncvpp {

// Identifies a 2-D (or 1-D when yd < 0) slice of a variable.
struct SliceKey {
    int var = -1;
    int xd = -1, yd = -1;            // positions within the variable's dims
    std::vector<size_t> pos;         // index for every dim (x/y entries ignored, stored as 0)
    bool operator==(const SliceKey& o) const { return var == o.var && xd == o.xd && yd == o.yd && pos == o.pos; }
    bool operator!=(const SliceKey& o) const { return !(*this == o); }
};

struct SliceKeyHash {
    size_t operator()(const SliceKey& k) const {
        size_t h = std::hash<int>()(k.var) ^ (size_t(k.xd + 1) << 20) ^ (size_t(k.yd + 2) << 26);
        for (size_t p : k.pos) h = h * 1000003u ^ std::hash<size_t>()(p);
        return h;
    }
};

struct Slice {
    size_t nx = 0, ny = 0;           // data laid out [y][x]
    std::vector<float> data;         // NaN = missing
    float vmin = 0, vmax = 0;
    bool anyValid = false;
    std::string error;
};
using SliceP = std::shared_ptr<const Slice>;

// Reads a slice synchronously (no cache). Usable from any thread.
SliceP readSlice(const Dataset& ds, const SliceKey& key);

class Loader {
public:
    Loader(const Dataset& ds, size_t cacheBytes);
    ~Loader();

    SliceP get(const SliceKey& key);                 // cached slice or null
    void request(const SliceKey& key, bool urgent);  // queue a load (no-op if cached/pending)
    void dropPrefetch();                             // forget queued non-urgent loads
    void submit(std::function<void()> job);          // run arbitrary job on the I/O thread
    bool busy() const { return busy_.load(); }
    void shutdown();                                 // stop the I/O thread (idempotent)

    std::function<void()> onDone;                    // called on I/O thread after every job

private:
    struct Job { std::function<void()> fn; SliceKey key; bool isSlice = false; bool urgent = false; };
    void run();
    void insert(const SliceKey& key, SliceP s);

    const Dataset& ds_;
    size_t cap_, bytes_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    std::unordered_map<SliceKey, int, SliceKeyHash> pending_;
    std::list<SliceKey> lru_;
    std::unordered_map<SliceKey, std::pair<SliceP, std::list<SliceKey>::iterator>, SliceKeyHash> cache_;
    std::atomic<bool> busy_{false};
    bool stop_ = false;
    std::thread th_;
};

} // namespace ncvpp
