#pragma once

#include <string>
#include <cstddef>
#include <stdexcept>

namespace nanodb {

    class MMapHandler {
    public:
        MMapHandler();
        ~MMapHandler();

        // Map file to memory (creates if missing) with initial size
        void open_file(const std::string& filepath, size_t min_size);

        // Sync data to disk and release resources
        void close_file();

        // Expand file size (Warning: Invalidates existing pointers)
        void resize(size_t new_size);

        // Get raw pointer to the start of the memory block
        void* get_data() const;

        // Get current file size
        size_t get_size() const;

    private:
        std::string file_path_;
        size_t file_size_;
        void* data_; // Base pointer to memory-mapped region

        // OS-Specific Handles
#ifdef _WIN32
        void* file_handle_;
        void* map_handle_;
#else
        int file_fd_;
#endif
    };

} // namespace nanodb