#!/usr/bin/env bash
# Linux-only checks that MinGW can't do: ASan/UBSan test run, gcov coverage, a libFuzzer run.
#
#   docker run --rm -v "$PWD:/src" -w /src ubuntu:24.04 bash scripts/check_linux.sh [fuzz_seconds]
set -euo pipefail
FUZZ_SECONDS="${1:-60}"

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential clang cmake ninja-build \
  gcovr git ca-certificates >/dev/null

echo "== ASan + UBSan (GCC, Debug)"
cmake -S . -B /tmp/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMATCHBOOK_SANITIZE=ON \
  -DMATCHBOOK_BUILD_BENCH=OFF >/dev/null
cmake --build /tmp/asan >/dev/null
/tmp/asan/tests/matchbook_tests --gtest_brief=1

echo "== line coverage (GCC, gcov)"
cmake -S . -B /tmp/cov -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMATCHBOOK_COVERAGE=ON \
  -DMATCHBOOK_BUILD_BENCH=OFF >/dev/null
cmake --build /tmp/cov >/dev/null
/tmp/cov/tests/matchbook_tests --gtest_brief=1 >/dev/null
gcovr -r . --object-directory /tmp/cov --filter 'src/' --filter 'include/' --print-summary \
  --txt /tmp/coverage.txt >/dev/null
tail -n 30 /tmp/coverage.txt

echo "== libFuzzer (Clang, ASan + UBSan), ${FUZZ_SECONDS}s"
cmake -S . -B /tmp/fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ \
  -DMATCHBOOK_FUZZ=ON -DMATCHBOOK_BUILD_TESTS=OFF -DMATCHBOOK_BUILD_BENCH=OFF >/dev/null
cmake --build /tmp/fuzz --target fuzz_session >/dev/null
mkdir -p /tmp/corpus
/tmp/fuzz/fuzz/fuzz_session /tmp/corpus -max_total_time="${FUZZ_SECONDS}" -max_len=4096 \
  -print_final_stats=1 2>&1 | tail -n 12
