# CustomShell

acsh is a small Unix shell in C, with its own parser, globbing and job control.

## Limits

- The line editor moves the cursor one byte at a time. A letter like č is two bytes. It takes two presses of Left or Backspace.
