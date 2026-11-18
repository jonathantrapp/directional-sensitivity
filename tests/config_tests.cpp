#include "directional/config.h"
#include "directional/transform.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>

using namespace directional;

// Simple test framework - replace GTest with basic assertions if needed
static bool test_passed = true;
#define EXPECT_TRUE(cond) if (!(cond)) { test_passed = false; printf("FAIL line %d: %s\n", __LINE__, #cond); }
#define EXPECT_EQ(a, b) if ((a) != (b)) { test_passed = false; printf("FAIL line %d: %s != %s\n", __LINE__, #a, #b); }
#define ASSERT_TRUE(cond) if (!(cond)) { test_passed = false; return; }

void test_config_valid_identity() {
    FilterConfig config;
    make_identity_config(&config);
    
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.sx, 65536);  // Q16.16 unity
    EXPECT_EQ(config.sy, 65536);  // Q16.16 unity
    EXPECT_EQ(config.c_ne, 65536); // Q16.16 unity
    EXPECT_EQ(config.c_se, 65536); // Q16.16 unity
    EXPECT_EQ(config.c_sw, 65536); // Q16.16 unity
    EXPECT_EQ(config.c_nw, 65536); // Q16.16 unity
    
    Profile profile;
    EXPECT_TRUE(config_to_profile(&config, profile));
    
    // Verify that identity config produces an identity profile
    for (unsigned q = 0; q < 4; ++q) {
        const auto& gain = profile.gain(static_cast<Quadrant>(q));
        EXPECT_EQ(gain.x, 65536);
        EXPECT_EQ(gain.y, 65536);
    }
}

void test_config_valid_custom() {
    FilterConfig config;
    config.enabled = true;
    config.sx = 65536;  // 1.0
    config.sy = 98304;  // 1.5 (1.5 * 65536)
    config.c_ne = 65536; // 1.0
    config.c_se = 65536; // 1.0 
    config.c_sw = 65536; // 1.0
    config.c_nw = 65536; // 1.0
    
    Profile profile;
    EXPECT_TRUE(config_to_profile(&config, profile));
    
    // Verify we can create a valid profile from it
    const auto& gain = profile.gain(Quadrant::NE);
    EXPECT_EQ(gain.x, 65536);  // sx * c_ne = 65536 * 1.0 = 65536
    EXPECT_EQ(gain.y, 98304);  // sy * c_ne = 98304 * 1.0 = 98304
}

void test_config_invalid_validation() {
    FilterConfig config;
    
    // Test with zero or negative gains - should fail validation
    config.enabled = true;
    config.sx = 0;
    config.sy = 65536;
    config.c_ne = 65536;
    config.c_se = 65536;
    config.c_sw = 65536;
    config.c_nw = 65536;
    
    EXPECT_TRUE(!validate_config(&config));
    
    // Test with negative gains - should fail validation
    config.sx = -10000;
    config.sy = 65536;
    EXPECT_TRUE(!validate_config(&config));
    
    // Test with invalid anchors (outside bounds)
    config.sx = 65536;
    config.sy = 65536;
    config.c_ne = 1000000; // Way too large
    EXPECT_TRUE(!validate_config(&config));
}

void test_config_edge_case_validation() {
    FilterConfig config;
    
    // Valid maximum gains
    config.enabled = true;
    config.sx = 262144;  // 4.0 * 65536 (maximum allowed)
    config.sy = 262144;
    config.c_ne = 65536; // 1.0
    config.c_se = 65536;
    config.c_sw = 65536;
    config.c_nw = 65536;
    
    EXPECT_TRUE(validate_config(&config));
    
    // Invalid - too large
    config.sx = 300000;  // Beyond maximum allowed
    EXPECT_TRUE(!validate_config(&config));
}

void test_config_exact_cardinal_identity() {
    // Test that after fractional diagonal movement, exact cardinal packets maintain identity
    FilterConfig config;
    make_identity_config(&config);
    
    Profile profile;
    EXPECT_TRUE(config_to_profile(&config, profile));
    
    Processor p;
    p.apply(profile);
    
    // Process a diagonal packet to build up fractions
    auto result = p.process({100, 100});
    EXPECT_EQ(result.delta.x, 100);
    EXPECT_EQ(result.delta.y, 100);
    
    // Now process an exact cardinal packet - should not change the fraction behavior (cardinal bypass)
    result = p.process({200, 0});
    EXPECT_EQ(result.delta.x, 200);
    EXPECT_EQ(result.delta.y, 0);
    
    // Process another fractional diagonal to check that fractions are preserved correctly
    result = p.process({50, 50});
    EXPECT_EQ(result.delta.x, 50);
    EXPECT_EQ(result.delta.y, 50);
}

