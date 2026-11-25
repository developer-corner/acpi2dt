/* SPDX-License-Identifier: MIT */
/**
 * @file   dts_parser.h
 * @author Ingo A. Kubbilun
 * @brief  device tree source (DTS/DTSI) parser that preserves comments and
 *         raw property text, plus a merged tree builder that follows
 *         \#include / /include/ and applies &label overrides
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_DTS_PARSER_H
#define A2D_DTS_PARSER_H

#include "common.h"

/** normal property */
#define DT_DIR_NONE        0
/** /delete-property/ name; */
#define DT_DIR_DEL_PROP    1   /**< /delete-property/ name; */
#define DT_DIR_DEL_NODE    2   /**< /delete-node/ name; */
#define DT_DIR_PREPROC     3   /**< preprocessor line (\#if ...) inside a node */

/** @brief a DT property (raw value) or a directive inside a node */
typedef struct dt_prop {
  char           *name;  /**< property name (raw line for DT_DIR_PREPROC) */
  char           *value;        /**< raw text between '=' and ';' (NULL: boolean) */
  char           *comment;      /**< leading comment block(s), raw */
  bool            blank_before;  /**< an empty line precedes the property */
  int             directive;    /**< DT_DIR_* */
  int             line;  /**< source line */
  struct dt_prop *next;  /**< next property */
} dt_prop_t;

/** @brief a DT node, an override block (&label) or the root node */
typedef struct dt_node {
  char           *name;         /**< "i2c\@b80000", "/" for the root node */
  char           *ref;          /**< "&i2c0" / "&{/path}" for override blocks */
  strlist_t       labels;  /**< labels */
  char           *comment;      /**< leading comments */
  char           *tail_comment; /**< comments right before the closing brace */
  bool            blank_before;  /**< an empty line precedes the node */
  bool            omit_if_no_ref;  /**< /omit-if-no-ref/ was given */
  dt_prop_t      *props;  /**< first property */
  dt_prop_t      *props_tail;  /**< last property */
  struct dt_node *children;  /**< first child */
  struct dt_node *child_tail;  /**< last child */
  struct dt_node *next;  /**< next sibling */
  struct dt_node *parent;  /**< parent node */
  const char     *file;  /**< file of the (first) definition */
  int             line;  /**< line of the (first) definition */
  bool            deleted;  /**< removed by /delete-node/ */
  void           *user;         /**< annotation data of the merger */
} dt_node_t;

/** @brief kind of a top-level DTS item */
typedef enum {
  TOP_DIRECTIVE = 1,            /**< /dts-v1/; /plugin/; /memreserve/ ...; */
  TOP_INCLUDE,                  /**< \#include "x" / \#include \<x\> / /include/ "x" */
  TOP_PREPROC,                  /**< \#define, \#if, ... */
  TOP_NODE,                     /**< "/ { }" or "\&ref { }" */
  TOP_DELETE_NODE,              /**< /delete-node/ \&ref; */
} dt_top_kind_t;

/** @brief a top-level item of a DTS file */
typedef struct dt_top {
  dt_top_kind_t   kind;  /**< item kind */
  char           *text;         /**< raw text of directives / preprocessor lines */
  char           *include;      /**< include file name */
  bool            sys_include;  /**< <...> */
  char           *ref;          /**< TOP_DELETE_NODE target */
  dt_node_t      *node;         /**< TOP_NODE */
  char           *comment;  /**< leading comments */
  bool            blank_before;  /**< an empty line precedes the item */
  struct dt_top  *next;  /**< next item */
} dt_top_t;

/** @brief a parsed DTS/DTSI file (items in source order) */
typedef struct {
  char           *path;  /**< file path */
  dt_top_t       *items;  /**< first top-level item */
  dt_top_t       *tail;  /**< last top-level item */
  char           *trailing_comment;  /**< comments after the last item */
  int             errors;  /**< number of parse problems */
} dt_file_t;

/** @brief a merged device tree (includes followed, overrides applied) */
typedef struct {
  dt_node_t      *root;  /**< root node */
  strmap_t        labels;       /**< label -> dt_node_t* */
  strlist_t       files;        /**< all files loaded */
  strlist_t       missing;      /**< includes that could not be resolved */
} dt_tree_t;

/* parsing */
/**
 * @brief Parses a DTS/DTSI file.
 *
 * @param path  file path
 * @return parsed file or NULL if unreadable
 */
dt_file_t  *dt_parse_file(const char *path);
/**
 * @brief Parses DTS text.
 *
 * @param path  file name for messages
 * @param text  DTS text
 * @return parsed file (errors counted in ->errors)
 */
dt_file_t  *dt_parse_text(const char *path, const char *text);

/* merged tree: base_dirs are searched for "..." includes (after the including file's dir) */
/**
 * @brief Initializes an empty merged tree.
 *
 * @param t  tree
 */
void        dt_tree_init(dt_tree_t *t);
/**
 * @brief Parses a file and merges it (with its includes) into a tree.
 *
 * @param t      tree
 * @param path   file path
 * @param dirs   include search directories
 * @param ndirs  number of directories
 * @return false if the file cannot be read
 */
