#!/usr/bin/env bash
# Generates a code coverage report from a litekv_tests run built with
# -DENABLE_COVERAGE=ON. Prefers lcov/genhtml for an HTML report; falls
# back to plain gcov summaries when they aren't installed.
set -euo pipefail

BINARY_DIR="${1:-.}"
SOURCE_DIR="${2:-..}"
OBJ_DIR="CMakeFiles/litekv_tests.dir/src"

cd "$BINARY_DIR"

if command -v lcov >/dev/null 2>&1; then
    lcov --capture --directory . --output-file coverage.info \
        --ignore-errors gcov,graph,mismatch,inconsistent,unused
    lcov --remove coverage.info '*/tests/*' '*/third_party/*' '/usr/*' \
        --ignore-errors unused -o coverage.filtered.info
    lcov --list coverage.filtered.info

    if command -v genhtml >/dev/null 2>&1; then
        genhtml coverage.filtered.info --output-directory coverage_html \
            --ignore-errors inconsistent,corrupt
        echo "HTML report: $(pwd)/coverage_html/index.html"
    fi
else
    echo "lcov not found; falling back to gcov summaries for storage.cpp/parser.cpp"
    for src in storage.cpp parser.cpp; do
        gcda="$OBJ_DIR/$src.gcda"
        if [ -f "$gcda" ]; then
            gcov "$gcda" 2>/dev/null | grep -A1 "File '.*/$src'"
        fi
    done
fi
