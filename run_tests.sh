#!/bin/bash

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'
PASS="${GREEN}✔ PASS${NC}"
FAIL="${RED}✘ FAIL${NC}"

# Helper: run one command in myshell, check expected regex in output
run_test() {
    desc="$1"
    cmd="$2"
    expected="$3"

    output=$(echo -e "$cmd\nexit\n" | ./myshell 2>&1)

    if echo "$output" | grep -qE "$expected"; then
        echo -e "$PASS $desc"
    else
        echo -e "$FAIL $desc"
        echo "  Command:  $cmd"
        echo "  Expected: $expected"
        echo "  Got:      $output"
    fi
}

# -------------------------
# SECTION 7: Composed Commands
# -------------------------

echo "hello world" > a.txt
echo "foo bar" > b.txt

run_test "Input redirection" \
    "cat < a.txt" \
    "hello world"

# Output redirection -> check file contents separately
echo -e "echo hi > out.txt\nexit\n" | ./myshell >/dev/null 2>&1
if [ "$(cat out.txt)" = "hi" ]; then
    echo -e "$PASS Output redirection"
else
    echo -e "$FAIL Output redirection"
fi

# Error redirection -> check file exists and not empty
echo -e "ls nofile 2> err.log\nexit\n" | ./myshell >/dev/null 2>&1
if [ -s err.log ]; then
    echo -e "$PASS Error redirection"
else
    echo -e "$FAIL Error redirection"
fi

run_test "Simple pipe" \
    "echo abc | wc -l" \
    "1"

# Pipe with output redirection
echo -e "echo abc | wc -l > out.txt\nexit\n" | ./myshell >/dev/null 2>&1
if [ "$(cat out.txt)" = "1" ]; then
    echo -e "$PASS Pipe with output redirection"
else
    echo -e "$FAIL Pipe with output redirection"
fi

run_test "Multi-stage pipeline" \
    "cat a.txt | tr a-z A-Z | wc -w" \
    "2"

# -------------------------
# SECTION 8: Error Handling
# -------------------------

run_test "Missing input file after <" \
    "cat <" \
    "syntax error|missing"

run_test "Missing output file after >" \
    "echo hi >" \
    "syntax error|missing"

run_test "Missing error file after 2>" \
    "echo hi 2>" \
    "syntax error|missing"

run_test "Trailing pipe" \
    "ls |" \
    "Command missing after pipe"

run_test "Double pipe" \
    "ls | | wc" \
    "syntax error|unexpected token"

run_test "Invalid command" \
    "doesnotexist" \
    "not found"

run_test "Invalid command in pipeline" \
    "ls | doesnotexist | wc" \
    "not found"

run_test "Nonexistent input file" \
    "cat < nofile.txt" \
    "No such file|not found"

echo
echo "All tests done."