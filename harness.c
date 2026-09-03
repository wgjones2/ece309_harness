/* ==========================================================================
 * harness.c -- A Minimal LLM Agent Harness in C
 * --------------------------------------------------------------------------
 * ECE 309 Project 1.
 *
 * WHAT IS AN "AGENT HARNESS"?
 *   A harness is the bridge between a language model and the operating
 *   system.  The model itself only maps text to text; the harness is the
 *   program around it that:
 *       1. reads input from the user (terminal I/O),
 *       2. stores the running conversation in memory (context management),
 *       3. trims that conversation so it never grows without bound
 *          (context boundaries / the "context window"),
 *       4. notices when the model has asked for a tool, runs that tool,
 *          and feeds the result back into the conversation (tool execution),
 *       5. prints the final response.
 *
 *   This file implements all five jobs around a *mock* model.  The mock model
 *   is a plain C function: it takes the conversation plus the newest user
 *   line and returns a heap-allocated reply string.  Swapping in a real model
 *   later would mean replacing only mock_model(); nothing else changes.
 *
 * BUILD (POSIX / standard C, no external libraries):
 *       gcc -std=c99 -Wall -Wextra -pedantic -o harness harness.c
 *
 * RUN:
 *       ./harness                    -- normal interactive session
 *       ./harness --max-history 4    -- tiny context window (shows eviction)
 *       ./harness --help             -- usage text
 *
 * Every include below is part of the C standard library.
 * ========================================================================== */

#include <stdio.h>   /* printf, fputs, fgets, fflush, stdin, stdout          */
#include <stdlib.h>  /* malloc, realloc, free, strtod, strtol, exit          */
#include <string.h>  /* strlen, strcmp, strncmp, strchr, memcpy, memmove     */
#include <ctype.h>   /* isspace, isdigit, tolower                            */
#include <time.h>    /* time, localtime, strftime -- used by the "time" tool */

/* ==========================================================================
 * SECTION 0: COMPILE-TIME CONFIGURATION
 * ========================================================================== */

#define HARNESS_VERSION      "1.0"  /* printed in the startup banner          */
#define INPUT_CHUNK          128    /* bytes requested per fgets() call       */
#define DEFAULT_MAX_HISTORY  12     /* messages kept before the oldest is cut */
#define MAX_RESPONSE         2048   /* scratch buffer for one model reply     */
#define MAX_TOOL_OUTPUT      512    /* scratch buffer for one tool result     */
#define MAX_EXPR             256    /* scratch buffer for one math expression */

/* ==========================================================================
 * SECTION 1: CHECKED MEMORY ALLOCATION
 * --------------------------------------------------------------------------
 * Every byte this program allocates passes through xmalloc/xrealloc/xfree.
 * Routing all allocation through three functions gives us two things:
 *   (a) one place to check for a failed malloc, so the rest of the code can
 *       assume a returned pointer is valid, and
 *   (b) a running count of allocations vs. frees, which the program prints
 *       when it exits.  If "outstanding" is not 0, the program leaked, and
 *       the test script checks exactly that line.
 * ========================================================================== */

static unsigned long g_alloc_count = 0;  /* how many blocks we have handed out */
static unsigned long g_free_count  = 0;  /* how many blocks we have given back */

/* Allocate n bytes, or abort if the system is out of memory. */
static void *xmalloc(size_t n)
{
    void *p = malloc(n);              /* ask the C library for the memory      */
    if (p == NULL) {                  /* malloc returns NULL only on failure   */
        fputs("harness: out of memory\n", stderr); /* report to standard error */
        exit(EXIT_FAILURE);           /* cannot continue safely, so stop       */
    }
    g_alloc_count++;                  /* one more live block                   */
    return p;                         /* hand the caller a valid pointer       */
}

/* Resize a previous block to n bytes, or abort on failure. */
static void *xrealloc(void *ptr, size_t n)
{
    void *p = realloc(ptr, n);        /* realloc(NULL, n) behaves like malloc  */
    if (p == NULL) {                  /* NULL means the resize failed          */
        fputs("harness: out of memory\n", stderr);
        free(ptr);                    /* the old block is still ours to free   */
        exit(EXIT_FAILURE);
    }
    if (ptr == NULL) {                /* growing from nothing == a new block   */
        g_alloc_count++;              /* so count it as an allocation          */
    }
    return p;                         /* note: a plain resize changes no count */
}

/* Release a block obtained above.  Safe to call with NULL. */
static void xfree(void *ptr)
{
    if (ptr == NULL) {                /* free(NULL) is legal but counts for    */
        return;                       /* nothing, so leave the tally alone     */
    }
    free(ptr);                        /* return the memory to the C library    */
    g_free_count++;                   /* one fewer live block                  */
}

/* Copy a NUL-terminated string onto the heap.  (strdup is POSIX, not ISO C,
 * so we write our own to stay inside standard C.) */
static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;         /* +1 leaves room for the '\0' byte      */
    char *copy = xmalloc(n);          /* reserve exactly that many bytes       */
    memcpy(copy, s, n);               /* copy the text and its terminator      */
    return copy;                      /* caller now owns the copy              */
}

/* ==========================================================================
 * SECTION 2: THE CONVERSATION (CONTEXT MANAGEMENT)
 * --------------------------------------------------------------------------
 * The conversation is a grow-able array of messages.  Each message owns one
 * heap string.  Two limits are enforced:
 *   * capacity     -- how many slots the array has (doubles when it fills)
 *   * max_messages -- the context window; when the count passes this limit
 *                     the OLDEST message is freed and dropped, which is how
 *                     a real harness keeps a prompt inside a token budget.
 * ========================================================================== */

