/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Yonatan Goldschmidt
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gcc-plugin.h>
#include <tree.h>
#include <tree-nested.h>
#include <print-tree.h>
#include <tree-iterator.h>
#include <c-family/c-common.h>
#include <c-tree.h>
#include <plugin-version.h>

#define PLUGIN_NAME "assert_introspect"

#define RESET_COLOR "\x1b[0m"
#define BOLD "\x1b[1m"
#define DARK "\x1b[2m"
#define RED "\x1b[31m"
#define GREEN "\x1b[32m"
#define YELLOW "\x1b[33m"
#define BLUE "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define CYAN "\x1b[36m"
#define BOLD_RED(s) BOLD RED  s RESET_COLOR
#define BOLD_BLUE(s) BOLD BLUE s RESET_COLOR

int plugin_is_GPL_compatible; // must be defined for the plugin to run

static tree printf_decl = NULL_TREE;
static tree abort_decl = NULL_TREE;
static tree sprintf_decl = NULL_TREE;

// based on build_function_call(), with some fixes.
// see https://gcc.gnu.org/pipermail/gcc/2020-April/232127.html for explanation.
tree my_build_function_call(location_t loc, tree function, tree params)
{
    const int len = list_length(params);

    vec<tree, va_gc> *v;
    auto_vec<location_t> argloc(len);
    tree ret;

    vec_alloc(v, len);
    for (; params; params = TREE_CHAIN(params)) {
        tree param = TREE_VALUE(params);
        v->quick_push(param);
        argloc.quick_push(EXPR_HAS_LOCATION(param) ? EXPR_LOCATION(param) : loc);
    }

    ret = c_build_function_call_vec(loc, argloc, function, v, NULL);
    vec_free(v);

    return ret;
}

// heuristic to check if cond_expr is the expression generated by glibc's "assert" macro, which is:
// ((void) sizeof ((EXPR) ? 1 : 0), __extension__ ({ if (EXPR) ; else __assert_fail ("EXPR", "file.c", line, __extension__ __PRETTY_FUNCTION__); }))
// the generated cond_expr has COND_EXPR_COND as the assert's condition,
// COND_EXPR_THEN is a nop_expr and COND_EXPR_ELSE is a call_expr calling to "__assert_fail".
static bool is_assert_fail_cond_expr(tree expr) {
    if (TREE_CODE(expr) != COND_EXPR) {
        return false;
    }

    tree expr_else = COND_EXPR_ELSE(expr);
    return (
        TREE_CODE(COND_EXPR_THEN(expr)) == NOP_EXPR &&
        TREE_CODE(expr_else) == CALL_EXPR &&
        TREE_CODE(CALL_EXPR_FN(expr_else)) == ADDR_EXPR &&
        TREE_CODE(TREE_OPERAND(CALL_EXPR_FN(expr_else), 0)) == FUNCTION_DECL &&
        0 == strcmp("__assert_fail", IDENTIFIER_POINTER(DECL_NAME(TREE_OPERAND(CALL_EXPR_FN(expr_else), 0))))
    );
}

// lol
#define build_string_literal_from_literal(here, s) build_string_literal_here((here), sizeof(s), (s))

static tree build_string_literal_here(location_t here, int len, const char *str) {
    tree t = build_string_literal(len, str);
    SET_EXPR_LOCATION(t, here);
    return t;
}

static inline tree _build_conditional_expr(location_t colon_loc, tree ifexp,
    tree op1, tree op1_original_type, tree op2, tree op2_original_type) {

#if GCCPLUGIN_VERSION >= 8001 // a32c8316ff282ec
    return build_conditional_expr(colon_loc, ifexp, false, op1, op1_original_type,
        colon_loc, op2, op2_original_type, colon_loc);
#else
    return build_conditional_expr(colon_loc, ifexp, false, op1, op1_original_type,
        op2, op2_original_type);
#endif
}

// get textual repr of expr's operation, if it's a logical / mathematical operation.
static const char *get_expr_op_repr(tree expr) {
    const char *op;

    switch (TREE_CODE(expr)) {
    case EQ_EXPR: op = "=="; break;
    case NE_EXPR: op = "!="; break;
    case LT_EXPR: op = "<"; break;
    case LE_EXPR: op = "<="; break;
    case GT_EXPR: op = ">"; break;
    case GE_EXPR: op = ">="; break;
    // about TRUTH_AND_EXPR and TRUTH_OR_EXPR: the docs say:
    // "There are no corresponding operators in C or C++, but the front end will sometimes generate these expressions anyhow"
    // I witnessed them generated and so I handle them here.
    // their different semantics about evaluation may require special handling, but since my current repr logic
    // follows short circuiting rules, I think it's alright (I mean, my rules are stricter).
    case TRUTH_AND_EXPR:
    case TRUTH_ANDIF_EXPR: op = "&&"; break;
    case TRUTH_OR_EXPR:
    case TRUTH_ORIF_EXPR: op = "||"; break;
    case PLUS_EXPR: op = "+"; break;
    case MINUS_EXPR: op = "-"; break;
    case MULT_EXPR: op = "*"; break;
    case TRUNC_DIV_EXPR: op = "/"; break;
    case TRUNC_MOD_EXPR: op = "%"; break;
    default: op = NULL; break;
    }

    return op;
}

