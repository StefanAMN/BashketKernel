/* Shell wildcards: does a name match a pattern?
 *
 *   *        any run of characters, also none
 *   ?        any one character
 *   [abc]    one of the characters listed; [a-z] a range; [!abc] none of them
 *
 * Any other character matches itself. A name starting with '.' is matched
 * only by a pattern starting with '.' (so "*" does not list hidden files),
 * as in POSIX sh. */
#ifndef USER_GLOB_H
#define USER_GLOB_H

/* 1 if `name` matches all of `pattern`, 0 if not. An unclosed '[' stands
 * for itself. */
int glob_match(const char *pattern, const char *name);

/* 1 if `word` contains a wildcard character (*, ? or [), 0 if not */
int glob_has_wildcard(const char *word);

#endif
