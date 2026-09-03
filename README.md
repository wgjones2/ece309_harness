# ECE 309 Project 1: An LLM Mini-Harness in C via Vibe Coding

This is a simple terminal chat program written in standard C that acts as a mock agent harness.
The program will echo back any general text and call tools for tasks AI model
are not designed for. An agent harness is the bridge between a language model and the
person using it. This harness is designed to chat with the model, save input and output,
store the conversations, etc.

The mock model function simulates and LLM's responses so if this were to be used with a real AI
you would have to swap out that section of the code. Beyond that, nothing else would change.

---

## Requirements

- A POSIX environment (Linux, macOS, or **WSL** on Windows).
- A C compiler supporting C99 — `gcc` or `clang`.
- `bash`, `awk`, `sed`, and `grep` for the test script (standard on any POSIX system).
- Optional: `valgrind`. The test script uses it automatically if it is installed and
  falls back to compiler sanitizers if it is not.

The program uses **only the C standard library**. No external libraries, and not even
`-lm`: square root is computed with Newton's method and exponentiation by repeated
multiplication, so `<math.h>` is never needed.

Standard headers used: `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<time.h>`.

---

## Running

To run from WSL in VSCode, use Ctrl + ~ or (Cmd + ), compile the code with gcc harness.c -o harness,
and then after compile run it with ./harness. You can also test the output using the built-in bash script
by running ./test.sh.

Here is an example of a full chat using the code:

```
=== Minimal LLM Agent Harness v1.0 ===
Context window: 12 messages. Type /help for commands, 'exit' to quit.

you> hello there
model> Hello! I am a mock language model running inside a small C harness. Ask me to do some math, or type 'exit' to quit.

you> what is 12 * 7?
[tool] calc("12 * 7") = 84
model> I used the calc tool: 12 * 7 = 84

you> remember the milk
model> You said: "remember the milk"

you> /tool wordcount the quick brown fox
[tool] wordcount("the quick brown fox") = 4 word(s), 16 non-space character(s)

you> /history
[history] 8 message(s) in context:
  1. user      | hello there
  2. assistant | Hello! I am a mock language model running inside a small C harness. Ask me to do some math, or type 'exit' to quit.
  3. user      | what is 12 * 7?
  4. tool      | calc("12 * 7") = 84
  5. assistant | I used the calc tool: 12 * 7 = 84
  6. user      | remember the milk
  7. assistant | You said: "remember the milk"
  8. tool      | wordcount("the quick brown fox") = 4 word(s), 16 non-space character(s)

you> /stats
[stats] messages=8/12 evicted=0 total_added=8 chars=310

you> exit
[harness] exit requested

[harness] session summary
[stats] messages=8/12 evicted=0 total_added=8 chars=310
[memory] allocations=19 frees=19 outstanding=0
```

### Output prefixes

Every line the program prints is tagged so you can tell who produced it:

| Prefix | Meaning |
| --- | --- |
| `you>` | The input prompt. Everything after it on that line is what you typed. |
| `model>` | The mock model's response. |
| `[tool]` | A tool the harness executed on the model's behalf, with its arguments and result. |
| `[history]`, `[stats]`, `[tools]`, `[context]` | Output of a harness command. |
| `[harness]` | A lifecycle message: exit requested, end of input, session summary. |
| `[memory]` | The allocation tally printed once at shutdown. |
| `[error]` | A bad command or option. |

### Ending the session

Three ways to stop, all of which run the same clean-up path:

- Type `exit` or `quit` (case-insensitive — `EXIT` works too).
- Type `/exit` or `/quit`.
- Press `Ctrl-D` (end of input), or let a piped input stream run out.

On the way out the program prints a final `/stats` line and the `[memory]` tally, then
frees every byte it allocated.

---

## Command-line options

| Option | Effect |
| --- | --- |
| `--max-history N` | Set the context window to `N` messages instead of the default 12. `N` must be at least 1. |
| `--no-banner` | Skip the startup banner. Useful when piping input for a script. |
| `--version` | Print `harness 1.0` and exit. |
| `--help` | Print the usage line and the command list, then exit. |