/* Who produced a message.  Real chat APIs use the same three roles. */
typedef enum {
    ROLE_USER,        /* text typed by the human                             */
    ROLE_ASSISTANT,   /* text produced by the (mock) model                   */
    ROLE_TOOL         /* text produced by a tool the harness executed        */
} Role;

/* One turn of the conversation. */
typedef struct {
    Role  role;       /* who said it                                          */
    char *text;       /* heap-allocated copy of what was said (owned by us)   */
} Message;

/* The whole conversation plus its bookkeeping. */
typedef struct {
    Message *items;         /* dynamic array of messages                      */
    size_t   count;         /* how many slots are currently in use            */
    size_t   capacity;      /* how many slots are allocated                   */
    size_t   max_messages;  /* context window: highest count we allow         */
    size_t   total_added;   /* lifetime number of messages ever appended      */
    size_t   evicted;       /* lifetime number dropped to honor the window    */
} Conversation;

/* Human-readable label for a role, used by /history. */
static const char *role_name(Role r)
{
    switch (r) {                          /* one case per enum value          */
        case ROLE_USER:      return "user";
        case ROLE_ASSISTANT: return "assistant";
        case ROLE_TOOL:      return "tool";
        default:             return "unknown";  /* unreachable, but safe      */
    }
}

/* Put a conversation into a valid empty state.  Nothing is allocated yet:
 * the array is created lazily on the first conv_add(). */
static void conv_init(Conversation *c, size_t max_messages)
{
    c->items        = NULL;               /* no array yet                     */
    c->count        = 0;                  /* no messages stored               */
    c->capacity     = 0;                  /* no slots reserved                */
    c->max_messages = max_messages;       /* remember the context window size */
    c->total_added  = 0;                  /* lifetime counters start at zero  */
    c->evicted      = 0;
}

/* Drop the oldest messages until the count fits inside the context window.
 * This is the "context boundary" the assignment asks for. */
static void conv_trim(Conversation *c)
{
    while (c->count > c->max_messages) {  /* keep going while we are over      */
        xfree(c->items[0].text);          /* free the oldest message's text    */
        memmove(&c->items[0],             /* destination: slot 0               */
                &c->items[1],             /* source: everything after slot 0   */
                (c->count - 1) * sizeof(Message)); /* bytes to shift down      */
        c->count--;                       /* the array is now one shorter      */
        c->evicted++;                     /* record that we dropped a message  */
    }
}

/* Append one message, copying the text onto the heap, then trim if needed. */
static void conv_add(Conversation *c, Role role, const char *text)
{
    if (c->count == c->capacity) {        /* the array is full (or empty)      */
        size_t newcap = (c->capacity == 0) ? 4 : c->capacity * 2; /* double it */
        c->items = xrealloc(c->items, newcap * sizeof(Message));  /* resize    */
        c->capacity = newcap;             /* remember the new slot count       */
    }
    c->items[c->count].role = role;       /* store who said it                 */
    c->items[c->count].text = xstrdup(text); /* store our own copy of the text */
    c->count++;                           /* the message is now part of state  */
    c->total_added++;                     /* lifetime counter, never trimmed   */
    conv_trim(c);                         /* enforce the context window        */
}

/* Free every message but keep the array itself, so the session can continue.
 * This backs the /clear command. */
static void conv_clear(Conversation *c)
{
    size_t i;                             /* loop index                        */
    for (i = 0; i < c->count; i++) {      /* visit every live message          */
        xfree(c->items[i].text);          /* release its heap string           */
        c->items[i].text = NULL;          /* avoid leaving a dangling pointer  */
    }
    c->count = 0;                         /* the conversation is now empty     */
}

/* Release absolutely everything the conversation owns.  Called once at exit. */
static void conv_free(Conversation *c)
{
    conv_clear(c);                        /* free the individual strings first */
    xfree(c->items);                      /* then free the array of slots      */
    c->items    = NULL;                   /* reset so a double free is impossible */
    c->capacity = 0;
}

/* Total characters currently held in context -- a stand-in for "token count". */
static size_t conv_total_chars(const Conversation *c)
{
    size_t i, total = 0;                  /* running sum                       */
    for (i = 0; i < c->count; i++) {      /* walk the live messages            */
        total += strlen(c->items[i].text);/* add each message's length         */
    }
    return total;                         /* report the sum                    */
}

/* Print the stored context, newest last.  This backs the /history command. */
static void conv_print(const Conversation *c)
{
    size_t i;                             /* loop index                        */
    if (c->count == 0) {                  /* nothing stored yet                */
        printf("[history] (empty)\n");    /* say so instead of printing nothing*/
        return;
    }
    printf("[history] %lu message(s) in context:\n",
           (unsigned long)c->count);      /* cast keeps %lu portable           */
    for (i = 0; i < c->count; i++) {      /* one line per stored message       */
        printf("  %lu. %-9s | %s\n",
               (unsigned long)(i + 1),     /* 1-based index for readability    */
               role_name(c->items[i].role),/* who said it                      */
               c->items[i].text);          /* what was said                    */
    }
}

/* Print the numbers the test script inspects.  Backs the /stats command. */
static void conv_stats(const Conversation *c)
{
    printf("[stats] messages=%lu/%lu evicted=%lu total_added=%lu chars=%lu\n",
           (unsigned long)c->count,        /* messages currently in context    */
           (unsigned long)c->max_messages, /* the configured window size       */
           (unsigned long)c->evicted,      /* how many were dropped so far     */
           (unsigned long)c->total_added,  /* how many were ever appended      */
           (unsigned long)conv_total_chars(c)); /* approximate context size    */
}

