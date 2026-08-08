#pragma once
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#ifndef CORE_TOPOLOGY
/// Overridden by CMake to point at the file the Python topology tool writes.
#define CORE_TOPOLOGY ".sys_topo"
#endif

namespace util::threading {

/**
 * @brief Places benchmark threads on specific logical cores.
 *
 * Where threads land changes what is being measured -- two producers on sibling hyperthreads
 * contend for one core's store buffer, on separate sockets they contend for an interconnect --
 * so the placement is an input to the experiment, not an implementation detail. The order of
 * cores comes from `python/mpmc_bench/topology/`, which emits one logical CPU id per line in
 * the order it wants them filled (cluster-first, ping-pong, and so on).
 *
 * ## Reading the layout
 *
 * The file is authoritative when present. When it is missing or unusable the pinner falls back
 * to the CPUs this process is actually allowed to run on (`sched_getaffinity`), which is the
 * honest default: it respects cpusets, taskset and container limits, and it means pinning works
 * without having run the generator first. `origin()` says which was used, so a run can record
 * whether it measured the intended layout or an ascending fallback.
 *
 * @note It used to `std::abort()` when the file was absent -- inside a `bool`-returning
 *       function whose result the only caller discarded, so `benchmark ... pin` dumped core
 *       rather than reporting anything. Nothing here aborts and nothing prints; failures are
 *       return values.
 *
 * @note Placement is computed by `plan()`, which touches no threads and makes no syscalls.
 *       That is what makes the interleaving rule testable: it is the part that is easy to get
 *       wrong, and it can be checked without a machine of a particular shape.
 */
class ThreadPinner {
public:
    /// Where the core order came from.
    enum class Origin { none, file, affinity };

    /**
     * @brief Which core each thread should get, as indices into `cores()`.
     *
     * Two groups, because a producer/consumer split is the layout that matters: the point is
     * how the two are interleaved, not where either lands on its own.
     */
    struct Layout {
        std::vector<std::size_t> first;
        std::vector<std::size_t> second;
    };

    /// @param topology_file one logical CPU id per line; `#` comments and blanks ignored.
    explicit ThreadPinner(std::string_view topology_file = CORE_TOPOLOGY) {
        if (load_file(topology_file, cores_) && !cores_.empty()) {
            origin_ = Origin::file;
        } else if (load_affinity(cores_) && !cores_.empty()) {
            origin_ = Origin::affinity;
        } else {
            cores_.clear();
            origin_ = Origin::none;
        }
    }

    /// False when no core list could be established; every pin call is then a no-op failure.
    [[nodiscard]] bool ok() const noexcept { return !cores_.empty(); }

    [[nodiscard]] Origin origin() const noexcept { return origin_; }

    [[nodiscard]] std::string_view origin_name() const noexcept {
        switch (origin_) {
        case Origin::file: return "topology file";
        case Origin::affinity: return "process affinity";
        default: return "none";
        }
    }

    [[nodiscard]] const std::vector<int>& cores() const noexcept { return cores_; }

    /// Round-robin one group over the core order.
    [[nodiscard]] bool pin(std::vector<std::thread>& threads) const noexcept {
        if (!ok()) return false;
        for (std::size_t i = 0; i < threads.size(); ++i)
            if (!bind(threads[i], cores_[i % cores_.size()])) return false;
        return true;
    }

    /**
     * @brief Interleave two groups across the cores in proportion to their sizes.
     *
     * Four consumers to two producers gives P C C P C C, so each producer sits next to the
     * consumers draining it rather than all producers bunching onto the first cores.
     *
     * @note Neither vector is modified. The previous version `std::swap`ped the caller's two
     *       vectors so the smaller was first, which left the caller holding its producers in
     *       the variable named for consumers.
     */
    [[nodiscard]] bool pin(std::vector<std::thread>& a, std::vector<std::thread>& b) const noexcept {
        if (!ok()) return false;
        const Layout l = plan(a.size(), b.size(), cores_.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!bind(a[i], cores_[l.first[i]])) return false;
        for (std::size_t i = 0; i < b.size(); ++i)
            if (!bind(b[i], cores_[l.second[i]])) return false;
        return true;
    }

