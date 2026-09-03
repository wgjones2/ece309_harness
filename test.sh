#!/usr/bin/env bash
# ============================================================================
# test_harness.sh -- validation script for harness.c
# ----------------------------------------------------------------------------
# ECE 309 Project 1.
#
# This script is the automated grader for the harness.  It does four things:
#
#   1. BUILD       compiles harness.c with warnings treated as errors, so the
#                  build itself is a test of code quality.
#   2. BEHAVIOR    feeds scripted input into the program over a pipe and checks
#                  the printed output (greeting / echo / exit / tool calls).
#   3. STATE       drives the context window on purpose -- overflow, eviction,
#                  /clear -- and verifies the numbers reported by /stats.
#   4. MEMORY      checks for leaks three ways: the harness's own allocation
#                  counter, an AddressSanitizer + UndefinedBehaviorSanitizer
#                  build, and valgrind when it happens to be installed.
#
# USAGE:   bash test_harness.sh
# EXIT:    0 if every check passed, 1 otherwise (usable in a CI pipeline).
# ============================================================================

# --- shell settings ---------------------------------------------------------
set -u                       # treat the use of an unset variable as an error

# --- locate ourselves so the script works from any directory ----------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # absolute path to this script
cd "$SCRIPT_DIR" || exit 1                     # run everything from there

SRC="harness.c"              # the program under test
BIN="./harness_test"         # normal build produced by this script
BIN_ASAN="./harness_asan"    # sanitizer build produced by this script

# --- test bookkeeping -------------------------------------------------------
PASSED=0                     # number of checks that succeeded
FAILED=0                     # number of checks that failed

# Print a section header so the output is easy to read.
section() {
    printf '\n=== %s ===\n' "$1"
}

# Record a passing check.
pass() {
    PASSED=$((PASSED + 1))                     # bump the counter
    printf '  [PASS] %s\n' "$1"                # and say which check it was
}

# Record a failing check, showing what we wanted and what we actually got.
fail() {
    FAILED=$((FAILED + 1))                     # bump the failure counter
    printf '  [FAIL] %s\n' "$1"                # the name of the check
    printf '         expected: %s\n' "$2"      # what should have happened
    printf '         --- actual output ---\n'  # then the real output, indented
    printf '%s\n' "$3" | sed 's/^/         | /'
}

# Run the harness with scripted input.
#   $1        = everything to type, newlines and all
#   $2 .. $n  = extra command-line options for the harness
# stdout and stderr are combined so error messages are testable too.
run() {
    local input="$1"                           # the scripted keystrokes
    shift                                      # the rest are harness options
    printf '%s\n' "$input" | "$BIN" --no-banner "$@" 2>&1
}

# Assert that the output contains a fixed string.
check_contains() {
    local name="$1" output="$2" needle="$3"    # name, haystack, needle
    if printf '%s' "$output" | grep -qF -- "$needle"; then
        pass "$name"                           # the string was found
    else
        fail "$name" "output containing: $needle" "$output"
    fi
}

# Assert that the output does NOT contain a fixed string.
check_absent() {
    local name="$1" output="$2" needle="$3"    # name, haystack, needle
    if printf '%s' "$output" | grep -qF -- "$needle"; then
        fail "$name" "output WITHOUT: $needle" "$output"
    else
        pass "$name"                           # correctly absent
    fi
}

# Assert that two strings are exactly equal.
check_equals() {
    local name="$1" actual="$2" expected="$3"  # name, got, wanted
    if [ "$actual" = "$expected" ]; then
        pass "$name"                           # exact match
    else
        fail "$name" "exactly: $expected" "$actual"
    fi
}

# ============================================================================
# 1. BUILD
# ============================================================================
section "1. Build"

# -Werror turns any warning into a build failure, so a clean build is a real
# check on the source and not just a convenience.
BUILD_LOG="$(gcc -std=c99 -Wall -Wextra -pedantic -Werror -o "$BIN" "$SRC" 2>&1)"
if [ $? -eq 0 ]; then
    pass "harness.c compiles with -Wall -Wextra -pedantic -Werror"
else
    fail "harness.c compiles with -Wall -Wextra -pedantic -Werror" \
         "a clean compile" "$BUILD_LOG"
    printf '\nThe program did not build, so no further tests can run.\n'
    exit 1                                     # nothing else is meaningful
fi

# ============================================================================
# 2. BEHAVIOR -- the required conversation loop
# ============================================================================
section "2. Conversation loop behavior"

