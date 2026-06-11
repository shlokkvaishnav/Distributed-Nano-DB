#pragma once

#include <cstddef>
#include "../common/types.hpp"

namespace nanodb {

    // Squared Euclidean (L2^2) distance — AVX2 optimized.
    // sqrt() is skipped because it preserves ranking order and is expensive.
    float l2_distance(const float* a, const float* b, size_t dim);

    // Cosine distance: 1 - cosine_similarity — AVX2 optimized.
    // Returns a value in [0, 2]. Lower = more similar.
    // Used in NLP embedding pipelines (BERT, OpenAI embeddings, etc.)
    float cosine_distance(const float* a, const float* b, size_t dim);

    // Inner product distance: -dot(a, b) — AVX2 optimized.
    // Negated so that lower = more similar (consistent with min-heap search).
    // Used in recommendation systems and with pre-normalized vectors.
    float inner_product_distance(const float* a, const float* b, size_t dim);

    // Dispatcher: routes to the correct kernel based on metric.
    // This is the single call site used throughout HNSW.
    float compute_distance(const float* a, const float* b, size_t dim, DistanceMetric metric);

    // Legacy alias kept for backward compatibility.
    inline float get_distance(const float* a, const float* b, size_t dim) {
        return l2_distance(a, b, dim);
    }

} // namespace nanodb