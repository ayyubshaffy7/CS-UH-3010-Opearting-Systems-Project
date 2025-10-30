#!/bin/bash
# ============================================================
# Automated test script for Phase 2 (Client–Server Remote Shell)
# ============================================================

PORT=9090
SERVER_LOG="server_test.log"
CLIENT_LOG="client_test.log"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'
PASS="${GREEN}✔ PASS${NC}"
FAIL="${RED}✘ FAIL${NC}"

# ------------------------
# Preparation
# ------------------------
rm -f "$SERVER_LOG" "$CLIENT_LOG" out.txt a.txt b.txt

echo "[INFO] Compiling..."
make -s
if [ $? -ne 0 ]; then
    echo "[ERROR] Build failed."
    exit 1
fi

echo "[INFO] Starting server on port $PORT..."
./server $PORT >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 1

# Create test input files
echo "hello world" > a.txt
echo "foo bar" > b.txt

# ------------------------
# Run all tests in one client session
# ------------------------
{
echo "cat < a.txt"
echo "echo hi > out.txt"
echo "echo abc | wc -l"
echo "cat a.txt | tr a-z A-Z | wc -w"
echo "doesnotexist"
echo "exit"
} | ./client 127.0.0.1 $PORT >"$CLIENT_LOG" 2>&1

# ------------------------
# Evaluate results
# ------------------------
echo
echo "========== Test Results =========="

check_output() {
    desc="$1"
    pattern="$2"
    if grep -Eq "$pattern" "$CLIENT_LOG"; then
        echo -e "$PASS $desc"
    else
        echo -e "$FAIL $desc"
    fi
}

# Test 1: Input redirection
check_output "Input redirection" "hello world"

# Test 2: Output redirection (file written)
if [ "$(cat out.txt 2>/dev/null)" = "hi" ]; then
    echo -e "$PASS Output redirection"
else
    echo -e "$FAIL Output redirection"
fi

# Test 3: Simple pipe
check_output "Simple pipe" "1"

# Test 4: Multi-stage pipeline
check_output "Multi-stage pipeline" "2"

# Test 5: Invalid command
check_output "Invalid command" "not found"

# Test 6: Exit command closes session cleanly
if tail -n 1 "$SERVER_LOG" | grep -q "Client disconnected"; then
    echo -e "$PASS Exit command"
else
    echo -e "$FAIL Exit command"
fi

# ------------------------
# Cleanup
# ------------------------
kill $SERVER_PID >/dev/null 2>&1
wait $SERVER_PID 2>/dev/null

echo
echo "=================================="
echo "Server logs saved to $SERVER_LOG"
echo "Client output saved to $CLIENT_LOG"
echo "All tests completed."
echo "=================================="
