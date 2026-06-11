#pragma once
#include <atomic>

namespace nanodb {

    // A lightweight lock that spins in a loop until it gets access.
    // Much faster than std::mutex for small, quick updates like ours.
    class SpinLock {
    public:
        void lock() {
            // "flag" is false (unlocked). We try to set it to true (locked).
            // memory_order_acquire ensures we see latest data.
            while (flag.test_and_set(std::memory_order_acquire)) {
                // Spin-wait (CPU hint to pause slightly)
                #if defined(_MSC_VER)
                    _mm_pause(); 
                #else
                    __builtin_ia32_pause();
                #endif
            }
        }

        void unlock() {
            // Release the lock
            flag.clear(std::memory_order_release);
        }

    private:
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
    };

} // namespace nanodb