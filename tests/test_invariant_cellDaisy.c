#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

// Include the actual production header
#include "libs/spurs/cellDaisy.h"

START_TEST(test_ring_buffer_allocation_overflow_protection)
{
    // Invariant: Ring buffer allocation must not overflow when multiplying entrySize by depth
    struct CellDaisyAttr attr;
    
    // Test cases: exploit case, boundary case, valid case
    struct {
        size_t entrySize;
        size_t depth;
        const char *description;
    } test_cases[] = {
        // Exploit case: multiplication overflows to small value
        {SIZE_MAX / 2 + 1, 2, "Overflow to small allocation"},
        // Boundary case: just below overflow threshold
        {SIZE_MAX / 2, 2, "Boundary below overflow"},
        // Valid case: normal operation
        {1024, 100, "Valid normal allocation"}
    };
    
    int num_cases = sizeof(test_cases) / sizeof(test_cases[0]);
    
    for (int i = 0; i < num_cases; i++) {
        attr.entrySize = test_cases[i].entrySize;
        attr.depth = test_cases[i].depth;
        
        // Call the actual production function
        CellDaisy *daisy = cellDaisyCreate(&attr);
        
        // Security property: If multiplication would overflow, 
        // the function must detect this and fail safely (return NULL)
        if (attr.entrySize > SIZE_MAX / attr.depth) {
            // Overflow would occur - function must return NULL
            ck_assert_msg(daisy == NULL, 
                "Failed to detect overflow: entrySize=%zu, depth=%zu",
                attr.entrySize, attr.depth);
        } else {
            // No overflow - function may succeed or fail for other reasons
            // Clean up if allocation succeeded
            if (daisy != NULL) {
                cellDaisyDestroy(daisy);
            }
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_ring_buffer_allocation_overflow_protection);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}