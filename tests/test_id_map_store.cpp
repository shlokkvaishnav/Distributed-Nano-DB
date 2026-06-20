#include <iostream>
#include <string>
#include <cassert>
#include <filesystem>
#include "../cluster/id_map_store.hpp"

using namespace nanodb::cluster;
namespace fs = std::filesystem;

void test_assign_lookup() {
    std::string path = "test_id_map.bin";
    if (fs::exists(path)) fs::remove(path);
    
    IdMapStore store;
    store.open_file(path);
    
    // Assign new
    auto [id1, is_new1] = store.assign("ext_1");
    assert(is_new1 == true);
    assert(id1 == 0);
    
    auto [id2, is_new2] = store.assign("ext_2");
    assert(is_new2 == true);
    assert(id2 == 1);
    
    // Assign existing
    auto [id1_again, is_new1_again] = store.assign("ext_1");
    assert(is_new1_again == false);
    assert(id1_again == 0);
    
    // Lookup
    uint32_t out_id;
    assert(store.lookup("ext_2", out_id) == true);
    assert(out_id == 1);
    assert(store.lookup("ext_3", out_id) == false);
    
    // Reverse lookup
    assert(store.reverse_lookup(0) == "ext_1");
    assert(store.reverse_lookup(1) == "ext_2");
    assert(store.reverse_lookup(99) == "");
    
    store.close_file();
    fs::remove(path);
    std::cout << "test_assign_lookup passed.\n";
}

void test_restart_reload() {
    std::string path = "test_id_map_reload.bin";
    if (fs::exists(path)) fs::remove(path);
    
    {
        IdMapStore store;
        store.open_file(path);
        store.assign("alpha");
        store.assign("beta");
        store.assign("gamma");
        store.close_file();
    }
    
    {
        IdMapStore store2;
        store2.open_file(path);
        assert(store2.size() == 3);
        
        uint32_t out_id;
        assert(store2.lookup("alpha", out_id)); assert(out_id == 0);
        assert(store2.lookup("beta", out_id)); assert(out_id == 1);
        assert(store2.lookup("gamma", out_id)); assert(out_id == 2);
        
        assert(store2.reverse_lookup(1) == "beta");
        store2.close_file();
    }
    
    fs::remove(path);
    std::cout << "test_restart_reload passed.\n";
}

int main() {
    test_assign_lookup();
    test_restart_reload();
    std::cout << "All id_map_store tests passed.\n";
    return 0;
}