/* ==========================================================================
 * SECTION 3: SMALL STRING HELPERS
 * ========================================================================== */

/* The <ctype.h> functions are only defined for unsigned char values, so every
 * call goes through this cast helper to stay standard-conforming. */
static int uc(char ch)
{
    return (unsigned char)ch;             /* widen without sign extension      */
}

/* Remove leading and trailing whitespace from a string, in place. */
static void trim_whitespace(char *s)
{
    size_t len, start = 0;                /* start = index of first real char  */
    while (s[start] != '\0' && isspace(uc(s[start]))) { /* skip leading blanks */
        start++;
    }
    if (start > 0) {                      /* something was skipped             */
        memmove(s, s + start, strlen(s + start) + 1); /* shift left; the +1    */
    }                                                 /* also moves the '\0'   */
    len = strlen(s);                      /* length after the left trim        */
    while (len > 0 && isspace(uc(s[len - 1]))) { /* walk back over blanks      */
        s[len - 1] = '\0';                /* cut the trailing whitespace off   */
        len--;
    }
}

/* Case-insensitive substring search: does 'hay' contain 'needle' anywhere? */
static const char *stristr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);         /* length of what we are looking for */
    if (nlen == 0) {                      /* an empty needle matches at once   */
        return hay;
    }
    for (; *hay != '\0'; hay++) {         /* try every starting position       */
        size_t i = 0;                     /* how many characters have matched  */
        while (i < nlen &&                /* stop at the end of the needle     */
               hay[i] != '\0' &&          /* ...or the end of the haystack     */
               tolower(uc(hay[i])) == tolower(uc(needle[i]))) { /* compare     */
            i++;                          /* this character matched            */
        }
        if (i == nlen) {                  /* the whole needle matched          */
            return hay;                   /* return where the match began      */
        }
    }
    return NULL;                          /* no match anywhere in the string   */
}

/* Case-insensitive whole-string comparison, used for the "exit" keyword. */
static int str_equals_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {    /* walk both strings together        */
        if (tolower(uc(*a)) != tolower(uc(*b))) { /* first difference wins     */
            return 0;                     /* not equal                         */
        }
        a++;                              /* advance both cursors              */
        b++;
    }
    return *a == '\0' && *b == '\0';      /* equal only if both ended together */
}

/* ==========================================================================
 * SECTION 4: TOOLS
 * --------------------------------------------------------------------------
 * A language model predicts text; it does not actually compute.  So the
 * harness owns a small table of real C functions ("tools").  When the model
 * decides a request needs one, the harness runs it and feeds the result back
 * into the conversation.  Every tool shares one signature so they can live in
 * a single table and be dispatched by name.
 * ========================================================================== */

/* ---- 4a. Recursive-descent arithmetic parser used by the "calc" tool ----
 * Grammar (lowest precedence first):
 *     expr    := term  (('+' | '-') term)*
 *     term    := power (('*' | '/' | '%') power)*
 *     power   := unary ('^' power)?          <- right associative
 *     unary   := ('+' | '-') unary | primary
 *     primary := number | '(' expr ')' | ("sqrt" | "abs") '(' expr ')'
 */

/* Parser state: a cursor plus a place to record the first error seen. */
typedef struct {
    const char *p;       /* current read position inside the expression      */
    int         error;   /* 0 while everything is fine, 1 after a failure    */
    char        msg[96]; /* human-readable description of the failure        */
} Parser;

/* Record an error message, but only the first one (later ones are noise). */
static void parse_fail(Parser *ps, const char *msg)
{
    if (!ps->error) {                     /* keep the earliest explanation     */
        ps->error = 1;                    /* mark the parse as failed          */
        strncpy(ps->msg, msg, sizeof(ps->msg) - 1); /* bounded copy            */
        ps->msg[sizeof(ps->msg) - 1] = '\0';        /* guarantee termination   */
    }
}

/* Advance the cursor past any spaces or tabs. */
static void parse_spaces(Parser *ps)
{
    while (*ps->p != '\0' && isspace(uc(*ps->p))) { /* while on whitespace     */
        ps->p++;                          /* step over it                      */
    }
}

/* Square root by Newton's method, so we do not need <math.h> or -lm. */
static double my_sqrt(double x)
{
    double guess = x;                     /* start the iteration at x          */
    int i;                                /* iteration counter                 */
    if (x <= 0.0) {                       /* 0 returns 0; negatives are caught */
        return 0.0;                       /* by the caller before we get here  */
    }
    for (i = 0; i < 60; i++) {            /* 60 rounds converge well past need */
        guess = 0.5 * (guess + x / guess);/* Newton step: average g and x/g    */
    }
    return guess;                         /* the refined estimate              */
}

/* Absolute value without <math.h>. */
static double my_fabs(double x)
{
    return (x < 0.0) ? -x : x;            /* flip the sign when negative       */
}

/* Integer power by repeated multiplication (also avoids <math.h>). */
static double my_pow(Parser *ps, double base, double exp_val)
{
    double result = 1.0;                  /* anything to the power 0 is 1      */
    long   e, i;                          /* whole-number exponent and counter */
    if (exp_val != (double)(long)exp_val) {/* reject 2^0.5 and friends         */
        parse_fail(ps, "'^' supports whole-number exponents only");
        return 0.0;
    }
    e = (long)exp_val;                    /* now safe to use as an integer     */
    if (e < 0) {                          /* negative exponent = 1 / positive  */
        if (base == 0.0) {                /* 0^-n would divide by zero         */
            parse_fail(ps, "0 cannot be raised to a negative power");
            return 0.0;
        }
        e = -e;                           /* work with the magnitude           */
        for (i = 0; i < e; i++) {         /* multiply e times                  */
            result *= base;
        }
        return 1.0 / result;              /* then invert                       */
    }
    for (i = 0; i < e; i++) {             /* plain positive exponent           */
        result *= base;
    }
    return result;
}

