// Small command-line entry point. For now it only reports build info; replay and benchmark
// commands come with the later phases.
#include <cstdio>
#include <cstring>

#include "matchbook/types.hpp"
#include "matchbook/version.hpp"

namespace {

void print_info() {
    std::printf("matchbook %s\n", matchbook::kVersion);
#if defined(__clang__)
    std::printf("compiler: clang %d.%d\n", __clang_major__, __clang_minor__);
#elif defined(__GNUC__)
    std::printf("compiler: gcc %d.%d\n", __GNUC__, __GNUC_MINOR__);
#elif defined(_MSC_VER)
    std::printf("compiler: msvc %d\n", _MSC_VER);
#endif
    std::printf("C++ standard: %ld\n", static_cast<long>(__cplusplus));
    // sizes matter here: these live in the hot path and get copied around a lot
    std::printf("sizeof(Price)=%zu sizeof(Qty)=%zu sizeof(OrderId)=%zu\n", sizeof(matchbook::Price),
                sizeof(matchbook::Qty), sizeof(matchbook::OrderId));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--info") == 0) {
        print_info();
        return 0;
    }
    std::printf("usage: matchbook --info\n");
    return argc > 1 ? 1 : 0;
}
