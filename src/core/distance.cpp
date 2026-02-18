#include "../../include/core/distance.hpp"
#include <immintrin.h> // AVX2 intrinsics
#include <cmath>

namespace nanodb {

    // ---------------------------------------------------------------------------
    // L2 (Squared Euclidean) Distance — AVX2
    // ---------------------------------------------------------------------------
    float l2_distance(const float* a, const float* b, size_t dim) {
        __m256 sum = _mm256_setzero_ps();

        size_t i = 0;
        for (; i + 8 <= dim; i += 8) {
            __m256 va   = _mm256_loadu_ps(a + i);
            __m256 vb   = _mm256_loadu_ps(b + i);
            __m256 diff = _mm256_sub_ps(va, vb);
            __m256 sq   = _mm256_mul_ps(diff, diff);
            sum         = _mm256_add_ps(sum, sq);
        }

        // Horizontal reduction
        float temp[8];
        _mm256_storeu_ps(temp, sum);
        float total = 0.0f;
        for (int k = 0; k < 8; ++k) total += temp[k];

        // Scalar tail
        for (; i < dim; ++i) {
            float d = a[i] - b[i];
            total += d * d;
        }
        return total;
    }

    // ---------------------------------------------------------------------------
    // Cosine Distance — AVX2
    // Returns 1 - dot(a,b) / (|a| * |b|), range [0, 2].
    // Lower = more similar. Commonly used in NLP embedding pipelines.
    // ---------------------------------------------------------------------------
    float cosine_distance(const float* a, const float* b, size_t dim) {
        __m256 dot_acc  = _mm256_setzero_ps();
        __m256 norm_a   = _mm256_setzero_ps();
        __m256 norm_b   = _mm256_setzero_ps();

        size_t i = 0;
        for (; i + 8 <= dim; i += 8) {
            __m256 va = _mm256_loadu_ps(a + i);
            __m256 vb = _mm256_loadu_ps(b + i);
            dot_acc   = _mm256_add_ps(dot_acc, _mm256_mul_ps(va, vb));
            norm_a    = _mm256_add_ps(norm_a,  _mm256_mul_ps(va, va));
            norm_b    = _mm256_add_ps(norm_b,  _mm256_mul_ps(vb, vb));
        }

        // Horizontal reduction
        float tdot[8], tna[8], tnb[8];
        _mm256_storeu_ps(tdot, dot_acc);
        _mm256_storeu_ps(tna,  norm_a);
        _mm256_storeu_ps(tnb,  norm_b);

        float dot = 0.0f, na = 0.0f, nb = 0.0f;
        for (int k = 0; k < 8; ++k) { dot += tdot[k]; na += tna[k]; nb += tnb[k]; }

        // Scalar tail
        for (; i < dim; ++i) {
            dot += a[i] * b[i];
            na  += a[i] * a[i];
            nb  += b[i] * b[i];
        }

        float denom = std::sqrt(na) * std::sqrt(nb);
        if (denom < 1e-10f) return 1.0f; // Treat zero-norm vectors as maximally dissimilar
        return 1.0f - (dot / denom);
    }

    // ---------------------------------------------------------------------------
    // Inner Product Distance — AVX2
    // Returns -dot(a, b) so that lower = more similar (consistent with min-heap).
    // Used in recommendation systems and with pre-normalized vectors.
    // ---------------------------------------------------------------------------
    float inner_product_distance(const float* a, const float* b, size_t dim) {
        __m256 acc = _mm256_setzero_ps();

        size_t i = 0;
        for (; i + 8 <= dim; i += 8) {
            __m256 va = _mm256_loadu_ps(a + i);
            __m256 vb = _mm256_loadu_ps(b + i);
            acc       = _mm256_add_ps(acc, _mm256_mul_ps(va, vb));
        }

        float temp[8];
        _mm256_storeu_ps(temp, acc);
        float dot = 0.0f;
        for (int k = 0; k < 8; ++k) dot += temp[k];

        for (; i < dim; ++i) dot += a[i] * b[i];

        return -dot; // Negate: lower = more similar
    }

    // ---------------------------------------------------------------------------
    // Dispatcher
    // ---------------------------------------------------------------------------
    float compute_distance(const float* a, const float* b, size_t dim, DistanceMetric metric) {
        switch (metric) {
            case DistanceMetric::Cosine:       return cosine_distance(a, b, dim);
            case DistanceMetric::InnerProduct: return inner_product_distance(a, b, dim);
            case DistanceMetric::L2:
            default:                           return l2_distance(a, b, dim);
        }
    }

} // namespace nanodb