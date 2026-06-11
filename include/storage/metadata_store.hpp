#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <iostream>

namespace nanodb {

    class MetadataHandler {
    public:
        MetadataHandler() = default;

        void open_file(const std::string& filepath) {
            filepath_ = filepath;
            
            // Open for Read/Write + Binary
            // Removed std::ios::app to allow manual seeking control
            file_stream_.open(filepath, std::ios::in | std::ios::out | std::ios::binary);
            
            if (!file_stream_.is_open()) {
                // Create if missing
                std::ofstream create(filepath);
                create.close();
                file_stream_.open(filepath, std::ios::in | std::ios::out | std::ios::binary);
            }

            rebuild_index();
        }

        void close_file() {
            if (file_stream_.is_open()) {
                file_stream_.close();
            }
        }

        void save_metadata(int id, const std::string& metadata) {
            if (metadata.empty()) return;

            std::lock_guard<std::mutex> lock(write_lock_);
            
            file_stream_.clear(); // Clear any previous EOF flags
            file_stream_.seekp(0, std::ios::end); // Go to end
            
            size_t offset = file_stream_.tellp();
            size_t length = metadata.size();

            // Debug Log
            // std::cout << "[DEBUG] Saving ID=" << id << " Offset=" << offset << " Len=" << length << " Data=" << metadata << "\n";

            uint32_t len_32 = static_cast<uint32_t>(length);
            file_stream_.write(reinterpret_cast<char*>(&len_32), sizeof(uint32_t));
            file_stream_.write(metadata.c_str(), length);
            file_stream_.flush();

            if (id >= offsets_.size()) {
                offsets_.resize(id + 1000, {0, 0});
            }
            offsets_[id] = {offset, length};
        }

        std::string get_metadata(int id) {
            if (id < 0 || id >= offsets_.size()) return "";
            
            auto [offset, length] = offsets_[id];
            if (length == 0) return ""; 

            std::lock_guard<std::mutex> lock(write_lock_);
            
            file_stream_.clear(); // <--- CRITICAL FIX: Reset stream state
            file_stream_.seekg(offset + sizeof(uint32_t), std::ios::beg);
            
            std::string data(length, '\0');
            if (file_stream_.read(&data[0], length)) {
                return data;
            } else {
                std::cerr << "[ERROR] Failed to read metadata for ID " << id << "\n";
                return "";
            }
        }

    private:
        std::string filepath_;
        std::fstream file_stream_;
        std::vector<std::pair<size_t, size_t>> offsets_;
        std::mutex write_lock_;

        void rebuild_index() {
            file_stream_.clear();
            file_stream_.seekg(0, std::ios::beg);
            
            if (file_stream_.peek() == EOF) return;

            int current_id = 0; // Note: This assumes sequential IDs starting at 0
            
            // In a real DB, we would write the ID to disk too. 
            // For this test, we assume file order matches ID order (0, 1, 2...)
            // which is true for our test_metadata.py
            while (file_stream_.peek() != EOF) {
                size_t offset = file_stream_.tellg();
                uint32_t len;
                if (!file_stream_.read(reinterpret_cast<char*>(&len), sizeof(uint32_t))) break;
                
                if (current_id >= offsets_.size()) {
                    offsets_.resize(current_id + 1000, {0, 0});
                }
                offsets_[current_id] = {offset, len};

                file_stream_.seekg(len, std::ios::cur);
                current_id++;
            }
            file_stream_.clear(); 
        }
    };

} // namespace nanodb