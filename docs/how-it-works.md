# How acsh works

The README gives each part a paragraph. This page goes through them step by step: the life of one pipeline, job control, and the line editor. File names point at the code. Each file starts with a comment on what it does.

## Where the code is

```text
src/
  main.c        options, then the read, parse and run loop in script.c
  lexer.c       a line into words and operators
  parser.c      tokens into a tree
  expand.c      ~, $X and field splitting, then pattern.c for file names
  exec.c        the tree into processes: built-ins, pipes, fork and exec
  redirect.c    < > >> <& >&, and putting the shell's own fds back
  jobs.c        process groups, the terminal and the jobs table
  signals.c     Ctrl+C at the prompt, SIGCHLD and the terminal guard
  editor.c      the line editor, with history.c and complete.c
  explain.c     explain mode
  builtins.c    cd, echo, export, jobs, kill and the others
tools/          my own cat, head, wc, touch, mkdir and ls
tests/
  unit/         C tests on my own harness
  e2e/          pytest and pexpect, many compared with dash and bash
  fuzz/         the libFuzzer target and its corpus
docs/           this page, and the GIF in images/
```

## The life of a pipeline

This follows one line from the prompt to its exit status:

```text
ls *.txt | wc -l > n.txt
```

### 1. Reading

At a terminal the line editor reads the line (see below). From a pipe or a script, `input.c` reads one byte at a time until a newline. That's on purpose. A buffered read takes more than one line from a pipe. In `printf 'cat\nhello\n' | acsh`, the second line belongs to cat. A shell that read ahead would run `hello` as a command.

A line that isn't finished, like one with an open quote or one that ends in `|`, gets a `> ` prompt. The next line is added to it.

### 2. Lexing

`lexer.c` cuts the line into words and operators:

```text
[ls] [*.txt] | [wc] [-l] > [n.txt]
```

A word keeps its quotes and backslashes exactly as typed. `"a | b"` stays one word with its quotes. Digits right before `<` or `>` name a file descriptor, as in `2>err`. A `#` starts a comment only where a word would start. `echo a#b` prints `a#b`.

### 3. Parsing

`parser.c` turns the tokens into a tree:

```text
list
  and-or
    pipeline
      command  words: ls *.txt
      command  words: wc -l   redirections: 1> n.txt
```

A list holds and-or chains (`a && b || c`). Those hold pipelines. Pipelines hold commands. A command is simple, or it's an `if`, `while`, `until` or `for` with lists of its own. Reserved words like `if` only count where a command starts, and only unquoted. `"if"` is a command called if.

Nesting deeper than 1000 levels is a syntax error. Parsing, running and freeing all recurse once per level. A few hundred thousand levels would run out of stack.

### 4. Expansion

`expand.c` runs right before a command does. It goes in the order POSIX gives:

1. `~` becomes `$HOME`.
2. `$X`, `${X}`, `$?` and the other parameters get their values.
3. The result of an unquoted `$X` is split into words on IFS.
4. A word with an unquoted `*`, `?` or `[...]` becomes the file names that match it, sorted (`pattern.c`).
5. Quotes are removed.

While a word expands, every byte remembers where it came from. A byte typed outside quotes can act as `*`. One from inside quotes, or from a backslash, can't. One from an unquoted `$X` gets split on IFS. Nothing that comes out of a variable is ever parsed again. After `X='a; rm -rf ~'`, `echo $X` prints `a;`, `rm`, `-rf` and `~`. It runs nothing.

A pattern with no match stays as typed, like in dash. `ls *.nope` passes `*.nope` on. ls then says it doesn't exist.

### 5. Running

`exec.c` looks at the first command. A built-in on its own runs inside the shell. `cd` changes the shell's own folder. `exit` ends the shell. A child couldn't do either. Its redirections are undone afterwards. Before the shell changes an fd for the first time, it keeps a copy at fd 10 or above (`redirect.c`). The copy is close-on-exec. No command inherits it.

Everything else gets one child per command. This is what explain shows when a script runs the line. At the prompt, `setpgid` and `tcsetpgrp` lines come in between (see Job control below).

```text
pipe() = [3, 4]                          what goes into 4 comes out of 3
fork() = 445                             a child for ls
[445] dup2(4, 1)                         stdout goes into the pipe
[445] expand: ls *.txt  ->  ls notes.txt todo.txt
[445] execvp(ls notes.txt todo.txt)      becomes /usr/bin/ls, found in PATH
close(4)                                 the shell lets go of its end
fork() = 446                             a child for wc
[446] dup2(3, 0)                         stdin comes from the pipe
[446] open("n.txt", O_WRONLY|O_CREAT|O_TRUNC) = 3
[446] dup2(3, 1)                         stdout goes to n.txt
[446] close(3)
[446] execvp(wc -l)                      becomes /usr/bin/wc, found in PATH
close(3)                                 the shell lets go of its end
```

The shell only ever holds the read end of the previous pipe and the new pipe. It closes each end as soon as the child that needs it has it. A reader only sees the end of its input once every write end is closed. With one end left open in the shell, `yes | head -n 1` would never finish.

Each child expands its own words. `echo $X | cat` reads X in the child. The command's own redirections come after the pipe. `echo hi > f | cat` writes hi to f.

A child never calls `exit()`, only `_exit()`. `exit()` would flush the stdio buffers the child copied from the shell. The first version of acsh printed things twice for exactly that reason.

When exec fails, the child says why and gives the status bash gives. 127 means not found. 126 means found but not runnable, like a file without the x bit or a folder.

### 6. Waiting

The shell waits for every child, in pipeline order. The status of the pipeline is the last command's, as in POSIX sh without pipefail. A command killed by signal N gives 128 + N. Ctrl+C on `sleep` gives 130.

