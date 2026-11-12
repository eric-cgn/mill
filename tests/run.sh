#!/bin/sh
# Build and run the core's host tests (gcc with warnings as errors and sanitizers), in
# the same pinned Retro68 image that builds Mill, which has a host gcc too. Allocations
# go through the tests' own functions, which can fail on purpose (see test_out_of_memory).
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/out
exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src \
  ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/t \
    -Dmalloc=t_malloc -Dcalloc=t_calloc -Drealloc=t_realloc \
    tests/test_core.c src/core/*.c src/third_party/tjpgd/tjpgd.c && /tmp/t && cp /tmp/out-*.pbm tests/out/'
