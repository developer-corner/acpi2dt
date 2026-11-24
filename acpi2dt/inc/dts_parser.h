/**
 * @file   dts_parser.h
 * @author Ingo A. Kubbilun
 * @brief  device tree source (DTS/DTSI) parser that preserves comments and
 *         raw property text, plus a merged tree builder that follows
 *         #include / /include/ and applies &label overrides
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_DTS_PARSER_H
#define A2D_DTS_PARSER_H

#include "common.h"

#define DT_DIR_NONE        0
#define DT_DIR_DEL_PROP    1   /* /delete-property/ name;  */
#define DT_DIR_DEL_NODE    2   /* /delete-node/ name;      */
#define DT_DIR_PREPROC     3   /* #if ... inside a node    */

typedef struct dt_prop {
  char           *name;
  char           *value;        /* raw text between '=' and ';' (NULL: boolean) */
  char           *comment;      /* leading comment block(s), raw                */
  bool            blank_before;
  int             directive;    /* DT_DIR_*                                     */
  int             line;
  struct dt_prop *next;
} dt_prop_t;

typedef struct dt_node {
  char           *name;         /* "i2c@b80000", "/" for the root node          */
  char           *ref;          /* "&i2c0" / "&{/path}" for override blocks     */
  strlist_t       labels;
  char           *comment;      /* leading comments                             */
  char           *tail_comment; /* comments right before the closing brace      */
  bool            blank_before;
  bool            omit_if_no_ref;
  dt_prop_t      *props, *props_tail;
  struct dt_node *children, *child_tail;
  struct dt_node *next, *parent;
  const char     *file;
  int             line;
  bool            deleted;
  void           *user;         /* annotation data of the merger                */
} dt_node_t;

typedef enum {
  TOP_DIRECTIVE = 1,            /* /dts-v1/; /plugin/; /memreserve/ ...;        */
  TOP_INCLUDE,                  /* #include "x" / #include <x> / /include/ "x"  */
  TOP_PREPROC,                  /* #define, #if, ...                            */
  TOP_NODE,                     /* "/ { }" or "&ref { }"                        */
  TOP_DELETE_NODE,              /* /delete-node/ &ref;                          */
} dt_top_kind_t;

typedef struct dt_top {
  dt_top_kind_t   kind;
  char           *text;         /* raw text of directives / preprocessor lines  */
  char           *include;      /* include file name                            */
  bool            sys_include;  /* <...>                                        */
  char           *ref;          /* TOP_DELETE_NODE target                       */
  dt_node_t      *node;         /* TOP_NODE                                     */
  char           *comment;
  bool            blank_before;
  struct dt_top  *next;
} dt_top_t;

typedef struct {
  char           *path;
  dt_top_t       *items, *tail;
  char           *trailing_comment;
  int             errors;
} dt_file_t;

typedef struct {
  dt_node_t      *root;
  strmap_t        labels;       /* label -> dt_node_t*                          */
  strlist_t       files;        /* all files loaded                             */
  strlist_t       missing;      /* includes that could not be resolved          */
} dt_tree_t;

/* parsing */
dt_file_t  *dt_parse_file(const char *path);
dt_file_t  *dt_parse_text(const char *path, const char *text);

/* merged tree: base_dirs are searched for "..." includes (after the including file's dir) */
void        dt_tree_init(dt_tree_t *t);
bool        dt_tree_add_file(dt_tree_t *t, const char *path, const char **dirs, int ndirs);
void        dt_tree_add_parsed(dt_tree_t *t, const dt_file_t *f, const char **dirs, int ndirs, bool follow_includes);
bool        dt_resolve_include(const char *name, const char *from_file, const char **dirs, int ndirs,
                               char *out, size_t cap);

/* queries */
dt_node_t  *dt_tree_label(const dt_tree_t *t, const char *label);
dt_node_t  *dt_tree_path(const dt_tree_t *t, const char *path);
dt_node_t  *dt_tree_ref(const dt_tree_t *t, const char *ref);       /* "&x" or "&{/p}" */
dt_node_t  *dt_child(const dt_node_t *n, const char *name);
dt_prop_t  *dt_prop(const dt_node_t *n, const char *name);
const char *dt_prop_value(const dt_node_t *n, const char *name);
bool        dt_unit_address(const dt_node_t *n, uint64_t *addr);
void        dt_node_path(const dt_node_t *n, char *out, size_t cap);
bool        dt_node_enabled(const dt_node_t *n);                   /* status absent or "okay" */
void        dt_node_basename(const dt_node_t *n, char *out, size_t cap); /* "i2c" from "i2c@b80000" */

/* value helpers */
int         dt_value_strings(const char *raw, strlist_t *out);         /* "a", "b" */
int         dt_value_cells(const char *raw, strlist_t *out);           /* tokens inside <...> */
bool        dt_value_first_u64(const char *raw, uint64_t *v);          /* first number in <..> */

/* node construction (used by the merger for generated nodes) */
dt_node_t  *dt_node_new(const char *name);
dt_prop_t  *dt_node_add_prop(dt_node_t *n, const char *name, const char *value, const char *comment);
void        dt_node_add_child(dt_node_t *parent, dt_node_t *child);
dt_node_t  *dt_node_clone(const dt_node_t *n);

#endif /* A2D_DTS_PARSER_H */
