#!/bin/sh
# host tests: needs g++ (any recent one) and python3 is not needed
set -e
cd "$(dirname "$0")/.."
T="${TMPDIR:-/tmp}"
g++ -O1 -Wall -Wextra -std=c++17 -Isrc -o "$T/nutool" tests/nutool.cpp
g++ -g -fsanitize=address,undefined -Wall -Wextra -std=c++17 -Itests/stub -Iinclude -Isrc -o "$T/test_nu" tests/test_nu.cpp
"$T/test_nu" | tail -1