/* Forward declaration: primary calls back into expr for parentheses. */
static double parse_expr(Parser *ps);

/* primary := number | '(' expr ')' | func '(' expr ')' */
static double parse_primary(Parser *ps)
{
    double value;                         /* the value this primary produces   */
    parse_spaces(ps);                     /* ignore leading blanks             */

    if (*ps->p == '(') {                  /* a parenthesized sub-expression    */
        ps->p++;                          /* consume the '('                   */
        value = parse_expr(ps);           /* parse whatever is inside          */
        parse_spaces(ps);                 /* allow blanks before the ')'       */
        if (*ps->p == ')') {              /* the closing parenthesis is there  */
            ps->p++;                      /* consume it                        */
        } else {
            parse_fail(ps, "missing ')'");/* unbalanced parentheses            */
        }
        return value;
    }

    if (strncmp(ps->p, "sqrt", 4) == 0) { /* the sqrt(...) function            */
        ps->p += 4;                       /* consume the name                  */
        parse_spaces(ps);
        if (*ps->p != '(') {              /* a function must be called         */
            parse_fail(ps, "expected '(' after sqrt");
            return 0.0;
        }
        ps->p++;                          /* consume the '('                   */
        value = parse_expr(ps);           /* evaluate the argument             */
        parse_spaces(ps);
        if (*ps->p == ')') {              /* consume the matching ')'          */
            ps->p++;
        } else {
            parse_fail(ps, "missing ')' after sqrt argument");
        }
        if (value < 0.0) {                /* no real square root of a negative */
            parse_fail(ps, "sqrt of a negative number");
            return 0.0;
        }
        return my_sqrt(value);            /* run the real computation          */
    }

    if (strncmp(ps->p, "abs", 3) == 0) {  /* the abs(...) function             */
        ps->p += 3;                       /* consume the name                  */
        parse_spaces(ps);
        if (*ps->p != '(') {
            parse_fail(ps, "expected '(' after abs");
            return 0.0;
        }
        ps->p++;                          /* consume the '('                   */
        value = parse_expr(ps);           /* evaluate the argument             */
        parse_spaces(ps);
        if (*ps->p == ')') {
            ps->p++;
        } else {
            parse_fail(ps, "missing ')' after abs argument");
        }
        return my_fabs(value);            /* strip the sign                    */
    }

    if (isdigit(uc(*ps->p)) || *ps->p == '.') {   /* a numeric literal         */
        char *end;                        /* strtod reports where it stopped   */
        value = strtod(ps->p, &end);      /* standard-library number parsing   */
        if (end == ps->p) {               /* nothing consumed => bad input     */
            parse_fail(ps, "malformed number");
            return 0.0;
        }
        ps->p = end;                      /* move the cursor past the number   */
        return value;
    }

    parse_fail(ps, "unexpected character in expression"); /* nothing matched   */
    return 0.0;
}

/* unary := ('+'|'-') unary | primary */
static double parse_unary(Parser *ps)
{
    parse_spaces(ps);                     /* skip blanks before the sign       */
    if (*ps->p == '-') {                  /* negation, e.g. -5                 */
        ps->p++;                          /* consume the '-'                   */
        return -parse_unary(ps);          /* recurse so "--5" also works       */
    }
    if (*ps->p == '+') {                  /* a leading '+' is a no-op          */
        ps->p++;
        return parse_unary(ps);
    }
    return parse_primary(ps);             /* otherwise fall through            */
}

/* power := unary ('^' power)?   -- right associative, so 2^3^2 = 2^(3^2) */
static double parse_power(Parser *ps)
{
    double base = parse_unary(ps);        /* left-hand side                    */
    parse_spaces(ps);                     /* blanks before the operator        */
    if (*ps->p == '^') {                  /* an exponent follows               */
        double exponent;                  /* right-hand side                   */
        ps->p++;                          /* consume the '^'                   */
        exponent = parse_power(ps);       /* recurse to the right              */
        return my_pow(ps, base, exponent);/* compute base^exponent             */
    }
    return base;                          /* no '^', just the base             */
}

/* term := power (('*'|'/'|'%') power)* */
static double parse_term(Parser *ps)
{
    double left = parse_power(ps);        /* start with the first operand      */
    for (;;) {                            /* loop over same-precedence ops     */
        char   op;                        /* the operator character            */
        double right;                     /* the next operand                  */
        parse_spaces(ps);                 /* blanks before the operator        */
        op = *ps->p;                      /* look at the current character     */
        if (op != '*' && op != '/' && op != '%') { /* not our precedence level */
            return left;                  /* hand the value up the chain       */
        }
        ps->p++;                          /* consume the operator              */
        right = parse_power(ps);          /* parse the right-hand operand      */
        if (ps->error) {                  /* stop early once something broke   */
            return 0.0;
        }
        if (op == '*') {                  /* multiplication                    */
            left = left * right;
        } else if (op == '/') {           /* division, guarding against /0     */
            if (right == 0.0) {
                parse_fail(ps, "division by zero");
                return 0.0;
            }
            left = left / right;
        } else {                          /* '%' modulo, integers only         */
            if ((long)right == 0) {
                parse_fail(ps, "modulo by zero");
                return 0.0;
            }
            left = (double)((long)left % (long)right); /* C's integer modulo   */
        }
    }
}

