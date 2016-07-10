// Generated by fructose_gen.py

#include "g1.h"
#include "g2.h"
#include "g3.h"

#include <stdlib.h>

int main(int argc, char* argv[])
{
    int retval = EXIT_SUCCESS;

    {
        sample_test sample_test_instance;
        sample_test_instance.add_test("test42", &sample_test::test42);
        sample_test_instance.add_test("beast", &sample_test::beast);
        sample_test_instance.add_test("fivealive", &sample_test::fivealive);
        const int r = sample_test_instance.run(argc, argv);
        retval = (retval == EXIT_SUCCESS) ? r : EXIT_FAILURE;
    }
    {
        misc_tests misc_tests_instance;
        misc_tests_instance.add_test("exceptions", &misc_tests::exceptions);
        misc_tests_instance.add_test("loopdata", &misc_tests::loopdata);
        misc_tests_instance.add_test("floatingpoint", &misc_tests::floatingpoint);
        const int r = misc_tests_instance.run(argc, argv);
        retval = (retval == EXIT_SUCCESS) ? r : EXIT_FAILURE;
    }
    {
        exception_test exception_test_instance;
        exception_test_instance.add_test("array_bounds", &exception_test::array_bounds);
        exception_test_instance.add_test("should_catch_std_exceptions", &exception_test::should_catch_std_exceptions);
        const int r = exception_test_instance.run(argc, argv);
        retval = (retval == EXIT_SUCCESS) ? r : EXIT_FAILURE;
    }
    {
        MyTest MyTest_instance;
        const int r = MyTest_instance.run(argc, argv);
        retval = (retval == EXIT_SUCCESS) ? r : EXIT_FAILURE;
    }

    return retval;
}