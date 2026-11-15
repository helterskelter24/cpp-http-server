#include <cstdio>

#include "version.h"

int main() {
    std::printf("cpp-http-server %s\n", hs::version());
    return 0;
}