/* expr := term (('+'|'-') term)*   -- the lowest precedence level */
static double parse_expr(Parser *ps)
{
    double left = parse_term(ps);         /* first operand                     */
    for (;;) {                            /* consume + and - left to right     */
        char   op;                        /* operator character                */
        double right;                     /* next operand                      */
        parse_spaces(ps);                 /* blanks before the operator        */
        op = *ps->p;                      /* inspect the current character     */
        if (op != '+' && op != '-') {     /* not an additive operator          */
            return left;                  /* we are done at this level         */
        }
        ps->p++;                          /* consume the operator              */
        right = parse_term(ps);           /* parse the operand after it        */
        if (ps->error) {                  /* abort on the first failure        */
            return 0.0;
        }
        left = (op == '+') ? left + right : left - right; /* apply the operator*/
    }
}

/* Format a double the way a person would write it: 84 rather than 84.000000. */
static void format_number(double value, char *out, size_t outsz)
{
    double rounded = (value < 0.0) ? -(double)(long)(-value + 0.5)
                                   :  (double)(long)( value + 0.5); /* nearest */
    if (my_fabs(value - rounded) < 1e-9 && my_fabs(value) < 1e15) {
        snprintf(out, outsz, "%.0f", value);   /* whole number: no decimals    */
    } else {
        snprintf(out, outsz, "%.10g", value);  /* otherwise drop trailing zeros*/
    }
}

/* ---- 4b. Tool implementations ------------------------------------------
 * Contract shared by every tool:
 *   args   : the argument text the model extracted (never NULL)
 *   out    : caller-owned buffer the tool writes its result into
 *   outsz  : size of that buffer, so a tool can never overflow it
 *   return : 0 on success, non-zero on failure (out then holds the reason)
 */

/* The calculator: the classic example of work an LLM should delegate. */
static int tool_calc(const char *args, char *out, size_t outsz)
{
    Parser ps;                            /* parser state for this call        */
    double value;                         /* the computed result               */
    char   number[64];                    /* formatted form of the result      */

    ps.p      = args;                     /* start at the beginning of args    */
    ps.error  = 0;                        /* no error yet                      */
    ps.msg[0] = '\0';                     /* empty error message               */

    parse_spaces(&ps);                    /* allow leading blanks              */
    if (*ps.p == '\0') {                  /* an empty expression is an error   */
        snprintf(out, outsz, "no expression given");
        return 1;
    }

    value = parse_expr(&ps);              /* run the recursive-descent parser  */
    parse_spaces(&ps);                    /* allow trailing blanks             */

    if (!ps.error && *ps.p != '\0') {     /* text left over means bad syntax   */
        parse_fail(&ps, "unexpected trailing characters");
    }
    if (ps.error) {                       /* report the first problem found    */
        snprintf(out, outsz, "%s", ps.msg);
        return 1;
    }

    format_number(value, number, sizeof(number)); /* pretty-print the answer   */
    snprintf(out, outsz, "%s", number);   /* copy it into the caller's buffer  */
    return 0;                             /* success                           */
}

/* Counting words and characters exactly -- another job models get wrong. */
static int tool_wordcount(const char *args, char *out, size_t outsz)
{
    unsigned long words = 0;              /* whitespace-separated runs         */
    unsigned long chars = 0;              /* non-whitespace characters         */
    int in_word = 0;                      /* are we currently inside a word?   */
    const char *p = args;                 /* cursor over the argument text     */

    for (; *p != '\0'; p++) {             /* look at every character           */
        if (isspace(uc(*p))) {            /* whitespace ends the current word  */
            in_word = 0;
        } else {                          /* a printable character             */
            chars++;                      /* always counts toward chars        */
            if (!in_word) {               /* a word starts at the first one    */
                in_word = 1;
                words++;                  /* so count a new word               */
            }
        }
    }
    snprintf(out, outsz, "%lu word(s), %lu non-space character(s)",
             words, chars);               /* build the result string           */
    return 0;                             /* this tool cannot fail             */
}

/* Reading the system clock: information the model simply does not have. */
static int tool_time(const char *args, char *out, size_t outsz)
{
    time_t     now   = time(NULL);        /* seconds since the epoch           */
    struct tm *local = localtime(&now);   /* broken down into local fields     */
    (void)args;                           /* this tool ignores its arguments   */

    if (local == NULL) {                  /* localtime can fail on odd systems */
        snprintf(out, outsz, "system clock unavailable");
        return 1;
    }
    if (strftime(out, outsz, "%Y-%m-%d %H:%M:%S", local) == 0) { /* format it  */
        snprintf(out, outsz, "time formatting failed");
        return 1;
    }
    return 0;                             /* success                           */
}

/* ---- 4c. The tool registry ---------------------------------------------
 * One table maps a name to a description and a function pointer.  Adding a
 * tool means writing the function and adding one row here.
 */

typedef int (*ToolFn)(const char *args, char *out, size_t outsz); /* signature */

typedef struct {
    const char *name;   /* how the model refers to the tool                    */
    const char *desc;   /* one line shown by the /tools command                */
    ToolFn      fn;     /* the C function that actually does the work          */
} Tool;

static const Tool g_tools[] = {           /* the harness's whole toolbox       */
    { "calc",      "evaluate arithmetic: + - * / % ^ ( ) sqrt() abs()", tool_calc      },
    { "wordcount", "count the words and characters in some text",       tool_wordcount },
    { "time",      "read the current local date and time",              tool_time      }
};