// different from tree_strip_nop_conversions and its friends - this strips away all NOPs.
// it's invalid to use the *value* of the inner expression, because those NOPs may express
// real casts.
// it's used to check the inner type & access its other fields.
static tree strip_nop_and_convert(tree expr) {
    if (TREE_CODE(expr) == NOP_EXPR) {
        expr = TREE_OPERAND(expr, 0);
    }
    if (CONVERT_EXPR_P(expr)) { // CONVERT_EXPR may follow
        expr = TREE_OPERAND(expr, 0);
    }

    return expr;
}

static void wrap_in_save_expr(tree *expr) {
    // use get_expr_op_repr as a predicate: "is it a binary expression with 2 args"
    // TODO this probably doesn't catch all cases + doesn't catch unary ops (whose inner value
    // should be made a SAVE_EXPR as well)

    // strip leading cast, for example promotion.
    tree inner = strip_nop_and_convert(*expr);
    if (get_expr_op_repr((inner)) != NULL) {
        wrap_in_save_expr(&TREE_OPERAND(inner, 0));
        wrap_in_save_expr(&TREE_OPERAND(inner, 1));
    }
    // however, expression with the cast is the one we save.
    *expr = save_expr(*expr);
}

// gets the STRING_CST tree of argument 'n' passed to 'function'.
static tree get_string_cst_arg(tree function, int n) {
    tree arg = CALL_EXPR_ARG(function, n);
    if (CONVERT_EXPR_P(arg)) {
        arg = TREE_OPERAND(arg, 0);
    }
    gcc_assert(TREE_CODE(arg) == ADDR_EXPR);

    arg = TREE_OPERAND(arg, 0);
    gcc_assert(TREE_CODE(arg) == STRING_CST);

    // TREE_STRING_LENGTH should include the null terminator
    gcc_assert(TREE_STRING_POINTER(arg)[TREE_STRING_LENGTH(arg) - 1] == '\0');

    return arg;
}

// create a printf("> assert(EXPR)\n") call (for the original assert() expression)
static void make_assert_expr_printf(location_t here, tree call__assert_fail, tree *stmts) {
    tree file_arg = get_string_cst_arg(call__assert_fail, 1);
    tree function_arg = CALL_EXPR_ARG(call__assert_fail, 3);
    tree line_arg = CALL_EXPR_ARG(call__assert_fail, 2);

    char buf[1024];
    // function_arg (that is, __PRETTY_FUNCTION__) is a variable, not a string constant.
    // that's why you can't do e.g `printf("hello from " __PRETTY_FUNCTION__);`.
    // for the same reason, we can't easily include it into the sprintf here (in compile time).
    // I guess there are other ways to get the current function name at this point, but meh,
    // we'll just print it in runtime.
    // if this snprintf ever exceeds... not bothering to check it
    (void)snprintf(buf, sizeof(buf), "In %s:%ld, function '%%s':\n",
        TREE_STRING_POINTER(file_arg), TREE_INT_CST_LOW(line_arg));

    tree header_line = build_string_literal_here(here, strlen(buf) + 1, buf);
    append_to_statement_list(my_build_function_call(here, printf_decl,
        tree_cons(NULL_TREE, header_line,
        tree_cons(NULL_TREE, function_arg, NULL_TREE))), stmts);

    tree format_str = build_string_literal_from_literal(here, "> assert(%s)\n");
    append_to_statement_list(my_build_function_call(here, printf_decl,
        tree_cons(NULL_TREE, format_str,
        // can just use the original argument directly in our call..
        tree_cons(NULL_TREE, CALL_EXPR_ARG(call__assert_fail, 0), NULL_TREE))), stmts);
}