void test_config_quadrant_independence() {
    // Test each quadrant configured independently and combined properly
    FilterConfig config;
    
    // Configure each quadrant with different values
    config.enabled = true;
    config.sx = 65536; // 1.0
    config.sy = 65536; // 1.0  
    config.c_ne = 65536; // 1.0 - NE quadrant unchanged 
    config.c_se = 98304; // 1.5 - SE quadrant increased in y
    config.c_sw = 49152; // 0.75 - SW quadrant decreased in x
    config.c_nw = 131072; // 2.0 - NW quadrant increased in both directions
    
    Profile profile;
    EXPECT_TRUE(config_to_profile(&config, profile));
    
    Processor p;
    p.apply(profile);
    
    // Check each quadrant's behavior  
    auto result = p.process({100, -100}); // NE direction 
    EXPECT_EQ(result.delta.x, 100); // Should be unchanged (c_ne=1.0)
    EXPECT_EQ(result.delta.y, -100); // Should be unchanged (c_ne=1.0)
    
    result = p.process({100, 100}); // SE direction
    EXPECT_EQ(result.delta.x, 150); // sx=1.0, c_se=1.5 -> anchor_x=1.5
    EXPECT_EQ(result.delta.y, 150); // Should be scaled up (c_se=1.5)
    
    result = p.process({-100, 100}); // SW direction
    EXPECT_EQ(result.delta.x, -75); // Should be scaled down (c_sw=0.75 -> x is 75) 
    EXPECT_EQ(result.delta.y, 75); // sy=1.0, c_sw=0.75 -> anchor_y=0.75
    
    result = p.process({-100, -100}); // NW direction
    EXPECT_EQ(result.delta.x, -200); // Should be increased (c_nw=2.0 -> x is 200)
    EXPECT_EQ(result.delta.y, -200); // Should be increased (c_nw=2.0 -> y is 200)
    
    // Test equality with expected values
    config.c_ne = 98304; // 1.5
    config.c_se = 98304; // 1.5  
    config.c_sw = 98304; // 1.5
    config.c_nw = 98304; // 1.5
    
    EXPECT_TRUE(config_to_profile(&config, profile)); // Should validate
}

void test_config_equal_vs_unequal_axis_gains() {
    // Test configurations with equal and unequal axis gains
    FilterConfig config;
    
    // Equal gains - should produce consistent behavior
    config.enabled = true;
    config.sx = 98304; // 1.5
    config.sy = 98304; // 1.5
    config.c_ne = 65536; // 1.0 
    config.c_se = 65536; // 1.0
    config.c_sw = 65536; // 1.0
    config.c_nw = 65536; // 1.0
    
    Profile profile;
    EXPECT_TRUE(config_to_profile(&config, profile));
    
    Processor p;
    p.apply(profile);
    
    // Process some packets and make sure they behave as expected for even scaling  
    auto result = p.process({100, 100}); 
    EXPECT_EQ(result.delta.x, 150); // sx=sy=1.5, c_se=1.0 -> anchor 1.5 on both axes; // Should be 100 (1.5 * 100 = 150 but using anchor_x = sx * c_ne = 1.5 * 65536)
    
    // Let's check the actual anchors are computed correctly:
    const auto& gain = profile.gain(Quadrant::NE);
    EXPECT_EQ(gain.x, 98304); // sx * c_ne = 98304 * 1.0 = 98304 (should be x gain)
    EXPECT_EQ(gain.y, 98304); // sy * c_ne = 98304 * 1.0 = 98304 (should be y gain)
    
    // Unequal gains - should create different behaviors per quadrant
    config.sx = 65536; // 1.0 
    config.sy = 98304; // 1.5
    
    EXPECT_TRUE(config_to_profile(&config, profile)); // Should still be valid
}