/* Number of rows in the table, computed so it can never drift out of date. */
static const size_t g_tool_count = sizeof(g_tools) / sizeof(g_tools[0]);

/* Look a tool up by name; returns NULL when no such tool exists. */
static const Tool *tool_find(const char *name)
{
    size_t i;                             /* row index                         */
    for (i = 0; i < g_tool_count; i++) {  /* linear scan; the table is tiny    */
        if (strcmp(g_tools[i].name, name) == 0) { /* exact name match          */
            return &g_tools[i];           /* hand back the row                 */
        }
    }
    return NULL;                          /* not found                         */
}

/* Print the toolbox.  Backs the /tools command. */
static void tool_list(void)
{
    size_t i;                             /* row index                         */
    printf("[tools] %lu tool(s) registered:\n", (unsigned long)g_tool_count);
    for (i = 0; i < g_tool_count; i++) {  /* one line per tool                 */
        printf("  %-10s - %s\n", g_tools[i].name, g_tools[i].desc);
    }
}

/* Execute a tool, print a trace line, and record the result in context.
 * This single function is the harness's "tool execution" step: it is the
 * only place where a request for a tool turns into real work. */
static int tool_run(Conversation *conv, const char *name, const char *args,
                    char *out, size_t outsz)
{
    const Tool *tool = tool_find(name);   /* resolve the name to a function    */
    char note[MAX_TOOL_OUTPUT + 128];     /* buffer for the history entry      */
    int  status;                          /* 0 = success, non-zero = failure   */

    if (tool == NULL) {                   /* an unknown tool was requested     */
        snprintf(out, outsz, "no such tool: %s", name);
        printf("[tool] %s failed: %s\n", name, out); /* still trace the attempt*/
        return 1;
    }

    status = tool->fn(args, out, outsz);  /* CALL THE TOOL                     */

    printf("[tool] %s(\"%s\") %s %s\n",   /* trace so the user sees the call   */
           name, args,
           (status == 0) ? "=" : "failed:", /* a different arrow on failure    */
           out);

    snprintf(note, sizeof(note), "%s(\"%s\") %s %s", /* same text for history  */
             name, args, (status == 0) ? "=" : "failed:", out);
    conv_add(conv, ROLE_TOOL, note);      /* the result becomes part of context*/

    return status;                        /* let the caller phrase the reply   */
}

/* ==========================================================================
 * SECTION 5: THE MOCK MODEL
 * --------------------------------------------------------------------------
 * Stands in for a real LLM.  Its decision order is:
 *   1. the input mentions "hello"        -> return the hardcoded greeting
 *   2. the input looks like arithmetic   -> request the calc tool
 *   3. anything else                     -> echo the input back
 * It returns a heap-allocated string; the caller must free it with xfree.
 * ========================================================================== */

/* Copy the arithmetic part of a sentence into 'dest', dropping '?' and '!'. */
static void copy_expression(const char *src, char *dest, size_t destsz)
{
    size_t i = 0;                         /* write position inside dest        */
    for (; *src != '\0' && i + 1 < destsz; src++) { /* stop before overflowing */
        if (*src == '?' || *src == '!') { /* sentence punctuation is not math  */
            continue;                     /* so skip it entirely               */
        }
        dest[i++] = *src;                 /* keep everything else              */
    }
    dest[i] = '\0';                       /* terminate the copy                */
    trim_whitespace(dest);                /* tidy up the edges                 */
}

/* Decide whether the text starting at p is a complete arithmetic expression.
 * It must contain at least one digit AND one operator/function, and nothing
 * that is not part of arithmetic, so "2 apples + 3" is NOT treated as math. */
static int is_expression(const char *p)
{
    int has_digit = 0;                    /* did we see a number?              */
    int has_op    = 0;                    /* did we see an operator/function?  */
    while (*p != '\0') {                  /* classify every character          */
        if (isspace(uc(*p)) || *p == '?' || *p == '!') { /* ignorable          */
            p++;
        } else if (isdigit(uc(*p)) || *p == '.') {       /* part of a number   */
            has_digit = 1;
            p++;
        } else if (strchr("+-*/%^", *p) != NULL) {       /* an operator        */
            has_op = 1;
            p++;
        } else if (*p == '(' || *p == ')') {             /* grouping           */
            p++;
        } else if (strncmp(p, "sqrt", 4) == 0) {         /* a known function   */
            has_op = 1;
            p += 4;
        } else if (strncmp(p, "abs", 3) == 0) {          /* a known function   */
            has_op = 1;
            p += 3;
        } else {
            return 0;                     /* anything else: not arithmetic     */
        }
    }
    return has_digit && has_op;           /* both are required for a math call */
}

/* Find where the arithmetic starts inside a sentence such as
 * "what is 12 * 7?".  Returns NULL when there is no arithmetic at all. */
static const char *find_expression(const char *s)
{
    const char *p = s;                    /* scan cursor                       */
    for (; *p != '\0'; p++) {             /* try each starting position        */
        if (isdigit(uc(*p)) || *p == '(' ||          /* a number or a group... */
            strncmp(p, "sqrt", 4) == 0 ||            /* ...or a function call  */
            strncmp(p, "abs", 3) == 0) {
            if (is_expression(p)) {       /* does the rest parse as math?      */
                return p;                 /* yes: this is where it begins      */
            }
        }
        /* A leading sign belongs to the expression, so "-5 + 2" is -3 and not
         * 7.  We only accept the sign at the very start of the line or after a
         * space, so a hyphen glued to a word is never mistaken for math. */
        if ((*p == '-' || *p == '+') &&
            (p == s || isspace(uc(p[-1]))) &&
            is_expression(p)) {
            return p;                     /* the sign starts the expression    */
        }
    }
    return NULL;                          /* the sentence holds no arithmetic  */
}