// sets up the repr buffer we'll use later as a variable and registers it to the scope of "block".
// also initializes it.
// this is essentialy: `char repr_buf[1024]; repr_buf[0] = 0;`
static void set_up_repr_buf(location_t here, tree stmts, tree block, tree *buf_param, tree *buf_pos) {
    const int REPR_BUF_SIZE = 1024;

    tree buf_type = build_array_type(char_type_node,
        // -1 because build_index_type accepts the maximum index for the array
        build_index_type(size_int(REPR_BUF_SIZE - 1)));

    tree buf = build_decl(here, VAR_DECL, NULL_TREE, buf_type);
    DECL_ARTIFICIAL(buf) = 1; // mark it as generated by the compiler
    DECL_CONTEXT(buf) = current_function_decl;
    finish_decl(buf, here, NULL_TREE, NULL_TREE, NULL_TREE);

    // add a MODIFY_EXPR to initialize the buffer.
    // I couldn't get DECL_INITIAL to work here. but anyway it makes more sense to set the first
    // element to '\0' instead of intializing the entire array.
    tree modify = build_modify_expr(here, build_array_ref(here, buf, integer_zero_node), NULL_TREE,
        NOP_EXPR, here, integer_zero_node, NULL_TREE);
    append_to_statement_list(modify, &stmts);

    *buf_param = build1_loc(here, NOP_EXPR, build_pointer_type(char_type_node), build_addr(buf));

    tree pos = build_decl(here, VAR_DECL, NULL_TREE, TYPE_DOMAIN(buf_type));
    DECL_ARTIFICIAL(pos) = 1;
    DECL_CONTEXT(pos) = current_function_decl;
    finish_decl(pos, here, NULL_TREE, NULL_TREE, NULL_TREE);
    *buf_pos = pos;

    append_to_statement_list(build_modify_expr(here, pos, NULL_TREE, NOP_EXPR, here,
        integer_zero_node, NULL_TREE), &stmts);

    BLOCK_VARS(block) = chainon(BLOCK_VARS(block), buf);
    BLOCK_VARS(block) = chainon(BLOCK_VARS(block), pos);
}

static tree from_save(tree save_expr) {
    gcc_assert(TREE_CODE(save_expr) == SAVE_EXPR);
    return TREE_OPERAND(save_expr, 0);
}

static bool is_save_equivalent(tree expr) {
    // save_expr() has some rules: it doesn't wrap constants, doesn't wrap read-only expressions
    // without side effects, ...
    // easiest way to check if `expr` is compatible to a SAVE_EXPR is to call save_expr
    // and see if it returns a new result. if the expression is compatible, it just returns
    // the expression.
    return save_expr(expr) == expr;
}

static tree from_save_maybe(tree expr) {
    if (TREE_CODE(expr) == SAVE_EXPR) {
        return from_save(expr);
    }

    gcc_assert(is_save_equivalent(expr));
    return expr;
}

static void assert_tree_is_save(tree expr) {
    gcc_assert(TREE_CODE(expr) == SAVE_EXPR || is_save_equivalent(expr));
}

static const char *get_int_type_name(tree expr) {
    tree type = TREE_TYPE(expr);
    gcc_assert(INTEGRAL_TYPE_P(type));

    if (TYPE_IDENTIFIER(type) != NULL_TREE) {
        return IDENTIFIER_POINTER(TYPE_IDENTIFIER(type));
    } else if (TYPE_UNSIGNED(type)) {
        return "unsigned";
    } else {
        return "int";
    }
}

static char *get_cast_repr(tree expr) {
    char buf[1024];
    int n = 0;

    if (TREE_CODE(expr) == NOP_EXPR) {
        n += snprintf(buf, sizeof(buf), "(%s)", get_int_type_name(expr));
        expr = TREE_OPERAND(expr, 0);
    }

    if (CONVERT_EXPR_P(expr)) {
        n += snprintf(buf + n, sizeof(buf) - n, "(%s)", get_int_type_name(expr));
    }

    return n ? xstrdup(buf) : NULL;
}

// can't use GCC's build_tree_list - these lists use TREE_CHAIN to link entries,
// but we can't override the TREE_CHAIN of existing exprs.
// so use this lousy list instead
struct expr_list {
    tree expr;
    const char *color;
    struct expr_list *next;
};

static struct expr_list *get_expr_list_item(tree expr, struct expr_list *list) {
    gcc_assert(list);
    list = list->next; // skip dummy

    while (list) {
        // compare inner expressions - save expressions are generated separately each time
        // an expression is met in the AST, so if a variable is used multiple times it'll have multiple
        // SAVE_EXPRs. but still only one DECL. note: it sounds weird, I actually read this comment
        // after a while and thought it's wrong. it isn't. if an expression (such as a variable decl)
        // is used multiple times in the AST, those separate instances will have different save_exprs
        // wrapping them, because the expression is indeed evaluated multiple times. for variables, those
        // are deemed "equal" here. for others (like function calls) - the CALL_EXPR will be different
        // if it's really a different call, so we're good.
        if (from_save_maybe(list->expr) == from_save_maybe(expr)) {
            return list;
        }

        list = list->next;
    }

    return NULL;
}

static const char *get_subexpr_color(tree expr, struct expr_list *ec) {
    struct expr_list *list = get_expr_list_item(expr, ec);
    if (list) {
        return list->color;
    }

    return NULL;
}

static void add_subexpr_color(tree expr, const char *color, struct expr_list *ec) {
    struct expr_list *new_ec = (struct expr_list*)xmalloc(sizeof(*new_ec));
    new_ec->expr = expr;
    new_ec->color = color;
    // order doesn't really matter in this list.
    new_ec->next = ec->next;
    ec->next = new_ec;
}

static void free_expr_list(struct expr_list *ec) {
    ec = ec->next; // skip dummy

    while (ec) {
        struct expr_list *next = ec->next;
        free(ec);
        ec = next;
    }
}

