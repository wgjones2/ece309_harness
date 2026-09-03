# Vibe Coding Summary and Log

For this project I used Claude Opus 5 set to ultracode effort in Claude Code.
Claude is already my choice of AI. I already had a subscription. I did not
buy it specifically for this class. 

Because I used such a top-tier model and because I used a very detailed intial
prompt, Claude was able to one-shot this assignment. I was not able to find
any bugs or errors in the initial output. This is partially because
the assignment document was not very detailed. I would have appreciated more
detail on how exactly it should work because its hard to do SDD when you don't
even understand exactly what you are supposed to make. This program seems
quite useless.

In the same prompt it also generated the bash test script. Here is my prompt:
```
I have been assigned a C programming project that, by requirements, must be coded by AI. I need you to build a minimal LLM agent harness in C (not C++).
They define harness as "An agent harness acts as the bridge between an LLM and the operating system, managing inputs, context boundaries, tool execution, etc." So while it does have tool elements they mean more the chat interface that also can do tool calls in a simple command-line program.
Your harness must be written in standard C and compile successfully in a POSIX environment. The program must have a terminal-based loop that captures user input, passes it to a mock model function (which mimics an LLM) and outputs the simulated response. The program should have context management to allocate and manage memory safely to store a minimal conversation history.
In addition to the main project you should write a seperate testing script that validates the harness's state management and checks for basic memory leaks.
Avoid external libraries when making this project. Stick to standard C libraries like <stdio.h> and <string.h>. Use simple coding methods and use clear line-by-line comments to explain what the code is doing. Use heavy comments. The program file should be harness.c. It should be a single file implementation.
Program specifications:
- Run an infinite while loop that asks for user input using fgets.
- If the user types 'exit' the loop should break and the program should end.
- If the user types a sentence containing the word 'hello', the program should print a hardcoded greeting.
- If the user types anything else, it should echo their input back to them.
- It should call a tool to execute functions such as mathematical calculation that an LLM is not designed for.
WSL and GCC are installed.
```
This was the response:
```
I'll start by checking the environment, then write the harness.

Empty project dir, GCC 15.2 in WSL, no valgrind (I'll use AddressSanitizer plus a built-in allocation counter as the leak check). Writing the harness now.

Command length exceeded the spawn limit — using the Write tool instead.

Clean build, no warnings. Smoke-testing the behaviors:

The harness works end to end. One improvement first — a leading unary minus (`-5 + 2`) should be part of the detected expression:

Now the test script:

Two fixes needed — both in the test script. The eviction check matched the harness's own live echo of the line rather than the history block, and one label had a stray `%%`:

All 61 checks pass. Cleaning up my scratch binary:

Done. Two files, both verified in WSL with GCC 15.2.

## [harness.c](harness.c) — 1094 lines, single file, standard C only

Builds clean under `-std=c99 -Wall -Wextra -pedantic -Werror`. Includes are `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<time.h>` — nothing external, and no `-lm` (I wrote `my_sqrt` via Newton's method and `my_pow` by repeated multiplication rather than pull in `<math.h>`).

Organized into 8 commented sections:

1. **Checked allocation** — every byte goes through `xmalloc`/`xrealloc`/`xfree`, which also tally allocations vs. frees. The last line of every session prints `[memory] allocations=N frees=N outstanding=0`, so the program self-reports leaks.
2. **Context management** — `Conversation` is a growable array of `{role, heap string}`. Capacity doubles as needed; `max_messages` is the context window, and `conv_trim` frees and drops the *oldest* message when the count exceeds it. That's the "context boundary."
3. **String helpers** — trim, case-insensitive search/compare.
4. **Tools** — a registry table (name, description, function pointer) holding `calc`, `wordcount`, and `time`. `calc` is a real recursive-descent parser: `+ - * / % ^`, parentheses, `sqrt()`, `abs()`, unary minus, with division-by-zero and syntax errors reported rather than crashing.
5. **Mock model** — decides in order: contains "hello" → hardcoded greeting; looks like arithmetic → request the `calc` tool; otherwise → echo. Tool results are appended to the conversation as `ROLE_TOOL` messages before the reply is composed, which is the actual agent loop.
6. **Harness commands** — `/help`, `/history`, `/stats`, `/clear`, `/tools`, `/tool NAME ARGS`.
7. **Input** — `read_line` calls `fgets` in a loop and grows the buffer, so a 500-character line works as well as a short one.
8. **Main loop** — infinite `for(;;)`, breaks on `exit`/`quit` (case-insensitive) or EOF, frees everything on the way out.

bash
gcc -std=c99 -Wall -Wextra -pedantic -o harness harness.c && ./harness


## [test_harness.sh](test_harness.sh) — 469 lines, 61 checks

bash
bash test_harness.sh


Compiles with `-Werror` (a warning fails the suite), then pipes scripted input in and greps the output. Coverage: the loop behaviors (exit, EOF, greeting, echo, blank lines, 500-char input), tool execution (precedence, parens, `2^10`, `10 % 3`, `-5 + 2`, `7/2`, division by zero, unbalanced parens, and that prose like "2 apples + 3 oranges" is *not* sent to calc), and state management — that `--max-history 4` over 6 messages yields `messages=4/4 evicted=2 total_added=6`, that the evicted message is really gone from `/history`, and that `/clear` empties context while preserving lifetime counters.

Leak checking runs three ways: the built-in `outstanding=0` counter across four different exit paths, an AddressSanitizer + UndefinedBehaviorSanitizer build with `detect_leaks=1`, and valgrind if present. Valgrind isn't installed in your WSL, so it prints `[SKIP]` and the sanitizer path covers it — `sudo apt install valgrind` would enable that block.

One thing to flag: expression detection is a heuristic, so a string like `555-1234` will be read as arithmetic. Since a real model would make the tool-call decision itself, I kept the detection simple and deliberately conservative — it requires the *entire* remainder of the line to be valid arithmetic, which is why prose with numbers falls through to echo.
```

I also asked it to catalog how to use the program in a readme file: "Write a detailed README.md describing how to use the program." I did edit this but I used the examples and tool explainations from it.
This was Claude's response to that prompt:
```
I'll capture real program output first so every example in the README is verbatim rather than reconstructed.

Verifying the example outputs I documented:

Every example matches. One timestamp in the tool table to align with the real format, then a final check of the directory:

[README.md](README.md) is written — 23 KB, and every code block in it is captured verbatim from actual runs rather than reconstructed from memory. I verified each documented example (`10 - 4 + 1` → 7, `(2+3)*4` → 20, `abs(3-10)` → 7, `sqrt(16)` → 4, the `missing ')'` error, and that `say hello and compute 2+2` returns the greeting rather than calling the calculator).
```