bool        dt_tree_add_file(dt_tree_t *t, const char *path, const char **dirs, int ndirs);
/**
 * @brief Merges a parsed file into a tree.
 *
 * @param t                tree
 * @param f                parsed file
 * @param dirs             include search directories
 * @param ndirs            number of directories
 * @param follow_includes  follow "..." includes
 */
void        dt_tree_add_parsed(dt_tree_t *t, const dt_file_t *f, const char **dirs, int ndirs, bool follow_includes);
/**
 * @brief Resolves an include name: directory of the including file first, then the search directories.
 *
 * @param name       include name
 * @param from_file  including file (may be NULL)
 * @param dirs       search directories
 * @param ndirs      number of directories
 * @param out        output buffer
 * @param cap        size of the output buffer
 * @return true if found
 */
bool        dt_resolve_include(const char *name, const char *from_file, const char **dirs, int ndirs,
                               char *out, size_t cap);

/* queries */
/**
 * @brief Looks up a node by label (deleted nodes are ignored).
 *
 * @param t      tree
 * @param label  label
 * @return node or NULL
 */
dt_node_t  *dt_tree_label(const dt_tree_t *t, const char *label);
/**
 * @brief Looks up a node by path.
 *
 * @param t     tree
 * @param path  path, e.g. "/soc@0/i2c\@b80000"
 * @return node or NULL
 */
dt_node_t  *dt_tree_path(const dt_tree_t *t, const char *path);
/**
 * @brief Resolves "&label" or "&{/path}".
 *
 * @param t    tree
 * @param ref  reference
 * @return node or NULL
 */
dt_node_t  *dt_tree_ref(const dt_tree_t *t, const char *ref);       /* "&x" or "&{/p}" */
/**
 * @brief Finds a child by name; a name without unit address matches a unique "name@...".
 *
 * @param n     node
 * @param name  child name
 * @return child or NULL
 */
dt_node_t  *dt_child(const dt_node_t *n, const char *name);
/**
 * @brief Finds a property (directives are ignored).
 *
 * @param n     node (may be NULL)
 * @param name  property name
 * @return property or NULL
 */
dt_prop_t  *dt_prop(const dt_node_t *n, const char *name);
/**
 * @brief Returns the raw value of a property.
 *
 * @param n     node
 * @param name  property name
 * @return raw value, "" for boolean properties, NULL if absent
 */
const char *dt_prop_value(const dt_node_t *n, const char *name);
/**
 * @brief Parses the unit address of a node name.
 *
 * @param n     node
 * @param addr  receives the address
 * @return true if the name has a hex unit address
 */
bool        dt_unit_address(const dt_node_t *n, uint64_t *addr);
/**
 * @brief Returns the full path of a node.
 *
 * @param n    node
 * @param out  output buffer
 * @param cap  size of the output buffer
 */
void        dt_node_path(const dt_node_t *n, char *out, size_t cap);
/**
 * @brief Checks the status property.
 *
 * @param n  node
 * @return true if status is absent or "okay"
 */
bool        dt_node_enabled(const dt_node_t *n);                   /* status absent or "okay" */
/**
 * @brief Returns the node name without unit address.
 *
 * @param n    node
 * @param out  output buffer
 * @param cap  size of the output buffer
 */
void        dt_node_basename(const dt_node_t *n, char *out, size_t cap); /* "i2c" from "i2c@b80000" */

/* value helpers */
/**
 * @brief Extracts the strings of a raw property value.
 *
 * @param raw  raw value
 * @param out  string list
 * @return number of strings
 */
int         dt_value_strings(const char *raw, strlist_t *out);         /* "a", "b" */
/**
 * @brief Extracts the tokens inside <...> of a raw property value.
 *
 * @param raw  raw value
 * @param out  string list
 * @return number of tokens
 */
int         dt_value_cells(const char *raw, strlist_t *out);           /* tokens inside <...> */
/**
 * @brief Returns the first number inside <...>.
 *
 * @param raw  raw value
 * @param v    receives the number
 * @return true if the first cell is a number
 */
bool        dt_value_first_u64(const char *raw, uint64_t *v);          /* first number in <..> */

/* node construction (used by the merger for generated nodes) */
/**
 * @brief Creates a node.
 *
 * @param name  node name (may be NULL)
 * @return new node
 */
dt_node_t  *dt_node_new(const char *name);
/**
 * @brief Appends a property to a node.
 *
 * @param n        node
 * @param name     property name
 * @param value    raw value (NULL: boolean)
 * @param comment  leading comment (may be NULL)
 * @return new property
 */
dt_prop_t  *dt_node_add_prop(dt_node_t *n, const char *name, const char *value, const char *comment);
/**
 * @brief Appends a child node.
 *
 * @param parent  parent node
 * @param child   child node
 */
void        dt_node_add_child(dt_node_t *parent, dt_node_t *child);
/**
 * @brief Deep-copies a node with properties and children.
 *
 * @param n  node
 * @return copy
 */
dt_node_t  *dt_node_clone(const dt_node_t *n);

#endif /* A2D_DTS_PARSER_H */