`explain` shows all of this as it happens. A child sends its lines through a pipe of its own. The shell reads it before it goes on. That pipe is close-on-exec. It reaches the end the moment exec works. The lines then come in the order things happened, even with several children.

## Job control

Job control only runs in an interactive shell (`jobs.c`). A script's commands stay in the script's own process group, as in every other shell.

### Process groups and the terminal

The terminal sends Ctrl+C, Ctrl+\ and Ctrl+Z to one process group, its foreground group. Only that group may read from it. acsh puts each pipeline into a process group of its own. It gives that group the terminal with `tcsetpgrp` while it runs. It takes the terminal back when the pipeline ends or stops.

```text
shell    pgid 100  (the terminal is here at the prompt)
ls | wc  pgid 4211 (ls is 4211, wc 4212; the terminal is here while they run)
```

The group is set with `setpgid` in the child and in the shell both. Whichever runs first, the group exists before the shell hands it the terminal or a second child joins it. The shell ignores SIGTSTP, SIGTTIN and SIGTTOU. Each child gets them back at their defaults before exec.

### Ctrl+Z, fg and bg

Ctrl+Z stops the whole group in front. The shell's `waitpid` sees the stop with WUNTRACED. The pipeline becomes a job:

```text
job 2 stopped: vim notes.txt
```

The shell saves the terminal's modes as the job left them, then puts its own back. `fg` gives the job its modes and the terminal again. Then it sends SIGCONT to the group. A stopped editor comes back as it was. `bg` only sends SIGCONT.

### Background jobs and their notices

`cmd &` starts a job without giving it the terminal. A job that tries to read from the terminal gets SIGTTIN and stops.

When a child ends, SIGCHLD arrives. The handler only writes one byte into a self-pipe. The prompt waits on the terminal and that pipe together. A byte in the pipe wakes it. The shell then collects every child that ended with `waitpid(-1, ..., WNOHANG)`. None of them stays a zombie. The notices wait until right before the next prompt:

```text
job 1 done: sleep 30 (exit 0)
```

### Ctrl+C at the prompt

Ctrl+C at the prompt should drop the line and show a new prompt. A handler that only interrupts `read()` isn't enough. A Ctrl+C that comes after the prompt is printed but before `read()` starts would be lost. acsh keeps SIGINT blocked instead. The prompt waits with `pselect`. It lets SIGINT in only while it waits, in one step. A Ctrl+C that comes early stays pending until then.

### Leaving

`exit` with stopped jobs names them and stays, the first time. A second `exit` right after leaves. Every job then gets SIGHUP. A stopped one gets SIGCONT too, to act on it. `disown` takes a job off the list. It gets no notice and no SIGHUP.

Without job control, in a script, a background command gets `/dev/null` as stdin. It ignores Ctrl+C and Ctrl+\ as POSIX says. Those only stop what's in front.

## The line editor

`editor.c` reads the line at the prompt when the terminal can draw it. It can't when TERM is `dumb` or unset, or when stdin isn't a terminal. Then the line is read plainly.

### Raw mode

While a line is typed, the terminal is in raw mode. There's no echo and no line buffering. Ctrl+C and Ctrl+Z are keys instead of signals. Ctrl+S and Ctrl+Q don't stop the output. The modes are set with TCSADRAIN. Keys typed ahead stay for the line. Raw mode ends before the line is returned. Commands always get the terminal as it was.

### Keys

| Keys | What they do |
|---|---|
| Left, Right, Home, End, Ctrl+A, Ctrl+E | move the cursor |
| Backspace, Delete | remove a character |
| Ctrl+K, Ctrl+U | cut to the end, cut to the start |
| Ctrl+W | cut the word before the cursor |
| Ctrl+L | clear the screen |
| Ctrl+C | drop the line |
| Ctrl+D | leave on an empty line, else Delete |
| Up, Down | older and newer lines from history |
| Ctrl+R | search back through history |
| Tab, Shift+Tab | complete the word, then go through the matches |

### Drawing the line

After a key the editor draws the line again. It goes up to the prompt's row and clears everything below. It writes the prompt's last line and the text. Then it moves the cursor to its place. A long line wraps over several rows. Text that ends right at the edge gets a newline first. Terminals hold the cursor in the last column until the next character.

A paste arrives as many keys at once. Drawing the whole line after each one takes time that grows with the square of its length. That was minutes for a few pages. The editor now asks with `poll` whether more input is waiting. While it is, the drawing waits too.

### History

`history.c` keeps the lines in `~/.acsh_history`, or in `$HISTFILE`. Each line goes into the file as it's entered, in one `write()` with O_APPEND. A killed acsh loses nothing. Two windows can't mix their lines. `$HISTSIZE` sets how many are kept, 1000 by default.

Up copies an entry into the line. Editing the copy never changes history. The line typed before the first Up comes back after the newest entry. Ctrl+R searches from the newest entry back as you type:

```text
search: sle  ->  sleep 30 &
```

Enter runs what's shown. An arrow key keeps it for editing. Ctrl+G brings back the line from before.

### Completion

`complete.c` finds the word under the cursor by walking the line from its start with the quote state. `'my no` is one word. A word where a command starts completes from the built-ins and the programs in PATH. Any other word completes from file names. The first Tab with several matches lists them once under the line, in columns like ls, at most 100. Tab and Shift+Tab then go through them. A name with special characters goes in single quotes.

### The terminal guard

A signal that ends acsh while the terminal is in raw mode would leave it unusable. `signals.c` guards the 17 catchable signals that end a process by default. Each one still at its default gets a handler with SA_RESETHAND. The handler puts the terminal's modes back with `tcsetattr`. Then it raises the signal again. acsh ends by that signal, as it would have. SIGKILL can't be caught. After it, `stty sane` fixes the terminal.
