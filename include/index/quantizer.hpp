#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include "../common/types.hpp"

namespace nanodb {

    // -------------------------------------------------------------------------
    // ScalarQuantizer: int8 Scalar Quantization
    //
    // Converts float32 vectors to int8 (signed byte) representation.
    // This achieves a 4x memory reduction (32-bit → 8-bit per component) with
    // typically 1–5% recall loss at equivalent ef_search settings.
    //
    // Usage pattern:
    //   1. Call train() on a representative sample of your dataset.
    //   2. Use quantize() before inserting into a compressed index.
    //   3. Use dequantize() to recover approximate float vectors for re-ranking.
    //   4. Use quantize_distance() for fast approximate distance on int8 data.
    //
    // Memory comparison (128d vectors):
    //   float32: 128 * 4 = 512 bytes per vector
    //   int8:    128 * 1 = 128 bytes per vector  (4x reduction)
    // -------------------------------------------------------------------------
    class ScalarQuantizer {
    public:
        ScalarQuantizer() = default;

        // Train the quantizer on a set of vectors.
        // Computes per-dimension min and max for the quantization range.
        // Call this before quantize() or dequantize().
        void train(const std::vector<Vector>& vecs) {
            if (vecs.empty()) return;
            dim_ = vecs[0].size();
            min_.assign(dim_, std::numeric_limits<float>::max());
            max_.assign(dim_, std::numeric_limits<float>::lowest());

            for (const auto& v : vecs) {
                for (size_t i = 0; i < dim_; ++i) {
                    if (v[i] < min_[i]) min_[i] = v[i];
                    if (v[i] > max_[i]) max_[i] = v[i];
                }
            }
            trained_ = true;
        }

        // Quantize a float32 vector to int8.
        // Maps each dimension from [min_i, max_i] → [-127, 127].
        // Output buffer must be pre-allocated to at least dim_ bytes.
        void quantize(const float* in, int8_t* out, size_t dim) const {
            check_trained();
            for (size_t i = 0; i < dim; ++i) {
                float range = max_[i] - min_[i];
                float normalized = (range > 1e-9f) ? ((in[i] - min_[i]) / range) : 0.0f;
                // Map [0, 1] → [-127, 127]
                out[i] = static_cast<int8_t>(std::clamp(normalized * 254.0f - 127.0f, -127.0f, 127.0f));
            }
        }

        // Convenience overload returning a vector.
        std::vector<int8_t> quantize(const float* in, size_t dim) const {
            std::vector<int8_t> out(dim);
            quantize(in, out.data(), dim);
            return out;
        }

        // Dequantize an int8 vector back to float32 (approximate reconstruction).
        // Useful for re-ranking a shortlist of candidates.
        void dequantize(const int8_t* in, float* out, size_t dim) const {
            check_trained();
            for (size_t i = 0; i < dim; ++i) {
                float range = max_[i] - min_[i];
                float normalized = (static_cast<float>(in[i]) + 127.0f) / 254.0f;
                out[i] = min_[i] + normalized * range;
            }
        }

        // Fast approximate L2 distance between two int8 quantized vectors.
        // Uses integer arithmetic — no floating point, no SIMD needed.
        // Suitable for pre-filtering large candidate sets before exact re-ranking.
        int32_t quantize_distance(const int8_t* a, const int8_t* b, size_t dim) const {
            int32_t sum = 0;
            for (size_t i = 0; i < dim; ++i) {
                int32_t diff = static_cast<int32_t>(a[i]) - static_cast<int32_t>(b[i]);
                sum += diff * diff;
            }
            return sum;
        }

        bool is_trained() const { return trained_; }
        size_t dim() const { return dim_; }

        // Access per-dimension range (useful for serialization)
        const std::vector<float>& min_vals() const { return min_; }
        const std::vector<float>& max_vals() const { return max_; }

    private:
        std::vector<float> min_;
        std::vector<float> max_;
        size_t dim_ = 0;
        bool trained_ = false;

        void check_trained() const {
            if (!trained_) {
                throw std::runtime_error("ScalarQuantizer: call train() before quantize()/dequantize()");
            }
        }
    };

} // namespace nanodb