// NULL, defined as `(void*)0`, is an INTEGER_CST with type as POINTER_TYPE,
// pointing to "char" with string-flag set. I couldn't find a way to separate those
// from real string pointers, so this function comes in help.
static bool is_NULL(tree expr) {
    return integer_zerop(expr) &&
        POINTER_TYPE_P(TREE_TYPE(expr)) &&
        TYPE_STRING_FLAG(TREE_TYPE(TREE_TYPE(expr)));
}

static const char *get_format_for_expr(tree expr) {
    // this helps asserting we use the outer expression here, not the inner one (after e.g strip_nop_and_convert)
    // beause we should pick a specifier for *after* the casts.
    assert_tree_is_save(expr);

    // it's okay to use the TREE_TYPE of SAVE_EXPR directly.
    tree type = TREE_TYPE(expr);
    if (POINTER_TYPE_P(type)) {
        // check if pointed type is marked with "string-flag"
        tree pointed_type = TREE_TYPE(type);
        if (TYPE_STRING_FLAG(pointed_type) && !is_NULL(expr)) {
            // assume it's also null terminated :) and a valid string, anyway.
            return "\"%s\"";
        } else {
            return "%p";
        }
    } else if (TREE_CODE(type) == BOOLEAN_TYPE) {
        return "%d";
    } else if (INTEGRAL_TYPE_P(type)) {
        if (TYPE_IDENTIFIER(type) == NULL_TREE) {
            // default int? :o
            return TYPE_UNSIGNED(type) ? "%u" : "%d";
        }

        const char *type_name = IDENTIFIER_POINTER(TYPE_IDENTIFIER(type));
        if (0 == strcmp(type_name, "int")) {
            return "%d";
        } else if (0 == strcmp(type_name, "unsigned int")) {
            return "%u";
        } else if (0 == strcmp(type_name, "long int")) {
            return "%ld";
        } else if (0 == strcmp(type_name, "long unsigned int")) {
            return "%lu";
        } else if (0 == strcmp(type_name, "short int")) {
            return "%hd";
        } else if (0 == strcmp(type_name, "short unsigned int")) {
            return "%hu";
        } else {
            printf("unknown integer type name '%s'\n", type_name);
        }
    }

    gcc_unreachable();
}

static char *_make_assert_expr_printf_from_ast(tree expr, struct expr_list *ec) {
    char buf[1024];

    tree unsaved = from_save_maybe(expr);
    tree inner = strip_nop_and_convert(unsaved);
    const char *op = get_expr_op_repr(inner);
    if (op) {
        char *left = _make_assert_expr_printf_from_ast(TREE_OPERAND(inner, 0), ec);
        char *right = _make_assert_expr_printf_from_ast(TREE_OPERAND(inner, 1), ec);

        // TODO show casts on binary expressions
        bool parentheses = 0 == strcmp(op, "&&") || 0 == strcmp(op, "||");
        const char *left_paren = parentheses ? "(" : "";
        const char *right_paren = parentheses ? ")" : "";
        (void)snprintf(buf, sizeof(buf), "%s%s%s %s %s%s%s",
            left_paren, left, right_paren, op, left_paren, right, right_paren);

        free(left);
        free(right);
        return xstrdup(buf);
    } else {
        if (DECL_P(inner)) {
            char *cast = get_cast_repr(unsaved);
            const char *color = get_subexpr_color(expr, ec);
            (void)snprintf(buf, sizeof(buf), "%s%s%s%s", color ?: "", cast ?: "",
                IDENTIFIER_POINTER(DECL_NAME(inner)), color ? RESET_COLOR : "");
            free(cast);
            return xstrdup(buf);
        } else if (TREE_CODE(inner) == CALL_EXPR) {
            // TODO show casts on function calls
            tree fn = get_callee_fndecl(inner);
            const char *fn_name = IDENTIFIER_POINTER(DECL_NAME(fn));
            const char *color = get_subexpr_color(expr, ec);

            int n = snprintf(buf, sizeof(buf), "%s%s(", color ?: "", fn_name);

            for (int i = 0; i < call_expr_nargs(inner); i++) {
                tree arg = CALL_EXPR_ARG(inner, i);
                char *arg_repr = _make_assert_expr_printf_from_ast(arg, ec);
                const char *arg_color = get_subexpr_color(arg, ec);
                n += snprintf(buf + n, sizeof(buf) - n, "%s%s, ", arg_repr,
                    // reinsert our color if arg itself had one (had a color in its arg_repr)
                    arg_color ? color : "");
            }

            n -= 2; // (remove last ", ")
            (void)snprintf(buf + n, sizeof(buf) - n, ")%s", color ? RESET_COLOR : "");

            return xstrdup(buf);
        } else if (is_NULL(unsaved)) { // before INTEGER_CST, "NULL" is INTEGER_CST itself.
            return xstrdup("NULL");
        } else if (TREE_CODE(inner) == INTEGER_CST) {
            gcc_assert(TREE_INT_CST_NUNITS(inner) == 1); // TODO handle greater
            (void)snprintf(buf, sizeof(buf), get_format_for_expr(inner), TREE_INT_CST_LOW(inner));
            return xstrdup(buf);
        } else if (TREE_CODE(inner) == ADDR_EXPR) {
            tree addr_inner = TREE_OPERAND(inner, 0);
            if (TREE_CODE(addr_inner) == STRING_CST) {
                // can't use get_format_for_expr() here
                (void)snprintf(buf, sizeof(buf), "\"%s\"", TREE_STRING_POINTER(addr_inner));
                return xstrdup(buf);
            } else {
                // handle &variable
                gcc_assert(DECL_P(addr_inner));
                (void)snprintf(buf, sizeof(buf), "&%s", IDENTIFIER_POINTER(DECL_NAME(addr_inner)));
                return xstrdup(buf);
            }
        }

        gcc_unreachable();
    }
}