# --- 'exit' must break the loop and shut the program down -------------------
OUT="$(run 'exit')"
check_contains "typing 'exit' leaves the loop" "$OUT" "[harness] exit requested"
check_contains "a session summary is printed on the way out" "$OUT" "session summary"

# --- nothing typed after 'exit' may be processed ----------------------------
OUT="$(run 'exit
hello there')"
check_absent "input after 'exit' is never processed" "$OUT" "Hello! I am a mock"

# --- 'exit' is matched case-insensitively -----------------------------------
OUT="$(run 'EXIT')"
check_contains "'EXIT' also leaves the loop" "$OUT" "[harness] exit requested"

# --- end of input (Ctrl-D / closed pipe) must also end the loop cleanly -----
OUT="$(run 'just one line and then EOF')"
check_contains "end of input ends the loop" "$OUT" "[harness] end of input"

# --- the hardcoded greeting ------------------------------------------------
OUT="$(run 'hello
exit')"
check_contains "'hello' returns the hardcoded greeting" "$OUT" \
    "Hello! I am a mock language model"

OUT="$(run 'Well HELLO there, harness!
exit')"
check_contains "'hello' is found mid-sentence, any case" "$OUT" \
    "Hello! I am a mock language model"

# --- the echo fallback ------------------------------------------------------
OUT="$(run 'banana bread
exit')"
check_contains "unrecognized input is echoed back" "$OUT" \
    'You said: "banana bread"'

# --- greeting takes priority over the echo ---------------------------------
OUT="$(run 'say hello please
exit')"
check_absent "a greeting is not also echoed" "$OUT" 'You said:'

# --- blank lines are ignored rather than sent to the model ------------------
OUT="$(run '


/stats
exit')"
check_contains "blank lines are ignored (nothing stored)" "$OUT" \
    "total_added=0"

# --- a whitespace-only line is also ignored ---------------------------------
OUT="$(run '
/stats
exit')"
check_contains "whitespace-only lines are ignored" "$OUT" "total_added=0"

# --- input longer than the fgets chunk must survive intact ------------------
# INPUT_CHUNK in harness.c is 128 bytes, so a 500-character line forces
# read_line() to grow its buffer with realloc several times.
LONG="$(awk 'BEGIN{s="";while(length(s)<500)s=s "abcdefghij";print substr(s,1,500)}')"
OUT="$(run "$LONG
exit")"
check_contains "a 500-character line is read and echoed in full" "$OUT" \
    "${LONG: -40}"

# --- an unknown harness command is reported, not crashed on -----------------
OUT="$(run '/bogus
exit')"
check_contains "unknown commands are reported" "$OUT" "[error] unknown command"

# ============================================================================
# 3. TOOL EXECUTION
# ============================================================================
section "3. Tool execution"

# --- the model recognizes arithmetic inside a sentence ----------------------
OUT="$(run 'what is 12 * 7?
exit')"
check_contains "arithmetic in a sentence triggers the calc tool" "$OUT" \
    '[tool] calc('
check_contains "12 * 7 evaluates to 84" "$OUT" "= 84"

# --- operator precedence ----------------------------------------------------
OUT="$(run '2+3*4
exit')"
check_contains "precedence: 2+3*4 = 14" "$OUT" "= 14"

# --- parentheses and functions ---------------------------------------------
OUT="$(run '(2+3)*sqrt(16)
exit')"
check_contains "(2+3)*sqrt(16) = 20" "$OUT" "= 20"

# --- exponent, modulo, unary minus -----------------------------------------
OUT="$(run '2^10
exit')"
check_contains "2^10 = 1024" "$OUT" "= 1024"

OUT="$(run '10 % 3
exit')"
check_contains "10 % 3 = 1" "$OUT" "= 1"

OUT="$(run '-5 + 2
exit')"
check_contains "a leading minus is part of the expression: -5 + 2 = -3" \
    "$OUT" "= -3"

# --- decimals ---------------------------------------------------------------
OUT="$(run '7 / 2
exit')"
check_contains "7 / 2 = 3.5" "$OUT" "= 3.5"

# --- tool errors are reported instead of crashing --------------------------
OUT="$(run '1/0
exit')"
check_contains "division by zero is reported" "$OUT" "division by zero"
check_contains "the program keeps running after a tool error" "$OUT" \
    "[harness] exit requested"

OUT="$(run '(2+3
exit')"
check_contains "unbalanced parentheses are reported" "$OUT" "missing ')'"

# --- plain prose that happens to contain numbers is NOT sent to calc --------
OUT="$(run '2 apples + 3 oranges
exit')"
check_absent "prose with numbers does not call calc" "$OUT" "[tool] calc"
check_contains "prose with numbers is echoed instead" "$OUT" \
    'You said: "2 apples + 3 oranges"'