    /**
     * @brief The placement rule on its own: no threads, no syscalls, no machine dependence.
     *
     * Both groups are cut into batches whose sizes are in the same ratio as the groups
     * (`n / gcd(n1, n2)`), and the batches alternate, taking consecutive cores. The smaller
     * group leads, which is what puts a lone producer ahead of the consumers it feeds.
     *
     * @param cores number of usable cores; assignments wrap around it.
     * @return an index into `cores()` for every thread of both groups.
     */
    [[nodiscard]] static Layout plan(std::size_t n1, std::size_t n2, std::size_t cores) {
        Layout out;
        out.first.assign(n1, 0);
        out.second.assign(n2, 0);
        if (cores == 0) return out;

        if (n1 == 0 || n2 == 0) { // only one group: plain round-robin
            auto& only = (n1 == 0) ? out.second : out.first;
            for (std::size_t i = 0; i < only.size(); ++i) only[i] = i % cores;
            return out;
        }

        const bool first_is_smaller = n1 <= n2;
        auto& small = first_is_smaller ? out.first : out.second;
        auto& large = first_is_smaller ? out.second : out.first;

        const std::size_t g = std::gcd(small.size(), large.size());
        const std::size_t small_batch = small.size() / g; // both are >= 1, so the loop below
        const std::size_t large_batch = large.size() / g; // always makes progress
        std::size_t cpu = 0, si = 0, li = 0;

        while (si < small.size() || li < large.size()) {
            for (std::size_t k = 0; k < small_batch && si < small.size(); ++k)
                small[si++] = cpu++ % cores;
            for (std::size_t k = 0; k < large_batch && li < large.size(); ++k)
                large[li++] = cpu++ % cores;
        }
        return out;
    }

    /**
     * @brief Parse a topology listing. Exposed so the parser can be tested directly.
     *
     * Ignores blank lines and `#` comments, drops ids that no `cpu_set_t` could hold, and
     * drops repeats while keeping first-seen order -- a duplicate would otherwise silently
     * shift every later thread onto the wrong core.
     *
     * @return false if the file cannot be opened, or if any line is present but unparseable.
     */
    static bool parse_topology(std::istream& in, std::vector<int>& out) {
        std::vector<int> cores;
        std::string line;
        while (std::getline(in, line)) {
            std::string_view sv{line};
            while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
            while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
                sv.remove_suffix(1);
            if (sv.empty() || sv.front() == '#') continue;

            int id = 0;
            const auto* end = sv.data() + sv.size();
            const auto [p, ec] = std::from_chars(sv.data(), end, id);
            if (ec != std::errc{} || p != end) return false; // a malformed line is an error,
                                                             // not something to skip past
            if (!representable(id)) continue;
            if (std::find(cores.begin(), cores.end(), id) == cores.end()) cores.push_back(id);
        }
        out = std::move(cores);
        return true;
    }

    /// True if @p id can be held by a cpu_set_t; CPU_SET on anything else is undefined.
    static constexpr bool representable(int id) noexcept {
#if defined(__linux__)
        return id >= 0 && id < static_cast<int>(CPU_SETSIZE);
#else
        return id >= 0;
#endif
    }

private:
    static bool load_file(std::string_view path, std::vector<int>& out) {
        std::ifstream in{std::string{path}};
        if (!in.is_open()) return false;
        return parse_topology(in, out);
    }

    /**
     * @brief The CPUs this process may actually run on, ascending.
     *
     * Respects cpusets, taskset and container limits, so it is a better fallback than
     * `0..hardware_concurrency()`: pinning to a core the process is not allowed to use would
     * fail, and pinning every thread to a core it happens to share would quietly change the
     * measurement.
     */
    static bool load_affinity(std::vector<int>& out) {
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) != 0) return false;
        out.clear();
        for (int i = 0; i < static_cast<int>(CPU_SETSIZE); ++i)
            if (CPU_ISSET(i, &set)) out.push_back(i);
        return !out.empty();
#else
        out.clear();
        return false; // no portable equivalent; ok() reports it rather than failing to build
#endif
    }

    static bool bind(std::thread& t, int core_id) noexcept {
#if defined(__linux__)
        if (!representable(core_id)) return false;
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core_id, &set);
        return pthread_setaffinity_np(t.native_handle(), sizeof(set), &set) == 0;
#else
        (void)t;
        (void)core_id;
        return false;
#endif
    }

    std::vector<int> cores_;
    Origin origin_ = Origin::none;
};

} // namespace util::threading
