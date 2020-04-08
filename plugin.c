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
#include <print-tree.h>
#include <tree-iterator.h>
#include <c-family/c-common.h>


int plugin_is_GPL_compatible; // must be defined for the plugin to run

static tree printf_decl = NULL_TREE;
static tree abort_decl = NULL_TREE;

// heuristic to check if cond_expr is the expression generated by glibc's "assert" macro, which is:
// ((void) sizeof ((EXPR) ? 1 : 0), __extension__ ({ if (EXPR) ; else __assert_fail ("EXPR", "file.c", line, __extension__ __PRETTY_FUNCTION__); }))
// the generated cond_expr has COND_EXPR_COND as the assert's condition,
// COND_EXPR_THEN is a nop_expr and COND_EXPR_ELSE is a call_expr calling to "__assert_fail".
static bool is_assert_fail_cond_expr(tree cond_expr) {
    gcc_assert(TREE_CODE(cond_expr) == COND_EXPR);

    tree expr_else = COND_EXPR_ELSE(cond_expr);
    return (
        TREE_CODE(COND_EXPR_THEN(cond_expr)) == NOP_EXPR &&
        TREE_CODE(expr_else) == CALL_EXPR &&
        TREE_CODE(CALL_EXPR_FN(expr_else)) == ADDR_EXPR &&
        TREE_CODE(TREE_OPERAND(CALL_EXPR_FN(expr_else), 0)) == FUNCTION_DECL &&
        0 == strcmp("__assert_fail", IDENTIFIER_POINTER(DECL_NAME(TREE_OPERAND(CALL_EXPR_FN(expr_else), 0))))
    );
}

// lol
#define build_string_literal_from_literal(s) build_string_literal(sizeof(s), s)

struct parse_result {
    tree params_tree;
    char *repr;
};

static void free_parse_result(struct parse_result *res) {
    free(res->repr);
    // params_tree is garbage collected (?)
}

static bool parse_expression(tree *expr, struct parse_result *res);

static bool parse_expression_binary(const char *op, tree expr, struct parse_result *res) {
    struct parse_result left, right;
    bool ret = false;
    int required_size;

    res->repr = NULL;
    res->params_tree = NULL_TREE;

    if (!parse_expression(&TREE_OPERAND(expr, 0), &left)) {
        goto out;
    }
    if (!parse_expression(&TREE_OPERAND(expr, 1), &right)) {
        goto out_free_left;
    }

    required_size = strlen("(") + strlen(left.repr) + strlen(" ")
                    + strlen(op)
                    + strlen(" ") + strlen(right.repr) + strlen(")") + sizeof('\0');
    res->repr = (char*)xmalloc(required_size);
    if (!res->repr) {
        goto out_free_right;
    }

    if (snprintf(res->repr, required_size, "(%s %s %s)", left.repr, op, right.repr) >= required_size) {
        printf("%s: binary expression repr is too long\n", __func__);
        goto out_free_right;
    }

    // attach right to the last entry in left
    TREE_CHAIN(tree_last(left.params_tree)) = right.params_tree;
    res->params_tree = left.params_tree;

    ret = true;

out_free_right:
    free_parse_result(&right);
out_free_left:
    free_parse_result(&left);
out:
    return ret;
}

static bool parse_expression(tree *expr, struct parse_result *res) {
    const char *op;

    switch (TREE_CODE(*expr)) {
    case EQ_EXPR: op = "=="; break;
    case NE_EXPR: op = "!="; break;
    case LT_EXPR: op = "<"; break;
    case LE_EXPR: op = "<="; break;
    case GT_EXPR: op = ">"; break;
    case GE_EXPR: op = ">="; break;
    case TRUTH_AND_EXPR:
    case TRUTH_ANDIF_EXPR: op = "&&"; break;
    case TRUTH_OR_EXPR:
    case TRUTH_ORIF_EXPR: op = "||"; break;
    default: op = NULL; break;
    }

    if (op) {
        return parse_expression_binary(op, *expr, res);
    } else {
        // by wrapping expr in save_expr(), we ensure the original expression is evaluated
        // only once (and not twice if we use the value when the params_tree is evaluated)
        *expr = save_expr(*expr);
        res->params_tree = tree_cons(NULL_TREE, *expr, NULL_TREE);
        // TODO specific types
        res->repr = xstrdup("%d");
        return res->repr != NULL;
    }
}

static bool expression_repr_call(tree cond_expr, tree *call) {
    struct parse_result res;
    if (!parse_expression(&COND_EXPR_COND(cond_expr), &res)) {
        printf("failed to parse expression! refusing to patch\n");
        return false;
    }

    // add some stuff to the expression repr
    const char *const prefix = "  assert";
    const int len = strlen(prefix) + strlen(res.repr) + sizeof('\n') + sizeof('\0');
    char *repr_newline = (char*)xmalloc(len);
    if (!repr_newline) {
        printf("malloc failed\n");
        free_parse_result(&res);
        return false;
    }

    memcpy(repr_newline, prefix, strlen(prefix));
    memcpy(repr_newline + strlen(prefix), res.repr, strlen(res.repr));
    repr_newline[len - 2] = '\n';
    repr_newline[len - 1] = '\0';
    tree params = tree_cons(NULL_TREE, build_string_literal(len, repr_newline), res.params_tree);

    *call = build_function_call(EXPR_LOCATION(COND_EXPR_ELSE(cond_expr)), printf_decl, params);

    free_parse_result(&res);
    free(repr_newline);

    return true;
}