void test_config_boundaries_and_extremes() {
    // Test near-axis and diagonal boundaries to ensure continuity
    FilterConfig config;
    config.enabled = true;
    config.sx = 65536;   // 1.0 
    config.sy = 65536;   // 1.0
    config.c_ne = 65536; // 1.0
    config.c_se = 65536; // 1.0  
    config.c_sw = 65536; // 1.0
    config.c_nw = 65536; // 1.0
    
    Profile profile;
    EXPECT_TRUE(config_to_profile(&config, profile));
    
    // Test extreme values for input processing
    Processor p;
    p.apply(profile);
    
    // Test with maximum int32 values to ensure overflow behavior 
    auto result = p.process({INT32_MAX, INT32_MAX});
    EXPECT_EQ(result.delta.x, INT32_MAX); // Overflow should pass through
    EXPECT_EQ(result.delta.y, INT32_MAX);
    EXPECT_TRUE(!result.overflow_bypass); // unity gain: output fits int32, so no bypass
    
    // Test near-axis conditions to ensure continuity  
    result = p.process({10000, 1}); // Near axis X
    EXPECT_EQ(result.delta.x, 10000); // Should preserve input
    EXPECT_EQ(result.delta.y, 1);
    
    result = p.process({1, 10000}); // Near axis Y  
    EXPECT_EQ(result.delta.x, 1);
    EXPECT_EQ(result.delta.y, 10000); // Should preserve input
    
    // Test that we can process packets with all sign combinations
    p.apply(profile);
    result = p.process({-100, 100});
    EXPECT_EQ(result.delta.x, -100);
    EXPECT_EQ(result.delta.y, 100);
    
    result = p.process({-100, -100});
    EXPECT_EQ(result.delta.x, -100);
    EXPECT_EQ(result.delta.y, -100);
}

void test_config_residual_accumulation_and_reset() {
    // Test residual behavior during reset and overflow
    FilterConfig config;
    make_identity_config(&config);
    
    Profile profile;
    EXPECT_TRUE(config_to_profile(&config, profile));
    
    Processor p;
    p.apply(profile);
    
    // Process diagonal packets to build up fractions
    p.process({100, 100});
    p.process({200, 200});
    
    // Process a cardinal packet - should bypass residuals but not clear them completely
    auto result = p.process({500, 0}); 
    EXPECT_EQ(result.delta.x, 500);
    
    // Reset and verify behavior again
    p.reset();
    
    // After reset, should process same with identity  
    result = p.process({100, 100});
    EXPECT_EQ(result.delta.x, 100);
    EXPECT_EQ(result.delta.y, 100);
    
    // Test that the identity configuration works correctly through resets
    for (int i = 0; i < 5; ++i) {
        p.reset();
        result = p.process({25, 25});
        EXPECT_EQ(result.delta.x, 25);
        EXPECT_EQ(result.delta.y, 25);
    }
}

void test_config_profile_validation_compatibility() {
    // Test to make sure configurations produce profiles with proper validation
    struct TestCase {
        std::int32_t sx, sy, c_ne, c_se, c_sw, c_nw;
        bool should_pass;
    };

    TestCase cases[] = {
        // Valid cases: identity, equal gains, etc.
        {65536, 65536, 65536, 65536, 65536, 65536, true},  // Identity
        {98304, 98304, 65536, 65536, 65536, 65536, true},  // 1.5x gain uniformly 
        {65536, 131072, 65536, 65536, 65536, 65536, true}, // Y-axis boost
        
        // Invalid cases: zero gain
        {0, 65536, 65536, 65536, 65536, 65536, false},     // Zero sx
        
        // Invalid cases: values too large or negative  
        {-10000, 65536, 65536, 65536, 65536, 65536, false}, // Negative sx
        {65536, 262144, 65536, 65536, 65536, 65536, false}, // Too large sy
        {65536, 65536, 1000000, 65536, 65536, 65536, false} // Invalid anchor
    };

    for (const auto& test : cases) {
        FilterConfig config;
        config.enabled = true;
        config.sx = test.sx;
        config.sy = test.sy; 
        config.c_ne = test.c_ne;
        config.c_se = test.c_se;
        config.c_sw = test.c_sw;
        config.c_nw = test.c_nw;
        
        if (test.should_pass) {
            EXPECT_TRUE(validate_config(&config)); // Should pass
            
            Profile profile;
            EXPECT_TRUE(config_to_profile(&config, profile)); // Should create valid profile
        } else {
            EXPECT_TRUE(!validate_config(&config)); // Should fail validation
        }
    }
}

int main() {
    // Run all tests
    test_config_valid_identity();
    test_config_valid_custom();
    test_config_invalid_validation();
    test_config_edge_case_validation();
    test_config_exact_cardinal_identity();
    test_config_quadrant_independence();
    test_config_equal_vs_unequal_axis_gains();
    test_config_boundaries_and_extremes();
    test_config_residual_accumulation_and_reset();
    test_config_profile_validation_compatibility();
    
    if (test_passed) {
        printf("All config tests passed!\n");
        return 0;
    } else {
        printf("Some config tests failed!\n");
        return 1;
    }
}