/* The mock language model itself. */
static char *mock_model(Conversation *conv, const char *user_input)
{
    char reply[MAX_RESPONSE];             /* scratch space for the reply text  */
    const char *expr_start;               /* where arithmetic begins, if any   */

    /* --- Rule 1: a greeting anywhere in the sentence wins ----------------- */
    if (stristr(user_input, "hello") != NULL) {  /* case-insensitive search    */
        snprintf(reply, sizeof(reply),
                 "Hello! I am a mock language model running inside a small C "
                 "harness. Ask me to do some math, or type 'exit' to quit.");
        return xstrdup(reply);            /* caller frees this copy            */
    }

    /* --- Rule 2: arithmetic is delegated to the calc tool ----------------- */
    expr_start = find_expression(user_input);    /* look for a math expression */
    if (expr_start != NULL) {                    /* the model "decides" to call*/
        char expr[MAX_EXPR];                     /* cleaned-up expression      */
        char result[MAX_TOOL_OUTPUT];            /* whatever the tool reports  */
        int  status;                             /* tool success flag          */

        copy_expression(expr_start, expr, sizeof(expr)); /* strip '?' and '!'  */
        status = tool_run(conv, "calc", expr, result, sizeof(result)); /* run  */

        if (status == 0) {                       /* the tool computed a value  */
            snprintf(reply, sizeof(reply),
                     "I used the calc tool: %s = %s", expr, result);
        } else {                                 /* the tool reported an error */
            snprintf(reply, sizeof(reply),
                     "The calc tool could not evaluate \"%s\": %s",
                     expr, result);
        }
        return xstrdup(reply);                   /* caller frees this copy     */
    }

    /* --- Rule 3: everything else is echoed back --------------------------- */
    snprintf(reply, sizeof(reply), "You said: \"%s\"", user_input);
    return xstrdup(reply);                /* caller frees this copy            */
}

/* ==========================================================================
 * SECTION 6: HARNESS COMMANDS
 * --------------------------------------------------------------------------
 * Lines beginning with '/' never reach the model.  They control the harness
 * itself, which is what lets a user inspect and reset the stored context.
 * ========================================================================== */

/* Print the built-in help text. */
static void print_help(void)
{
    printf("Commands (handled by the harness, never sent to the model):\n");
    printf("  /help              show this text\n");
    printf("  /history           print the conversation currently in context\n");
    printf("  /stats             print context-window statistics\n");
    printf("  /clear             erase the stored conversation\n");
    printf("  /tools             list the tools the harness can execute\n");
    printf("  /tool NAME ARGS    run a tool directly, e.g. /tool calc 6*7\n");
    printf("  exit               quit the program (so does 'quit' or EOF)\n");
    printf("Anything else is sent to the mock model:\n");
    printf("  a line containing 'hello' gets a greeting;\n");
    printf("  a line containing arithmetic triggers the calc tool;\n");
    printf("  anything else is echoed back.\n");
}

/* Handle one '/' command.
 * Returns 0 if the line was not a command, 1 if it was handled,
 * and 2 if the command asks the program to quit. */
static int handle_command(Conversation *conv, const char *line)
{
    if (line[0] != '/') {                 /* not a command at all              */
        return 0;                         /* let the model see it              */
    }

    if (strcmp(line, "/help") == 0) {     /* /help                             */
        print_help();
        return 1;
    }
    if (strcmp(line, "/history") == 0) {  /* /history                          */
        conv_print(conv);
        return 1;
    }
    if (strcmp(line, "/stats") == 0) {    /* /stats                            */
        conv_stats(conv);
        return 1;
    }
    if (strcmp(line, "/clear") == 0) {    /* /clear: free every stored message */
        conv_clear(conv);
        printf("[context] cleared\n");    /* confirm, so the test can check it */
        return 1;
    }
    if (strcmp(line, "/tools") == 0) {    /* /tools                            */
        tool_list();
        return 1;
    }
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) { /* /exit   */
        return 2;                         /* signal the main loop to stop      */
    }
    if (strncmp(line, "/tool ", 6) == 0) {/* /tool NAME ARGS                   */
        char name[32];                    /* the tool name goes here           */
        const char *cursor = line + 6;    /* skip the "/tool " prefix          */
        size_t i = 0;                     /* write index into name[]           */

        while (*cursor != '\0' && isspace(uc(*cursor))) { /* skip extra spaces */
            cursor++;
        }
        while (*cursor != '\0' && !isspace(uc(*cursor)) &&
               i + 1 < sizeof(name)) {    /* copy the name, bounded            */
            name[i++] = *cursor++;
        }
        name[i] = '\0';                   /* terminate the name                */
        while (*cursor != '\0' && isspace(uc(*cursor))) { /* skip to the args  */
            cursor++;
        }
        if (name[0] == '\0') {            /* "/tool" with no name              */
            printf("[error] usage: /tool NAME ARGS\n");
        } else {
            char result[MAX_TOOL_OUTPUT]; /* buffer for the tool's output      */
            tool_run(conv, name, cursor, result, sizeof(result)); /* execute   */
        }
        return 1;
    }

    printf("[error] unknown command: %s (try /help)\n", line); /* typo case    */
    return 1;
}

/* ==========================================================================
 * SECTION 7: TERMINAL INPUT
 * ========================================================================== */

/* Read one line of any length using fgets, growing the buffer as needed.
 * Returns a heap string with the newline removed, or NULL at end of input.
 * The caller owns the returned pointer and must xfree it. */
