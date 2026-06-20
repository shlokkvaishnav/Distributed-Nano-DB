#include <iostream>
#include <cassert>
#include <map>
#include <string>
#include "../cluster/hash_ring.hpp"

using namespace nanodb::cluster;

void test_route_deterministic() {
    HashRing ring({0, 1, 2});
    int a = ring.route("vector-1");
    int b = ring.route("vector-1");
    assert(a == b);
    std::cout << "test_route_deterministic passed.\n";
}

void test_load_balance() {
    HashRing ring({0, 1, 2});
    std::map<int, int> counts;
    int n = 20000;
    for (int i = 0; i < n; i++) counts[ring.route("vec-" + std::to_string(i))]++;
    double mean = n / 3.0;
    for (auto& [shard, count] : counts) {
        double ratio = count / mean;
        assert(ratio > 0.8 && ratio < 1.2); // within 20% of perfectly even
    }
    std::cout << "test_load_balance passed.\n";
}

void test_migration_cost_bounded() {
    HashRing ring3({0, 1, 2});
    HashRing ring4({0, 1, 2, 3});
    int n = 20000, moved = 0;
    for (int i = 0; i < n; i++) {
        std::string key = "vec-" + std::to_string(i);
        if (ring3.route(key) != ring4.route(key)) moved++;
    }
    double pct = 100.0 * moved / n;
    // theoretical minimum is 25% (1/4); generous upper bound to avoid flakiness
    assert(pct > 15.0 && pct < 32.0);
    std::cout << "test_migration_cost_bounded passed. (" << pct << "% moved)\n";
}

int main() {
    test_route_deterministic();
    test_load_balance();
    test_migration_cost_bounded();
    std::cout << "All hash ring tests passed.\n";
    return 0;
}