// create a printf("> assert(EXPR)\n") call (for the original assert() expression)
static bool expression_print_call(tree cond_expr, tree *call) {
    // grab the EXPR from the __assert_fail call (1st argument)
    tree arg1 = CALL_EXPR_ARG(COND_EXPR_ELSE(cond_expr), 0);
    if (CONVERT_EXPR_P(arg1)) {
        arg1 = TREE_OPERAND(arg1, 0);
    }
    gcc_assert(TREE_CODE(arg1) == ADDR_EXPR);

    arg1 = TREE_OPERAND(arg1, 0);
    gcc_assert(TREE_CODE(arg1) == STRING_CST);

    const char *expr = TREE_STRING_POINTER(arg1);
    const size_t expr_len = TREE_STRING_LENGTH(arg1);
    gcc_assert(expr[expr_len - 1] == '\0'); // TREE_STRING_LENGTH should include the NULL terminator

    char line[1024]; // should be enough...
    if ((unsigned int)snprintf(line, sizeof(line), "> assert(%s)\n", expr) >= sizeof(line)) {
        printf("%s: expression too long for me to handle! '%s'\n", __func__, expr);
        return false;
    }

    *call = build_function_call(EXPR_LOCATION(COND_EXPR_ELSE(cond_expr)), printf_decl,
        tree_cons(NULL_TREE, build_string_literal(strlen(line) + 1, line ), NULL_TREE));
    return true;
}

// cond_expr is an expression that matched is_assert_fail_cond_expr().
// this function returns a new expression that'll be used to replace it.
static tree patch_assert(tree cond_expr) {
    gcc_assert(printf_decl != NULL_TREE);
    gcc_assert(abort_decl != NULL_TREE);
    gcc_assert(TREE_CODE(cond_expr) == COND_EXPR);

    tree repr_call, expr_print_call;

    if (!expression_repr_call(cond_expr, &repr_call)
        || !expression_print_call(cond_expr, &expr_print_call)) {

        // leave unmodified.
        return cond_expr;
    }

    // finally, an abort call
    tree abort_call = build_function_call(EXPR_LOCATION(COND_EXPR_ELSE(cond_expr)), abort_decl, NULL_TREE);

    // put all calls together in a list.
    tree t = alloc_stmt_list();
    append_to_statement_list(expr_print_call, &t);
    append_to_statement_list(repr_call, &t);
    append_to_statement_list(abort_call, &t);

    // in the future I can use this, which also works:
    // return build3_loc(EXPR_LOCATION(COND_EXPR_ELSE(cond_expr)), COND_EXPR, void_type_node, COND_EXPR_COND(cond_expr), COND_EXPR_THEN(cond_expr), t);
    // meanwhile, just replacing is fine
    COND_EXPR_ELSE(cond_expr) = t;
    return cond_expr;
}

static void iterate_bind_expr(tree bind) {
    gcc_assert(TREE_CODE(bind) == BIND_EXPR);

    tree body = BIND_EXPR_BODY(bind);
    if (TREE_CODE(body) == STATEMENT_LIST) {
        for (tree_stmt_iterator i = tsi_start(body); !tsi_end_p(i); tsi_next(&i)) {
            tree stmt = tsi_stmt(i);

            if (TREE_CODE(stmt) == BIND_EXPR) {
                iterate_bind_expr(stmt);
            }
        }
    } else {
        gcc_assert(TREE_CODE(body) == COND_EXPR); // I don't know of other possible types yet

        if (is_assert_fail_cond_expr(body)) {
            BIND_EXPR_BODY(bind) = patch_assert(body);
        }
    }
}

static void pre_genericize_callback(void *event_data, void *user_data) {
    (void)user_data;

    tree t = (tree)event_data;

    if (TREE_CODE(t) != FUNCTION_DECL || 0 != strcmp("test_func", IDENTIFIER_POINTER(DECL_NAME(t)))) {
        return;
    }

    tree bind = DECL_SAVED_TREE(t);
    if (!bind || TREE_CODE(bind) != BIND_EXPR) {
        printf("function body is not BIND_EXPR??\n");
        return;
    }

    iterate_bind_expr(bind);
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
}

int plugin_init(struct plugin_name_args *plugin_info, struct plugin_gcc_version *version) {
    register_callback(plugin_info->base_name, PLUGIN_PRE_GENERICIZE, pre_genericize_callback, NULL);
    register_callback(plugin_info->base_name, PLUGIN_FINISH_DECL, finish_decl_callback, NULL);

    return 0;
}
