#!/usr/bin/env bash
#
# RINEX writer regression tests:
#   1. unit test for the satellite-ID formatter (host C++),
#   2. integration validator on a known-good RINEX file (must pass),
#   3. regression validator on a known-bad file reproducing the G34/G38 bug
#      (must be flagged invalid).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "==> unit: format_sat_id"
c++ -std=c++17 -Wall -Wextra -Imain tools/test_rinex_satid.cpp -o /tmp/test_rinex_satid
/tmp/test_rinex_satid

echo
echo "==> integration: good sample must validate clean"
python3 tools/validate_rinex.py tools/rinex_test_data/good_sample.rnx

echo
echo "==> regression: bad sample (G34/G38/...) must be flagged"
python3 tools/validate_rinex.py --expect-fail tools/rinex_test_data/bad_sample.rnx

echo
echo "ALL RINEX TESTS PASSED"
