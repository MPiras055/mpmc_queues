#pragma once
#include <numeric>
#include <vector>
#include <sched.h>
#include <thread>
#include <string>
#include <fstream>
#include <cassert>

#ifndef CORE_TOPOLOGY
#define CORE_TOPOLOGY ".sys_topo"
#endif

class ThreadPinner {
public:
    ThreadPinner() {
        bool ok = load_topology(logical_core_list);
        assert(ok && "Failed to load topology");

    }

    /**
     * @brief pins a group of threads
     */
    bool pin_threads(std::vector<std::thread>& threads) const {
        size_t core_len = logical_core_list.size();
        for(size_t i = 0; i < threads.size(); i++) {
            bool ok = bind_thread_to_core(
                threads[i],
                logical_core_list[i % core_len]
            );
            if(!ok)
                return false;
        }
        return true;
    }

    /**
     * @brief pins 2 groups of threads over their ratio
     */
    bool pin_threads(
        std::vector<std::thread>& g1,
        std::vector<std::thread>& g2
    ) const {

        if(g1.size() == 0) {
            return pin_threads(g2);
        } else if (g2.size() == 0) {
            return pin_threads(g1);
        }

        if(g1.size() > g2.size()) {
            std::swap(g1,g2);
        }

        const size_t gcd = std::gcd(g1.size(),g2.size());
        const size_t g1_batch = g1.size() / gcd;
        const size_t g2_batch = g2.size() / gcd;

        size_t cpu_i    = 0;
        size_t g1_i     = 0;
        size_t g2_i     = 0;
        size_t cpus     = logical_core_list.size();

        while(g1_i < g1.size() || g2_i < g2.size()) {
            for(size_t i = 0; i < g1_batch && g1_i < g1.size(); i++, cpu_i++) {
                if(!bind_thread_to_core(
                    g1[g1_i++],
                    logical_core_list[cpu_i % cpus]
                )) return false;
            }

            for(size_t i = 0; i < g2_batch && g2_i < g2.size(); i++,cpu_i++) {
                if(!bind_thread_to_core(
                    g2[g2_i++],
                    logical_core_list[cpu_i % cpus]
                ))  return false;
            }
        }

        return true;
    }

private:
    /**
     * @brief loads the system topology from a predefined file, and reads it
     * mapping it to an ordered list of cores
     */
    static bool load_topology(std::vector<int>& res) {
        std::string topo(CORE_TOPOLOGY);
        std::ifstream input(topo);
        std::vector<int> core_list;
        if(!input.is_open())
            return false;

        std::string line;
        int core;
        while(std::getline(input,line)) {
            if(std::scanf(line.c_str(),"%d",&core) != 1) {
                return false;
            }
            core_list.push_back(core);
        }
        res = core_list;
        return true;
    }

    /**
     * @binds a thread handler to a specified core
     */
    static bool bind_thread_to_core(std::thread& t, int core_id) {
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(core_id,&cpu_set);
        return pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpu_set);
    }

    std::vector<int> logical_core_list;


};
