/* A small strict JSON reader (RFC 8259) for swcc: a tree of nodes, each with its line and column, so every
   validation error can point at the place in the file. No dependencies beyond libc. */
#ifndef SWCC_JSON_H
#define SWCC_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JNode JNode;
struct JNode {
    JType type;
    int line, col;
    char *key;              /* member name when inside an object */
    char *str;              /* J_STR (UTF-8, unescaped) */
    double num;             /* J_NUM */
    int is_int;             /* J_NUM written without fraction / exponent */
    int b;                  /* J_BOOL */
    JNode *child, *next;    /* J_ARR / J_OBJ children, in file order */
    int n;                  /* number of children */
    int used;               /* set by the validator when a member is read: unknown keys = unused members */
};

/* Parses text; on error returns NULL and writes "line:col: message" into err. */
JNode *json_parse(const char *text, size_t len, char *err, size_t errn);
void json_free(JNode *n);

JNode *json_get(JNode *obj, const char *key);    /* member (marks it used), NULL if absent */
JNode *json_at(JNode *arr, int i);
const char *json_type_name(JType t);

#endif
