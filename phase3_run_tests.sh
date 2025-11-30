#!/bin/bash
# ============================================================
# Automated test script for Phase 3 (Multithreaded Server)
# ============================================================

# --- Config ---
PORT=5050
HOST="127.0.0.1"
SERVER_LOG="server_test.log"
C1_LOG="client1_test.log"
C2_LOG="client2_test.log"
C3_LOG="client3_test.log"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'
PASS="${GREEN}✔ PASS${NC}"
FAIL="${RED}✘ FAIL${NC}"

# ------------------------
# Preparation
# ------------------------  
echo "[INFO] Cleaning up old files..."
rm -f "$SERVER_LOG" "$C1_LOG" "$C2_LOG" "$C3_LOG"
rm -f out_c1.txt out_c3.txt a.txt b.txt

echo "[INFO] Compiling..."
make -s
if [ $? -ne 0 ]; then
    echo "[ERROR] Build failed."
    exit 1
fi

echo "[INFO] Starting server on port $PORT..."
./server $PORT >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 1 # Give server a moment to start

# Create test input files
echo "hello from a.txt" > a.txt
echo "hello from b.txt" > b.txt

# ------------------------
# Run all tests in PARALLEL
# ------------------------
echo "[INFO] Launching 3 clients simultaneously..."

# Client 1: Long-running command + output redirection
{
echo "sleep 2" # This will run long
echo "echo 'client 1' > out_c1.txt"
echo "exit"
} | ./client $HOST $PORT >"$C1_LOG" 2>&1 &

# Client 2: Quick command + error
{
echo "pwd"
echo "unknowncmd"
echo "exit"
} | ./client $HOST $PORT >"$C2_LOG" 2>&1 &

# Client 3: Input/Output redirection
{
echo "cat < a.txt"
echo "echo 'client 3' > out_c3.txt"
echo "exit"
} | ./client $HOST $PORT >"$C3_LOG" 2>&1 &

echo "[INFO] Waiting for all clients to finish..."
wait # Waits for all background jobs to complete
echo "[INFO] All clients finished."

# ------------------------
# Evaluate results
# ------------------------
echo
echo "========== Test Results =========="

# Helper to check a specific client's log
check_client_output() {
    desc="$1"
    pattern="$2"
    logfile="$3"
    if grep -Eq "$pattern" "$logfile"; then
        echo -e "$PASS $desc"
    else
        echo -e "$FAIL $desc"
    fi
}

# Helper to check a created file's content
check_file_content() {
    desc="$1"
    expected="$2"
    file="$3"
    if [ "$(cat $file 2>/dev/null)" = "$expected" ]; then
        echo -e "$PASS $desc"
    else
        echo -e "$FAIL $desc"
    fi
}

# --- Client-side checks ---
check_client_output "Client 2 (pwd)" "/.*" "$C2_LOG"
check_client_output "Client 2 (error)" "not found" "$C2_LOG"
check_client_output "Client 3 (cat)" "hello from a.txt" "$C3_LOG"
check_file_content "Client 1 (output redir)" "client 1" "out_c1.txt"
check_file_content "Client 3 (output redir)" "client 3" "out_c3.txt"

# --- Server-side checks (Concurrency) ---
if grep -q "Client #1 connected" "$SERVER_LOG" && \
   grep -q "Client #2 connected" "$SERVER_LOG" && \
   grep -q "Client #3 connected" "$SERVER_LOG"; then
    echo -e "$PASS Server accepted 3 clients"
else
    echo -e "$FAIL Server did not accept 3 clients"
fi

# --- Server-side checks (Log Format) ---
# This regex checks for "[TAG] [Client #X - IP:PORT] ..."
if grep -Eq "\[(RECEIVED|EXECUTING|ERROR|OUTPUT)\] \[Client #[0-9]+ - .*\]" "$SERVER_LOG"; then
    echo -e "$PASS Server used Phase 3 log format"
else
    echo -e "$FAIL Server did NOT use Phase 3 log format"
fi

# --- Server-side checks (Cleanup) ---
disconnect_count=$(grep -c "Client #[0-9]+ disconnected" "$SERVER_LOG")
if [ "$disconnect_count" -eq 3 ]; then
    echo -e "$PASS Server disconnected all 3 clients cleanly"
else
    echo -e "$FAIL Server disconnected $disconnect_count clients (expected 3)"
fi

# ------------------------
# Cleanup
# ------------------------
kill $SERVER_PID >/dev/null 2>&1
wait $SERVER_PID 2>/dev/null

echo
echo "=================================="
echo "Server logs saved to: $SERVER_LOG"
echo "Client 1 logs saved to: $C1_LOG"
echo "Client 2 logs saved to: $C2_LOG"
echo "Client 3 logs saved to: $C3_LOG"
echo "All tests completed."
echo "=================================="