static char *read_line(FILE *stream)
{
    size_t cap = INPUT_CHUNK;             /* current buffer size               */
    size_t len = 0;                       /* characters stored so far          */
    char  *buf = xmalloc(cap);            /* the growing buffer                */

    buf[0] = '\0';                        /* start out as an empty string      */

    for (;;) {                            /* loop until a full line is read    */
        if (fgets(buf + len, (int)(cap - len), stream) == NULL) { /* READ      */
            if (len == 0) {               /* end of input before any text      */
                xfree(buf);               /* nothing to return, so free it     */
                return NULL;              /* NULL tells the caller to stop     */
            }
            break;                        /* EOF right after text: keep it     */
        }
        len += strlen(buf + len);         /* account for what fgets appended   */

        if (len > 0 && buf[len - 1] == '\n') { /* a complete line was read     */
            buf[--len] = '\0';            /* drop the newline character        */
            break;                        /* the line is finished              */
        }
        if (len + 1 >= cap) {             /* the buffer filled up completely   */
            cap *= 2;                     /* double the capacity...            */
            buf = xrealloc(buf, cap);     /* ...and keep reading the same line */
            continue;
        }
        break;                            /* short read: EOF with no newline   */
    }
    return buf;                           /* caller owns and frees this string */
}

/* ==========================================================================
 * SECTION 8: MAIN -- THE AGENT LOOP
 * ========================================================================== */

int main(int argc, char **argv)
{
    Conversation conv;                        /* the entire conversation state */
    size_t max_history = DEFAULT_MAX_HISTORY; /* context window; may be overridden */
    int    show_banner = 1;                   /* printing the banner is default*/
    int    i;                                 /* argument index                */

    /* ---- parse the command line ----------------------------------------- */
    for (i = 1; i < argc; i++) {          /* argv[0] is the program name       */
        if (strcmp(argv[i], "--help") == 0) {           /* usage and exit      */
            printf("usage: %s [--max-history N] [--no-banner] [--version]\n",
                   argv[0]);
            print_help();
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) { /* version and exit    */
            printf("harness %s\n", HARNESS_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--no-banner") == 0) { /* quiet startup     */
            show_banner = 0;
        } else if (strcmp(argv[i], "--max-history") == 0 && i + 1 < argc) {
            long n = strtol(argv[++i], NULL, 10);       /* read the number     */
            if (n < 1) {                  /* a window smaller than 1 is useless*/
                fprintf(stderr, "harness: --max-history must be at least 1\n");
                return 1;
            }
            max_history = (size_t)n;      /* apply the requested window        */
        } else {                          /* anything unrecognized             */
            fprintf(stderr, "harness: unknown option '%s' (try --help)\n",
                    argv[i]);
            return 1;
        }
    }

    conv_init(&conv, max_history);        /* set up empty, valid context       */

    if (show_banner) {                    /* greet the user once at startup    */
        printf("=== Minimal LLM Agent Harness v%s ===\n", HARNESS_VERSION);
        printf("Context window: %lu messages. Type /help for commands, "
               "'exit' to quit.\n", (unsigned long)max_history);
    }

    /* ---- the main agent loop: read -> model -> tools -> print ------------ */
    for (;;) {                            /* the infinite loop, broken below   */
        char *line;                       /* one line typed by the user        */
        char *reply;                      /* the model's response              */
        int   command_result;             /* what handle_command decided       */

        printf("\nyou> ");                /* the prompt                        */
        fflush(stdout);                   /* force it out before we block      */

        line = read_line(stdin);          /* READ USER INPUT (uses fgets)      */
        if (line == NULL) {               /* end of input (Ctrl-D or pipe end) */
            printf("\n[harness] end of input\n");
            break;                        /* leave the loop and clean up       */
        }

        trim_whitespace(line);            /* ignore stray spaces and tabs      */

        if (line[0] == '\0') {            /* the user just pressed Enter       */
            xfree(line);                  /* free before looping, or we leak   */
            continue;                     /* nothing to send to the model      */
        }

        if (str_equals_ci(line, "exit") || str_equals_ci(line, "quit")) {
            xfree(line);                  /* free the line we are done with    */
            printf("[harness] exit requested\n");
            break;                        /* THE 'exit' KEYWORD ENDS THE LOOP  */
        }

        command_result = handle_command(&conv, line); /* harness-level commands*/
        if (command_result == 2) {        /* /exit was typed                   */
            xfree(line);
            printf("[harness] exit requested\n");
            break;
        }
        if (command_result == 1) {        /* handled; do not call the model    */
            xfree(line);
            continue;
        }

        conv_add(&conv, ROLE_USER, line); /* STORE the user turn in context    */

        reply = mock_model(&conv, line);  /* CALL THE MOCK MODEL               */

        printf("model> %s\n", reply);     /* SHOW the simulated response       */

        conv_add(&conv, ROLE_ASSISTANT, reply); /* STORE the model turn too    */

        xfree(reply);                     /* the conversation kept its own copy*/
        xfree(line);                      /* and we are finished with the line */
    }

    /* ---- shutdown: report state, then release every byte ----------------- */
    printf("\n[harness] session summary\n");
    conv_stats(&conv);                    /* final context-window statistics   */
    conv_free(&conv);                     /* FREE ALL CONVERSATION MEMORY      */

    /* The counters below come from xmalloc/xfree.  "outstanding" must be 0;
     * the test script greps for exactly this line. */
    printf("[memory] allocations=%lu frees=%lu outstanding=%lu\n",
           g_alloc_count, g_free_count, g_alloc_count - g_free_count);

    return 0;                             /* success                           */
}