// combination of make_assert_expr_printf and make_conditional_expr_repr:
// this prints the "expression text" without evaluation anything (like make_assert_expr_printf),
// but it generates this text from AST (like make_conditional_expr_repr)
static void make_assert_expr_printf_from_ast(location_t here, tree cond_expr, struct expr_list *ec, tree *stmts) {
    char *expr_text = _make_assert_expr_printf_from_ast(cond_expr, ec);

    char buf[1024];
    snprintf(buf, sizeof(buf), BOLD_BLUE("A") " assert(%s)\n", expr_text);
    free(expr_text);

    append_to_statement_list(my_build_function_call(here, printf_decl,
        tree_cons(NULL_TREE, build_string_literal_here(here, strlen(buf) + 1, buf), NULL_TREE)),
        stmts);
}

static const char *SUBEXPR_COLORS[] = {
    BOLD GREEN,
    BOLD YELLOW,
    BOLD MAGENTA,
    BOLD CYAN,
    DARK RED,
    DARK BLUE,
    DARK GREEN,
    DARK YELLOW,
    // enough
};

// unites all "common" parameters of make_conditional_expr_repr so we don't have to pass
// them each call.
struct make_repr_params {
    location_t here;
    tree buf_param;
    tree buf_pos;
    tree call_buf_param;
    tree call_buf_pos;
    size_t color_idx;
    struct expr_list decl_repr_exprs;
    struct expr_list subexpr_colors;
};

// returns next usable color by subexpr reprs.
static const char *alloc_subexpr_color(struct make_repr_params *params) {
    if (params->color_idx < ARRAY_SIZE(SUBEXPR_COLORS)) {
        return SUBEXPR_COLORS[params->color_idx++];
    }

    return NULL;
}

static const char *append_decl_subexpression_repr(tree expr, tree raw_expr, struct make_repr_params *params) {
    gcc_assert(DECL_P(raw_expr));

    // don't add if found
    struct expr_list *list = get_expr_list_item(expr, &params->decl_repr_exprs);
    if (list) {
        return list->color;
    }

    struct expr_list *new_list = (struct expr_list *)xmalloc(sizeof(*new_list));
    new_list->expr = expr;
    new_list->next = NULL;
    new_list->color = alloc_subexpr_color(params);
    if (new_list->color) {
        add_subexpr_color(expr, new_list->color, &params->subexpr_colors);
    }

    // add it last - so order is kept.
    list = &params->decl_repr_exprs;
    while (list->next) {
        list = list->next;
    }
    list->next = new_list;

    return new_list->color;
}

static void make_decl_subexpressions_repr(location_t here, struct expr_list *list, tree *stmts) {
    list = list->next; // skip first

    while (list) {
        tree expr = list->expr;
        tree unsaved = from_save_maybe(expr);
        tree raw_expr = strip_nop_and_convert(unsaved);
        char *cast = get_cast_repr(unsaved);

        char buf[1024];
        (void)snprintf(buf, sizeof(buf), "  %s%s%s = %s%s\n", list->color ?: "",
            cast ?: "", IDENTIFIER_POINTER(DECL_NAME(raw_expr)), get_format_for_expr(expr),
            list->color ? RESET_COLOR : "");
        free(cast);

        tree printf_call = my_build_function_call(here, printf_decl,
            tree_cons(NULL_TREE, build_string_literal_here(here, strlen(buf) + 1, buf),
            tree_cons(NULL_TREE, expr, NULL_TREE)));

        append_to_statement_list(printf_call, stmts);

        struct expr_list *next = list->next;
        free(list);
        list = next;
    }
}

static tree make_buf_ref_addr(location_t here, tree buf_param, tree buf_pos) {
    return build_addr(build_array_ref(here, buf_param, buf_pos));
}

