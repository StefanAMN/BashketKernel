/**
 * @file ctype.c
 * @brief Implementation of Character Handling Functions
 */

#include "ctype.h"

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c; }
