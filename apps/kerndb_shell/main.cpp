#include <iostream>

#include "shell.h"

int main(int argc, const char* argv[]) {
    return kerndb::shell::Run(argc, argv, std::cin, std::cout, std::cerr);
}