# --- explicit tool invocation ----------------------------------------------
OUT="$(run '/tool calc 6*7
exit')"
check_contains "/tool calc runs the calculator" "$OUT" "= 42"

OUT="$(run '/tool wordcount the quick brown fox
exit')"
check_contains "/tool wordcount counts words" "$OUT" "4 word(s)"

OUT="$(run '/tool nosuchtool 1 2 3
exit')"
check_contains "an unknown tool is refused safely" "$OUT" "no such tool"

# --- the registry is discoverable ------------------------------------------
OUT="$(run '/tools
exit')"
check_contains "/tools lists the calculator" "$OUT" "calc"
check_contains "/tools lists the word counter" "$OUT" "wordcount"

# ============================================================================
# 4. STATE MANAGEMENT -- context storage, the window, and eviction
# ============================================================================
section "4. Context / state management"

# --- a fresh session starts empty ------------------------------------------
OUT="$(run '/history
exit')"
check_contains "a new session has an empty history" "$OUT" "[history] (empty)"

# --- one exchange stores exactly two messages (user + assistant) -----------
OUT="$(run 'banana bread
/stats
exit')"
check_contains "one echo exchange stores 2 messages" "$OUT" "messages=2/12"
check_contains "and nothing was evicted yet" "$OUT" "evicted=0"

# --- a tool call stores three messages (user + tool result + assistant) ----
OUT="$(run '2+2
/stats
exit')"
check_contains "a tool exchange stores 3 messages" "$OUT" "messages=3/12"

# --- both roles are recorded, in order -------------------------------------
OUT="$(run 'banana bread
/history
exit')"
check_contains "the user turn is stored" "$OUT" "user      | banana bread"
check_contains "the assistant turn is stored" "$OUT" "assistant | You said:"

# --- the context window evicts the oldest message once it is full ----------
# --max-history 4 with three echo exchanges = 6 messages appended, so exactly
# two of them must have been dropped from the front.
OUT="$(run 'one
two
three
/stats
exit' --max-history 4)"
check_contains "the window is never exceeded" "$OUT" "messages=4/4"
check_contains "the oldest messages are evicted" "$OUT" "evicted=2"
check_contains "the lifetime counter still sees all 6" "$OUT" "total_added=6"

# --- eviction drops the OLDEST message, not a newer one --------------------
OUT="$(run 'oldest line
newest line
/history
exit' --max-history 2)"
check_absent "the evicted message is really gone" "$OUT" "| oldest line"
check_contains "the newest message is retained" "$OUT" "newest line"

# --- a window of exactly 1 still works -------------------------------------
OUT="$(run 'alpha
beta
/stats
exit' --max-history 1)"
check_contains "a window of 1 holds a single message" "$OUT" "messages=1/1"

# --- /clear empties the context but keeps the lifetime counters ------------
OUT="$(run 'alpha
beta
/clear
/stats
exit')"
check_contains "/clear reports success" "$OUT" "[context] cleared"
check_contains "/clear empties the context" "$OUT" "messages=0/12"
check_contains "/clear resets the character total" "$OUT" "chars=0"
check_contains "/clear keeps the lifetime counter" "$OUT" "total_added=4"

# --- the session can continue normally after /clear ------------------------
OUT="$(run '/clear
hello again
exit')"
check_contains "the harness still works after /clear" "$OUT" \
    "Hello! I am a mock language model"

# --- the character total tracks the stored text ----------------------------
OUT="$(run 'abc
/stats
exit')"
check_absent "the context size is measured, not left at zero" "$OUT" "chars=0"

# --- invalid configuration is rejected -------------------------------------
OUT="$(printf 'exit\n' | "$BIN" --max-history 0 2>&1)"
STATUS=$?
check_equals "--max-history 0 is rejected with a non-zero status" "$STATUS" "1"

OUT="$(printf 'exit\n' | "$BIN" --nonsense 2>&1)"
STATUS=$?
check_equals "an unknown option is rejected with a non-zero status" "$STATUS" "1"

# ============================================================================
# 5. MEMORY -- leak checks
# ============================================================================
section "5. Memory safety"

# A single stress script that exercises every allocation path in the program:
# growth of the message array, eviction, tool calls, /clear, long input, and
# the final teardown.
STRESS="hello there
what is (12 * 7) + sqrt(81)?
$LONG
1/0
/tool wordcount alpha beta gamma delta
/history
/clear
another line entirely
2^16
/stats
exit"

