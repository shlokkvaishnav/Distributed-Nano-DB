#include <iostream>
#include <cmath>
#include <cassert>
#include <vector>
#include <string>

#include "../include/core/distance.hpp"
#include "../include/common/types.hpp"

// Simple test framework
static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT_NEAR(val, expected, tol, msg) \
    do { \
        ++tests_run; \
        float _v = (val); float _e = (expected); float _t = (tol); \
        if (std::fabs(_v - _e) > _t) { \
            std::cerr << "[FAIL] " << msg << ": got " << _v << ", expected " << _e \
                      << " (tol=" << _t << ")\n"; \
            ++tests_failed; \
        } else { \
            std::cout << "[PASS] " << msg << "\n"; \
        } \
    } while(0)

#define ASSERT_EQ(val, expected, msg) \
    do { \
        ++tests_run; \
        if ((val) != (expected)) { \
            std::cerr << "[FAIL] " << msg << ": got " << (val) << ", expected " << (expected) << "\n"; \
            ++tests_failed; \
        } else { \
            std::cout << "[PASS] " << msg << "\n"; \
        } \
    } while(0)

using namespace nanodb;

// ---------------------------------------------------------------------------
// L2 Distance Tests
// ---------------------------------------------------------------------------
void test_l2_identical() {
    float a[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    ASSERT_NEAR(l2_distance(a, a, 8), 0.0f, 1e-6f, "L2: identical vectors = 0");
}

void test_l2_known_value() {
    // a = [1,0,...], b = [0,1,...] → L2^2 = 2
    float a[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float b[8] = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    ASSERT_NEAR(l2_distance(a, b, 8), 2.0f, 1e-5f, "L2: orthogonal unit vectors = 2");
}

void test_l2_scalar_tail() {
    // dim=9 exercises the scalar tail path (9 = 8 + 1)
    float a[9] = {1,2,3,4,5,6,7,8, 3.0f};
    float b[9] = {1,2,3,4,5,6,7,8, 0.0f};
    ASSERT_NEAR(l2_distance(a, b, 9), 9.0f, 1e-5f, "L2: scalar tail path (dim=9)");
}

// ---------------------------------------------------------------------------
// Cosine Distance Tests
// ---------------------------------------------------------------------------
void test_cosine_identical() {
    float a[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    ASSERT_NEAR(cosine_distance(a, a, 8), 0.0f, 1e-5f, "Cosine: identical vectors = 0");
}

void test_cosine_orthogonal() {
    float a[8] = {1,0,0,0,0,0,0,0};
    float b[8] = {0,1,0,0,0,0,0,0};
    // dot=0, so cosine_similarity=0, cosine_distance=1
    ASSERT_NEAR(cosine_distance(a, b, 8), 1.0f, 1e-5f, "Cosine: orthogonal vectors = 1");
}

void test_cosine_opposite() {
    float a[8] = {1,0,0,0,0,0,0,0};
    float b[8] = {-1,0,0,0,0,0,0,0};
    // cosine_similarity=-1, cosine_distance=2
    ASSERT_NEAR(cosine_distance(a, b, 8), 2.0f, 1e-5f, "Cosine: opposite vectors = 2");
}

void test_cosine_known_angle() {
    // 45-degree angle: a=[1,0], b=[1,1]/sqrt(2) → cosine_similarity = 1/sqrt(2)
    float a[8] = {1,0,0,0,0,0,0,0};
    float b[8] = {0.7071068f, 0.7071068f, 0,0,0,0,0,0}; // normalized [1,1]
    float expected = 1.0f - (1.0f / std::sqrt(2.0f));
    ASSERT_NEAR(cosine_distance(a, b, 8), expected, 1e-4f, "Cosine: 45-degree angle");
}

// ---------------------------------------------------------------------------
// Inner Product Distance Tests
// ---------------------------------------------------------------------------
void test_ip_identical_unit() {
    float a[8] = {1,0,0,0,0,0,0,0};
    // -dot(a,a) = -1
    ASSERT_NEAR(inner_product_distance(a, a, 8), -1.0f, 1e-6f, "IP: unit vector self = -1");
}

void test_ip_orthogonal() {
    float a[8] = {1,0,0,0,0,0,0,0};
    float b[8] = {0,1,0,0,0,0,0,0};
    ASSERT_NEAR(inner_product_distance(a, b, 8), 0.0f, 1e-6f, "IP: orthogonal = 0");
}

void test_ip_known_value() {
    float a[8] = {1,2,3,4,5,6,7,8};
    float b[8] = {8,7,6,5,4,3,2,1};
    // dot = 1*8+2*7+3*6+4*5+5*4+6*3+7*2+8*1 = 8+14+18+20+20+18+14+8 = 120
    ASSERT_NEAR(inner_product_distance(a, b, 8), -120.0f, 1e-3f, "IP: known dot product");
}

// ---------------------------------------------------------------------------
// Dispatcher Tests
// ---------------------------------------------------------------------------
void test_dispatcher_routes_correctly() {
    float a[8] = {1,0,0,0,0,0,0,0};
    float b[8] = {0,1,0,0,0,0,0,0};

    ASSERT_NEAR(compute_distance(a, b, 8, DistanceMetric::L2),
                l2_distance(a, b, 8), 1e-6f, "Dispatcher: L2 routes correctly");
    ASSERT_NEAR(compute_distance(a, b, 8, DistanceMetric::Cosine),
                cosine_distance(a, b, 8), 1e-6f, "Dispatcher: Cosine routes correctly");
    ASSERT_NEAR(compute_distance(a, b, 8, DistanceMetric::InnerProduct),
                inner_product_distance(a, b, 8), 1e-6f, "Dispatcher: IP routes correctly");
}

int main() {
    std::cout << "=== Distance Function Tests ===\n\n";

    test_l2_identical();
    test_l2_known_value();
    test_l2_scalar_tail();

    test_cosine_identical();
    test_cosine_orthogonal();
    test_cosine_opposite();
    test_cosine_known_angle();

    test_ip_identical_unit();
    test_ip_orthogonal();
    test_ip_known_value();

    test_dispatcher_routes_correctly();

    std::cout << "\n=== Results: " << (tests_run - tests_failed) << "/" << tests_run << " passed ===\n";
    return tests_failed > 0 ? 1 : 0;
}
