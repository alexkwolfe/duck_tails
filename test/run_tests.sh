#!/bin/bash
# Test runner for Duck Tails extension
# Sets up test fixtures and runs the DuckDB test suite

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FIXTURES_DIR="$SCRIPT_DIR/fixtures"
TEST_FIXTURES_DIR="/tmp/duck_tails_test_fixtures"

echo "Duck Tails Test Runner"
echo "======================"

# Clean up any existing fixtures
if [ -d "$TEST_FIXTURES_DIR" ]; then
    echo "Cleaning up existing test fixtures..."
    rm -rf "$TEST_FIXTURES_DIR"
fi

# Set up test fixtures in predictable location
echo "Setting up test fixtures..."
export DUCK_TAILS_TEST_FIXTURES_DIR="$TEST_FIXTURES_DIR"
"$FIXTURES_DIR/setup_fixtures.sh" setup

# Validate fixtures
echo "Validating test fixtures..."
"$FIXTURES_DIR/setup_fixtures.sh" validate

# Run DuckDB tests
echo "Running DuckDB tests..."
cd "$PROJECT_DIR"

# Run individual test files or all tests based on arguments
if [ $# -eq 0 ]; then
    # Run all tests
    echo "Running all tests..."
    make test
else
    # Run specific test files
    for test_file in "$@"; do
        echo "Running test: $test_file"
        if [[ "$test_file" == *".test" ]]; then
            # Run specific .test file
            ./build/*/test/unittest "[sql]" --test-dir=test/sql --file="$test_file"
        else
            # Run by pattern
            ./build/*/test/unittest "$test_file"
        fi
    done
fi

echo "Test run complete."
echo "Test fixtures remain at: $TEST_FIXTURES_DIR"
echo "Use '$FIXTURES_DIR/setup_fixtures.sh cleanup' to clean up."