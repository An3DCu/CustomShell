#ifndef ACSH_VARS_H
#define ACSH_VARS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The shell's variables. This table is the only copy: the shell never calls
 * setenv or getenv. Variables from the environment acsh started with are
 * imported as exported, `X=1` makes a shell variable, `export X` marks it
 * for children, and right before exec the child builds its environment
 * from the exported ones (vars_environ).
 */

struct var {
    char *name;
    char *value;   /* NULL for `export X` when X has no value yet */
    bool exported;
};

struct vars {
    struct var *items;
    size_t len;
    size_t cap;
};

/* Imports every NAME=value entry of env (a NULL-terminated environ) as an
 * exported variable. Entries without a usable name are passed on to
 * children unchanged, but can't be read with $. */
void vars_init(struct vars *v, char **env);

void vars_free(struct vars *v);

/* The value, or NULL if the variable isn't set. */
const char *vars_get(const struct vars *v, const char *name);

/* Sets name (which must be a valid name) to value. A variable that was
 * exported stays exported. */
void vars_set(struct vars *v, const char *name, const char *value);

/* Marks name for export, creating it without a value if needed. */
void vars_export(struct vars *v, const char *name);

/* Removes name; fine if it isn't set. */
void vars_unset(struct vars *v, const char *name);

/* True if the variable exists, even without a value. */
bool vars_exists(const struct vars *v, const char *name);
bool vars_is_exported(const struct vars *v, const char *name);

/* A new NULL-terminated "NAME=value" array of the exported variables with a
 * value, for a child's environ. Free it with vars_free_environ. */
char **vars_environ(const struct vars *v);
void vars_free_environ(char **env);

/* The variables sorted by name, for `export -p`. A new array of pointers
 * into the table; free only the array. */
const struct var **vars_sorted(const struct vars *v, size_t *count);

/* How many bytes at the start of s form a variable name: a letter or _,
 * then letters, digits and _. 0 if s doesn't start with one. */
size_t var_name_len(const char *s);

/* True if all of s is a variable name. */
bool var_name_valid(const char *s);

#endif