Invalid input is rejected with a message on standard error and an exit status of 1:

```bash
./harness --max-history 0      # harness: --max-history must be at least 1
./harness --nonsense           # harness: unknown option '--nonsense' (try --help)
```

`./harness --help` prints:

```
usage: ./harness [--max-history N] [--no-banner] [--version]
Commands (handled by the harness, never sent to the model):
  /help              show this text
  /history           print the conversation currently in context
  /stats             print context-window statistics
  /clear             erase the stored conversation
  /tools             list the tools the harness can execute
  /tool NAME ARGS    run a tool directly, e.g. /tool calc 6*7
  exit               quit the program (so does 'quit' or EOF)
Anything else is sent to the mock model:
  a line containing 'hello' gets a greeting;
  a line containing arithmetic triggers the calc tool;
  anything else is echoed back.
```

---

## Harness commands

Lines that begin with `/` are handled by the harness itself and are **never sent to the
model**. They are how you inspect and reset the stored conversation.

| Command | What it does |
| --- | --- |
| `/help` | Print the command list. |
| `/history` | Print every message currently in context, numbered, with its role. |
| `/stats` | Print the context-window counters (explained below). |
| `/clear` | Free every stored message and empty the context. The session continues. |
| `/tools` | List the tools the harness can execute, with a one-line description each. |
| `/tool NAME ARGS` | Run a tool directly, bypassing the model. Example: `/tool calc 6*7`. |
| `/exit`, `/quit` | Leave the program (same as typing `exit`). |

An unrecognized command is reported rather than sent to the model:

```
you> /bogus
[error] unknown command: /bogus (try /help)
```

Blank lines and whitespace-only lines are ignored entirely — they are not stored and the
model is not called.

---

## How the mock model decides what to say

`mock_model()` applies three rules **in this order**, and the first one that matches wins:

1. **Greeting.** If the line contains `hello` anywhere, case-insensitively, return the
   hardcoded greeting. `hello`, `HELLO`, and `Well hello there, harness!` all match.
2. **Tool call.** If the line contains an arithmetic expression, ask the harness to run
   the `calc` tool and report the result.
3. **Echo.** Otherwise, repeat the input back as `You said: "..."`.

Because rule 1 is checked first, `say hello and compute 2+2` gets the greeting and does
*not* trigger the calculator.

### When is a line treated as arithmetic?

The detector is deliberately conservative. Starting from the first digit, `(`, `sqrt`,
`abs`, or a leading `+`/`-` sign, **the entire rest of the line** must consist only of
arithmetic characters, and it must contain at least one digit *and* one operator. This is
why prose containing numbers still falls through to the echo rule:

```
you> what is 12 * 7?
[tool] calc("12 * 7") = 84
model> I used the calc tool: 12 * 7 = 84

you> 2 apples + 3 oranges
model> You said: "2 apples + 3 oranges"
```

In the first line, the text from `12` onward (`12 * 7?`) is pure arithmetic once the
question mark is stripped, so the tool runs. In the second, the word `apples` disqualifies
it, so the line is echoed instead. A leading sign is included in the expression when it
begins the line or follows a space, so `-5 + 2` evaluates to `-3` rather than `7`.

---

## Tools

A language model predicts text; it does not actually compute. So the harness owns a table
of real C functions and runs them when the model asks. Each tool result is added to the
conversation as a `tool` message before the model composes its reply — that round trip is
the agent loop.

```
you> /tools
[tools] 3 tool(s) registered:
  calc       - evaluate arithmetic: + - * / % ^ ( ) sqrt() abs()
  wordcount  - count the words and characters in some text
  time       - read the current local date and time
```

| Tool | Invoked by | Example |
| --- | --- | --- |
| `calc` | The model, automatically, whenever it sees arithmetic; or `/tool calc EXPR`. | `/tool calc 6*7` → `= 42` |
| `wordcount` | `/tool wordcount TEXT` | `/tool wordcount the quick brown fox` → `4 word(s), 16 non-space character(s)` |
| `time` | `/tool time` | `= 2026-09-02 23:08:41` |