# --- 5a. the harness's own allocation counter ------------------------------
# xmalloc/xrealloc/xfree keep a running tally; the last line of a session
# reports it.  "outstanding=0" means every block was handed back.
OUT="$(run "$STRESS")"
check_contains "internal counter: no blocks outstanding at exit" "$OUT" \
    "outstanding=0"

# The same check for a session that ends at EOF rather than at 'exit', because
# that path frees the input buffer in a different place.
OUT="$(run 'hello
2+2
some text')"
check_contains "internal counter: no leak on the end-of-input path" "$OUT" \
    "outstanding=0"

# And for a session that overflows a tiny window many times over.
MANY="$(awk 'BEGIN{for(i=1;i<=60;i++) print "message number " i}')"
OUT="$(run "$MANY
/stats
exit" --max-history 3)"
check_contains "internal counter: no leak while evicting 60 messages" "$OUT" \
    "outstanding=0"
check_contains "the window held while 60 messages streamed through" "$OUT" \
    "messages=3/3"

# --- 5b. AddressSanitizer + UndefinedBehaviorSanitizer ---------------------
# These are compiler features, not external libraries: ASan reports real leaks
# at exit as well as any out-of-bounds access or use-after-free, and UBSan
# catches undefined behavior such as signed overflow or a bad shift.
ASAN_LOG="$(gcc -std=c99 -Wall -Wextra -pedantic -g -fno-omit-frame-pointer \
                -fsanitize=address,undefined -o "$BIN_ASAN" "$SRC" 2>&1)"
if [ $? -ne 0 ]; then
    printf '  [SKIP] sanitizer build unavailable on this system\n'
    printf '%s\n' "$ASAN_LOG" | sed 's/^/         | /'
else
    # detect_leaks=1 makes LeakSanitizer report any block still held at exit.
    SAN_OUT="$(printf '%s\n' "$STRESS" | \
        ASAN_OPTIONS=detect_leaks=1 \
        UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        "$BIN_ASAN" --no-banner 2>&1)"
    check_absent "AddressSanitizer reports no error" "$SAN_OUT" \
        "ERROR: AddressSanitizer"
    check_absent "LeakSanitizer reports no leak" "$SAN_OUT" \
        "ERROR: LeakSanitizer"
    check_absent "UndefinedBehaviorSanitizer reports no error" "$SAN_OUT" \
        "runtime error:"

    # Re-run under the sanitizers with a tiny window, which is the path that
    # frees and shifts messages most often.
    SAN_OUT="$(printf '%s\n/stats\nexit\n' "$MANY" | \
        ASAN_OPTIONS=detect_leaks=1 \
        "$BIN_ASAN" --no-banner --max-history 3 2>&1)"
    check_absent "sanitizers stay quiet during heavy eviction" "$SAN_OUT" \
        "ERROR: "

    # And on the end-of-input path, where the loop exits without 'exit'.
    SAN_OUT="$(printf 'hello\n2+2\ntrailing text with no newline' | \
        ASAN_OPTIONS=detect_leaks=1 "$BIN_ASAN" --no-banner 2>&1)"
    check_absent "sanitizers stay quiet on the end-of-input path" "$SAN_OUT" \
        "ERROR: "
fi

# --- 5c. valgrind, when the machine has it ---------------------------------
if command -v valgrind >/dev/null 2>&1; then
    VG_OUT="$(printf '%s\n' "$STRESS" | \
        valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect \
                 --error-exitcode=42 "$BIN" --no-banner 2>&1)"
    VG_STATUS=$?
    check_equals "valgrind finds no errors or definite leaks" "$VG_STATUS" "0"
    check_contains "valgrind confirms all heap blocks were freed" "$VG_OUT" \
        "All heap blocks were freed"
else
    printf '  [SKIP] valgrind is not installed; the sanitizer build above\n'
    printf '         and the internal counter already covered leak checking\n'
fi

# ============================================================================
# 6. SUMMARY
# ============================================================================
section "Summary"
printf '  passed: %d\n' "$PASSED"           # how many checks succeeded
printf '  failed: %d\n' "$FAILED"           # how many checks failed

# Remove the binaries this script built so the directory is left clean.
rm -f "$BIN" "$BIN_ASAN"

if [ "$FAILED" -eq 0 ]; then
    printf '\nALL TESTS PASSED\n'          # the message a grader looks for
    exit 0                                  # success status for CI
else
    printf '\n%d TEST(S) FAILED\n' "$FAILED"
    exit 1                                  # failure status for CI
fi