// creats a `buf_pos += sprintf(...)` with given arguments.
static tree make_repr_sprintf(location_t here, tree buf_param, tree buf_pos, const char *format, tree args) {
    tree sprintf_call = my_build_function_call(here, sprintf_decl,
            tree_cons(NULL_TREE, make_buf_ref_addr(here, buf_param, buf_pos),
            tree_cons(NULL_TREE, build_string_literal_here(here, strlen(format) + 1, format), args)));

    // save_expr on sprintf_call required to avoid crashing on GCC 7.5.0, see commit message
    return build_modify_expr(here, buf_pos, NULL_TREE, PLUS_EXPR, here, save_expr(sprintf_call), NULL_TREE);
}

static tree simple_nop_void(location_t here, tree expr) {
    return build1_loc(here, NOP_EXPR, void_type_node, expr);
}

static const char *make_subexpressions_repr(tree expr, tree *stmts, struct make_repr_params *params);

// builds a sprintf into params->call_buf_param in the format
// `%result = sprintf(buf + pos, "%d = function(%d, %d, %d)", return, arg1, arg2, arg3)`
static const char *make_call_subexpression_repr(tree expr, tree raw_expr, tree *stmts, struct make_repr_params *params) {
    gcc_assert(TREE_CODE(raw_expr) == CALL_EXPR);

    char buf[1024];

    tree fn = get_callee_fndecl(raw_expr);
    const char *fn_name = IDENTIFIER_POINTER(DECL_NAME(fn));

    const char *color = alloc_subexpr_color(params);
    if (color) {
        add_subexpr_color(expr, color, &params->subexpr_colors);
    }

    // use the expresion type for the format, not function result type!
    int n = snprintf(buf, sizeof(buf), "  %s%s(", color ?: "", fn_name);

    // parameters to the sprintf call.
    tree call_params = NULL_TREE;

    for (int i = 0; i < call_expr_nargs(raw_expr); i++) {
        tree *argp = CALL_EXPR_ARGP(raw_expr) + i;
        *argp = save_expr(*argp); // it will be evaluated twice, save it.

        // recursively, for my arguments
        const char *subexpr_color = make_subexpressions_repr(*argp, stmts, params);

        n += snprintf(buf + n, sizeof(buf) - n, "%s%s%s, ",
            subexpr_color ?: "", get_format_for_expr(*argp), color ?: (subexpr_color ? RESET_COLOR : ""));
        call_params = chainon(call_params, tree_cons(NULL_TREE, *argp, NULL_TREE));
    }

    call_params = chainon(call_params, tree_cons(NULL_TREE, expr, NULL_TREE)); //  last is the return value - expr itself.

    n -= 2; // (remove last ", ")
    n += snprintf(buf + n, sizeof(buf) - n, ") = %s%s\n", get_format_for_expr(expr), color ? RESET_COLOR : "");

    append_to_statement_list(make_repr_sprintf(params->here, params->call_buf_param, params->call_buf_pos,
        buf, call_params), stmts);

    return color;
}

static const char *make_subexpressions_repr(tree expr, tree *stmts, struct make_repr_params *params) {
    tree inner = strip_nop_and_convert(from_save_maybe(expr));

    if (DECL_P(inner)) {
        return append_decl_subexpression_repr(expr, inner, params);
    } else if (TREE_CODE(inner) == CALL_EXPR) {
        return make_call_subexpression_repr(expr, inner, stmts, params);
    }

    return NULL;
}

