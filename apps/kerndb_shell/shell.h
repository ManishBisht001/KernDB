#pragma once

#include <iosfwd>

namespace kerndb::shell {

int Run(
    int argc,
    const char* const argv[],
    std::istream& input,
    std::ostream& output,
    std::ostream& error);

}  // namespace kerndb::shell
