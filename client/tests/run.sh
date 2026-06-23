#!/usr/bin/env bash
# Build + run the host-side core tests against a running relay.
# Usage: ./run.sh [base_url] [dev_token]
set -euo pipefail
cd "$(dirname "$0")"
BASE="${1:-http://127.0.0.1:8099/v1}"
TOKEN="${2:-}"

g++ -std=gnu++17 -Wall host_test.cpp ../src/core/*.cpp ../third_party/cjson/cJSON.c \
    -I../src -lcurl -o /tmp/diizeru_host_test

/tmp/diizeru_host_test "$BASE" "$TOKEN"