// this function is the core logic: it recursively generates a conditional expressions that walks
// `expr`, following short cicuting rules, creating the repr buf for `expr` based on what subexpressions
// have failed and which didn't. for example, if an && expression left-hand side fails, the generated
// code will repr only the left side, without the right.
static tree make_conditional_expr_repr(struct make_repr_params *params, tree expr) {
    tree raw_expr = from_save_maybe(expr);
    const enum tree_code code = TREE_CODE(raw_expr);

    if (TREE_OPERAND_LENGTH(raw_expr) == 2) {
        // we're about to evaluate these again, they better be SAVE_EXPR.
        assert_tree_is_save(TREE_OPERAND(raw_expr, 0));
        assert_tree_is_save(TREE_OPERAND(raw_expr, 1));
    }

    location_t here = params->here;
    tree buf_param = params->buf_param;
    tree buf_pos = params->buf_pos;

    // for TRUTH_ANDIF_EXPR/TRUTH_AND_EXPR:
    // * if left fails, we print only left
    // * if right fails, we print (...) && right
    // * if both pass, we print nothing
    if (code == TRUTH_ANDIF_EXPR || code == TRUTH_AND_EXPR) {
        tree stmts = alloc_stmt_list();
        append_to_statement_list(make_conditional_expr_repr(params, TREE_OPERAND(raw_expr, 0)), &stmts);

        tree left_stmts = stmts;

        stmts = alloc_stmt_list();
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, "(...) && (", NULL_TREE), &stmts);
        append_to_statement_list(make_conditional_expr_repr(params, TREE_OPERAND(raw_expr, 1)), &stmts);
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, ")", NULL_TREE), &stmts);

        tree right_stmts = stmts;

        tree cond = _build_conditional_expr(here, TREE_OPERAND(raw_expr, 0),
            simple_nop_void(here, right_stmts), NULL_TREE,
            simple_nop_void(here, left_stmts), NULL_TREE);

        return cond;
    }
    // for TRUTH_ORIF_EXPR/TRUTH_OR_EXPR
    // * if left and right fail, we print both
    // * if any pass, we print nothing
    else if (code == TRUTH_ORIF_EXPR || code == TRUTH_OR_EXPR) {
        tree stmts = alloc_stmt_list();
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, "(", NULL_TREE), &stmts);
        append_to_statement_list(make_conditional_expr_repr(params, TREE_OPERAND(raw_expr, 0)), &stmts);
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, ") || (", NULL_TREE), &stmts);
        append_to_statement_list(make_conditional_expr_repr(params, TREE_OPERAND(raw_expr, 1)), &stmts);
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, ")", NULL_TREE), &stmts);

        return _build_conditional_expr(here, raw_expr,
            simple_nop_void(here, integer_zero_node), NULL_TREE,
            simple_nop_void(here, stmts), NULL_TREE);
    }
    // for others - we always print them - because this code gets called only if the expression it reprs
    // has failed, because the &&/|| code guards it.
    else {
        tree stmts = alloc_stmt_list();

        tree inner = strip_nop_and_convert(raw_expr);
        const char *op = get_expr_op_repr(inner);

        if (op != NULL) {
            // TODO handle escaping more nicely :/
            if (0 == strcmp(op, "%")) {
                op = "%%"; // escape for the sprintf emitted here
            }

            // TODO: if inner != raw_expr then we had a cast here, display it.
            char format[64];
            (void)snprintf(format, sizeof(format), " %s ", op);

            append_to_statement_list(make_conditional_expr_repr(params, TREE_OPERAND(inner, 0)), &stmts);
            append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, format, NULL_TREE), &stmts);
            append_to_statement_list(make_conditional_expr_repr(params, TREE_OPERAND(inner, 1)), &stmts);
        } else {
            const char *subexpr_color = make_subexpressions_repr(expr, &stmts, params);

            char format[64];
            (void)snprintf(format, sizeof(format), "%s%s%s",
                subexpr_color ?: "", get_format_for_expr(expr), subexpr_color ? RESET_COLOR :"");

            append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, format,
                tree_cons(NULL_TREE, expr, NULL_TREE)), &stmts);
        }

        return stmts;
    }
}

static bool function_decl_missing_error(location_t here, tree func_decl, const char *name) {
    if (func_decl == NULL_TREE) {
        error_at(here, PLUGIN_NAME ": plugin requires declaration of '%s', please include relevant header\n", name);
        return true;
    }

    return false;
}

static tree make_assert_failed_body(location_t here, tree cond_expr) {
    // the patched expression is as follows:
    //
    // we create a new BIND_EXPR - local scope, in which we define
    // a variable to hold the generated assert repr. originally I designed it with this variable
    // so that deletions can be made, but eventually the implementation doesn't require this
    // "deletion" feature, so I might remove it later and simply emit printf calls.
    //
    // this scope starts by running the original condition of the COND_EXPR (that is, the assert)
    // but with the values of all subexpressions wrapped in a SAVE_EXPR, so we can reuse
    // them later.
    // if it passed - all good, we leave the bind EXPR.
    // if it fails, we build a tree of COND_EXPR that matches the tree of expressions
    // in the condition, this tree uses the SAVE_EXPR we generated earlier to re-do walk the
    // expression tree in-order and write the assert repr and values into the before mentioned
    // variable.
    //
    // lastly, the assert repr is printed, and abort() is called.

    if (function_decl_missing_error(here, printf_decl, "printf") ||
        function_decl_missing_error(here, sprintf_decl, "sprintf") ||
        function_decl_missing_error(here, abort_decl, "abort")) {

        // continue unmodified.
        return cond_expr;
    }

    tree stmts = alloc_stmt_list();
    tree first_stmts = alloc_stmt_list();
    tree block = make_node(BLOCK);

    // print "> assert(...)" with the original expression text
    make_assert_expr_printf(here, COND_EXPR_ELSE(cond_expr), &first_stmts);

    tree buf_param, buf_pos;
    set_up_repr_buf(here, stmts, block, &buf_param, &buf_pos);

    tree call_buf_param, call_buf_pos;
    set_up_repr_buf(here, stmts, block, &call_buf_param, &call_buf_pos);

    // wrap all subexpressions
    wrap_in_save_expr(&COND_EXPR_COND(cond_expr));

    // write the expression repr itself
    struct make_repr_params params = {
        .here = here,
        .buf_param = buf_param,
        .buf_pos = buf_pos,
        .call_buf_param = call_buf_param,
        .call_buf_pos = call_buf_pos,
        .color_idx = 0,
        .decl_repr_exprs = {0},
        .subexpr_colors = {0},
    };
    append_to_statement_list(make_conditional_expr_repr(&params, COND_EXPR_COND(cond_expr)), &stmts);

    // print the repr buf now
    tree printf_call = my_build_function_call(here, printf_decl,
        tree_cons(NULL_TREE, build_string_literal_from_literal(here, BOLD_RED("E") " assert(%s)\n"),
        tree_cons(NULL_TREE, buf_param, NULL_TREE)));
    append_to_statement_list(printf_call, &stmts);

    append_to_statement_list(my_build_function_call(here, printf_decl,
        tree_cons(NULL_TREE, build_string_literal_from_literal(here, "> subexpressions:\n"), NULL_TREE)),
        &stmts);

    // add DECLs subexpressions repr
    make_decl_subexpressions_repr(here, &params.decl_repr_exprs, &stmts);

    // recreate the repr from AST
    make_assert_expr_printf_from_ast(here, COND_EXPR_COND(cond_expr), &params.subexpr_colors, &first_stmts);
    free_expr_list(&params.subexpr_colors);

    // print call buf repr
    printf_call = my_build_function_call(here, printf_decl,
        tree_cons(NULL_TREE, build_string_literal_from_literal(here, "%s"),
        tree_cons(NULL_TREE, call_buf_param, NULL_TREE)));
    append_to_statement_list(printf_call, &stmts);

    // finally, an abort call
    tree abort_call = my_build_function_call(here, abort_decl, NULL_TREE);
    append_to_statement_list(abort_call, &stmts);

    // concat the 2 lists
    append_to_statement_list(stmts, &first_stmts);

    return c_build_bind_expr(here, block, first_stmts);
}

