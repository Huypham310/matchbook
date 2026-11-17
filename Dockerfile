# Build the release binaries and run the unit tests. Used by CI and for a quick
# local check without installing the toolchain on the host:
#
#   docker build -t matchbook .
#   docker run --rm matchbook                 # prints --info
#   docker run --rm matchbook ./build/release/tests/matchbook_tests --gtest_brief=1

FROM ubuntu:24.04

RUN apt-get update -qq \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake --preset release -DMATCHBOOK_BUILD_BENCH=OFF \
    && cmake --build --preset release \
    && ctest --preset release --output-on-failure

CMD ["./build/release/matchbook", "--info"]
