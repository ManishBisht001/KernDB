#include <iostream>

#include "test_framework.h"

int main() {
    return kerndb::test::TestRegistry::Instance().RunAll(std::cout, std::cerr);
}