// cond_expr is an expression that matched is_assert_fail_cond_expr().
// this function returns a new expression that'll be used to replace it.
static tree patch_assert(tree cond_expr) {
    location_t here = EXPR_LOCATION(cond_expr);

    tree bind = make_assert_failed_body(here, cond_expr);

    // `(original_cond) ? (void)0 : { ... our bind expr ... }`
    tree new_cond = build3_loc(here, COND_EXPR, void_type_node, COND_EXPR_COND(cond_expr),
        simple_nop_void(here, integer_zero_node), bind);

    return new_cond;
}

static void iterate_function_body(tree expr) {
    tree body;

    if (TREE_CODE(expr) == BIND_EXPR) {
        body = BIND_EXPR_BODY(expr);
    } else {
        gcc_assert(TREE_CODE(expr) == STATEMENT_LIST);
        body = expr;
    }

    if (TREE_CODE(body) == STATEMENT_LIST) {
        for (tree_stmt_iterator i = tsi_start(body); !tsi_end_p(i); tsi_next(&i)) {
            tree stmt = tsi_stmt(i);

            if (TREE_CODE(stmt) == BIND_EXPR) {
                iterate_function_body(stmt);
            }
        }
    } else {
        // for individual statements in BIND_EXPRs - check if they're the COND_EXPR of assert()s.
        // see the docs of is_assert_fail_cond_expr().
        if (is_assert_fail_cond_expr(body)) {
            gcc_assert(TREE_CODE(expr) == BIND_EXPR);
            gcc_assert(TREE_CODE(body) == COND_EXPR);

            // as far as I understand it, if there's any error inside COND_EXPR_COND,
            // the entire expression will be marked as error.
            if (!error_operand_p((COND_EXPR_COND(body)))) {
                BIND_EXPR_BODY(expr) = patch_assert(body);
            } else {
                error_at(EXPR_LOCATION(body), PLUGIN_NAME ": previous error in expression, not rewriting assert\n");
            }
        }
    }
}

static void pre_genericize_callback(void *event_data, void *user_data) {
    (void)user_data;

    tree t = (tree)event_data;

    if (TREE_CODE(t) == FUNCTION_DECL) {
        iterate_function_body(DECL_SAVED_TREE(t));
    }
}

static void finish_decl_callback(void *event_data, void *user_data) {
    tree decl = (tree)event_data;

    // TODO use GCC's lookup_name instead
    if (TREE_CODE(decl) == FUNCTION_DECL && 0 == strcmp("printf", IDENTIFIER_POINTER(DECL_NAME(decl)))) {
        printf_decl = decl;
    }
    if (TREE_CODE(decl) == FUNCTION_DECL && 0 == strcmp("abort", IDENTIFIER_POINTER(DECL_NAME(decl)))) {
        abort_decl = decl;
    }
    if (TREE_CODE(decl) == FUNCTION_DECL && 0 == strcmp("sprintf", IDENTIFIER_POINTER(DECL_NAME(decl)))) {
        sprintf_decl = decl;
    }
}

int plugin_init(struct plugin_name_args *plugin_info, struct plugin_gcc_version *version) {
    printf(PLUGIN_NAME " loaded, compiled for GCC %s\n", gcc_version.basever);
    register_callback(plugin_info->base_name, PLUGIN_PRE_GENERICIZE, pre_genericize_callback, NULL);
    register_callback(plugin_info->base_name, PLUGIN_FINISH_DECL, finish_decl_callback, NULL);

    return 0;
}