### Calculator syntax

`calc` is a real recursive-descent parser, not a lookup table. It supports:

| Feature | Example | Result |
| --- | --- | --- |
| Add, subtract | `10 - 4 + 1` | `7` |
| Multiply, divide | `12 * 7` | `84` |
| Correct precedence | `2+3*4` | `14` |
| Parentheses | `(2+3)*4` | `20` |
| Exponent (whole-number, right associative) | `2^10` | `1024` |
| Integer modulo | `10 % 3` | `1` |
| Unary minus | `-5 + 2` | `-3` |
| Decimals | `7 / 2` | `3.5` |
| Square root | `sqrt(16)` | `4` |
| Absolute value | `abs(3-10)` | `7` |
| Nested combinations | `(2+3)*sqrt(16)` | `20` |

Whole-number results print without a decimal point (`84`, not `84.000000`).

### Tool errors

Bad input is reported and the session continues; the program never crashes on it:

```
you> 1/0
[tool] calc("1/0") failed: division by zero
model> The calc tool could not evaluate "1/0": division by zero

you> (2+3
[tool] calc("(2+3") failed: missing ')'
model> The calc tool could not evaluate "(2+3": missing ')'
```

Other messages you may see: `modulo by zero`, `sqrt of a negative number`,
`unexpected trailing characters`, `malformed number`,
`'^' supports whole-number exponents only`, and `no such tool: NAME`.

---

## Context management

The conversation is stored in a dynamically allocated array of messages. Each message
records a **role** (`user`, `assistant`, or `tool`) and its own heap-allocated copy of the
text. The array starts at 4 slots and doubles whenever it fills.

### The context window

A real model has a token limit, so a real harness has to decide what to forget. This
program models that with a message-count window, set by `--max-history` and defaulting to
**12**. When appending a message would exceed the window, the **oldest** message is freed
and dropped from the front of the array:

```bash
./harness --max-history 4
```

```
you> one
model> You said: "one"

you> two
model> You said: "two"

you> three
model> You said: "three"

you> /stats
[stats] messages=4/4 evicted=2 total_added=6 chars=40
```

Three exchanges appended six messages (three from the user, three from the model), the
window held at four, and the two oldest were evicted.

### Reading `/stats`

```
[stats] messages=8/12 evicted=0 total_added=8 chars=310
```

| Field | Meaning |
| --- | --- |
| `messages=8/12` | 8 messages currently in context, out of a window of 12. |
| `evicted=0` | How many messages have been dropped so far to honor the window. |
| `total_added=8` | Lifetime count of messages ever appended. Never decreases. |
| `chars=310` | Total characters currently held — a stand-in for a token count. |

`/clear` sets `messages` and `chars` back to 0 but deliberately **keeps** `evicted` and
`total_added`, because those are lifetime session statistics rather than context.

### What gets stored

One echo exchange stores 2 messages (user, assistant). One tool exchange stores 3 (user,
tool result, assistant). Harness commands such as `/history` and `/stats` store nothing —
they are not part of the conversation. `/tool NAME ARGS` stores only the tool result.

---

## Memory management

Every allocation in the program goes through three wrappers — `xmalloc`, `xrealloc`, and
`xfree` — which check for allocation failure in one place and keep a running tally of
blocks handed out versus blocks handed back. The last line of every session reports it:

```
[memory] allocations=19 frees=19 outstanding=0
```

`outstanding=0` means the program released everything it allocated. Anything else is a
leak, and the test script checks this line specifically.

Ownership rules used throughout:

- `read_line()` returns a heap string the caller must free.
- `mock_model()` returns a heap string the caller must free.
- `conv_add()` makes its **own copy** of the text, so the caller keeps ownership of what
  it passed in and frees it normally.
- `conv_free()` at shutdown releases every message string and then the array itself.

The input reader grows its buffer with `realloc` as needed, so a line of any length is
read correctly; it does not truncate at a fixed size.
