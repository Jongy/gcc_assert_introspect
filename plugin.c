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

#include "utils.h"

#define PLUGIN_NAME "assert_introspect"

int plugin_is_GPL_compatible; // must be defined for the plugin to run

static tree printf_decl = NULL_TREE;
static tree abort_decl = NULL_TREE;
static tree sprintf_decl = NULL_TREE;

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
#define build_string_literal_from_literal(s) build_string_literal(sizeof(s), s)

static inline tree _build_conditional_expr(location_t colon_loc, tree ifexp,
    tree op1, tree op1_original_type, tree op2, tree op2_original_type) {

#if GCCPLUGIN_VERSION >= 8001 // a32c8316ff282ec
    return build_conditional_expr(colon_loc, ifexp, false, op1, op1_original_type,
        UNKNOWN_LOCATION, op2, op2_original_type, UNKNOWN_LOCATION);
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

static void wrap_in_save_expr(tree *expr) {
    // use get_expr_op_repr as a predicate: "is it a binary expression with 2 args"
    // TODO this probably doesn't catch all cases + doesn't catch unary ops (whose inner value
    // should be made a SAVE_EXPR as well)
    if (get_expr_op_repr(*expr) != NULL) {
        wrap_in_save_expr(&TREE_OPERAND(*expr, 0));
        wrap_in_save_expr(&TREE_OPERAND(*expr, 1));
    }
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
    (void)snprintf(buf, sizeof(buf), "In '%s':%ld, function '%%s':\n",
        TREE_STRING_POINTER(file_arg), TREE_INT_CST_LOW(line_arg));

    tree header_line = build_string_literal(strlen(buf) + 1, buf);
    append_to_statement_list(build_function_call(here, printf_decl,
        tree_cons(NULL_TREE, header_line,
        tree_cons(NULL_TREE, function_arg, NULL_TREE))), stmts);

    tree format_str = build_string_literal_from_literal("> assert(%s)\n");
    append_to_statement_list(build_function_call(here, printf_decl,
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

    BLOCK_VARS(block) = buf;
    TREE_CHAIN(buf) = pos;
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

static tree make_buf_ref_addr(tree buf_param, tree buf_pos) {
    return build_addr(build_array_ref(UNKNOWN_LOCATION, buf_param, buf_pos));
}

// creats a `buf_pos += sprintf(...)` with given arguments.
static tree make_repr_sprintf(location_t here, tree buf_param, tree buf_pos, const char *format, tree args) {
    tree sprintf_call = build_function_call(here, sprintf_decl,
            tree_cons(NULL_TREE, make_buf_ref_addr(buf_param, buf_pos),
            tree_cons(NULL_TREE, build_string_literal(strlen(format) + 1, format), args)));

    return build_modify_expr(here, buf_pos, NULL_TREE, PLUS_EXPR, here, sprintf_call, NULL_TREE);
}

static tree simple_nop_void(location_t here, tree expr) {
    return build1_loc(here, NOP_EXPR, void_type_node, expr);
}

// this function is the core logic: it recursively generates a conditional expressions that walks
// `expr`, following short cicuting rules, creating the repr buf for `expr` based on what subexpressions
// have failed and which didn't. for example, if an && expression left-hand side fails, the generated
// code will repr only the left side, without the right.
static tree make_conditional_expr_repr(location_t here, tree expr, tree buf_param, tree buf_pos) {
    tree raw_expr = from_save_maybe(expr);
    const enum tree_code code = TREE_CODE(raw_expr);

    if (TREE_OPERAND_LENGTH(raw_expr) == 2) {
        // we're about to evaluate these again, they better be SAVE_EXPR.
        assert_tree_is_save(TREE_OPERAND(raw_expr, 0));
        assert_tree_is_save(TREE_OPERAND(raw_expr, 1));
    }

    // for TRUTH_ANDIF_EXPR/TRUTH_AND_EXPR:
    // * if left fails, we print only left
    // * if right fails, we print (...) && right
    // * if both pass, we print nothing
    if (code == TRUTH_ANDIF_EXPR || code == TRUTH_AND_EXPR) {
        tree stmts = alloc_stmt_list();
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, "(...) && (", NULL_TREE), &stmts);
        append_to_statement_list(make_conditional_expr_repr(here, TREE_OPERAND(raw_expr, 1), buf_param, buf_pos), &stmts);
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, ")", NULL_TREE), &stmts);

        tree right_stmts = stmts;

        stmts = alloc_stmt_list();
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, "(", NULL_TREE), &stmts);
        append_to_statement_list(make_conditional_expr_repr(here, TREE_OPERAND(raw_expr, 0), buf_param, buf_pos), &stmts);
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, ")", NULL_TREE), &stmts);

        tree left_stmts = stmts;

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
        append_to_statement_list(make_conditional_expr_repr(here, TREE_OPERAND(raw_expr, 0), buf_param, buf_pos), &stmts);
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, ") || (", NULL_TREE), &stmts);
        append_to_statement_list(make_conditional_expr_repr(here, TREE_OPERAND(raw_expr, 1), buf_param, buf_pos), &stmts);
        append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, ")", NULL_TREE), &stmts);

        return _build_conditional_expr(here, raw_expr,
            simple_nop_void(here, integer_zero_node), NULL_TREE,
            simple_nop_void(here, stmts), NULL_TREE);
    }
    // for others - we always print them - because this code gets called only if the expression it reprs
    // has failed, because the &&/|| code guards it.
    else {
        const char *op = get_expr_op_repr(raw_expr);

        if (op != NULL) {
            // TODO handle escaping more nicely :/
            if (0 == strcmp(op, "%")) {
                op = "%%%%"; // 2 levels of escaping: 1 for the sprintf emitted here, 1 for the printf later.
            }

            char format[64];
            sprintf(format, " %s ", op);

            tree stmts = alloc_stmt_list();
            append_to_statement_list(make_conditional_expr_repr(here, TREE_OPERAND(raw_expr, 0), buf_param, buf_pos), &stmts);
            append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, format, NULL_TREE), &stmts);
            append_to_statement_list(make_conditional_expr_repr(here, TREE_OPERAND(raw_expr, 1), buf_param, buf_pos), &stmts);
            return stmts;
        } else {
            // TODO specific types, not just %d
            return make_repr_sprintf(here, buf_param, buf_pos, "%d",
                tree_cons(NULL_TREE, expr, NULL_TREE));
        }
    }
}

static bool function_decl_missing_error(location_t here, tree func_decl, const char *name) {
    if (func_decl == NULL_TREE) {
        error_at(here, PLUGIN_NAME ": plugin requires declaration of '%s'\n", name);
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
    tree block = make_node(BLOCK);

    // print "> assert(...)" with the original expression text
    make_assert_expr_printf(here, COND_EXPR_ELSE(cond_expr), &stmts);

    tree buf_param, buf_pos;
    set_up_repr_buf(here, stmts, block, &buf_param, &buf_pos);

    // wrap all subexpressions
    wrap_in_save_expr(&COND_EXPR_COND(cond_expr));

    append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, "  assert(", NULL_TREE), &stmts);
    append_to_statement_list(make_conditional_expr_repr(here, COND_EXPR_COND(cond_expr), buf_param, buf_pos), &stmts);
    append_to_statement_list(make_repr_sprintf(here, buf_param, buf_pos, ")\n", NULL_TREE), &stmts);

    // print the repr buf now
    tree printf_repr_call = build_function_call(here, printf_decl, tree_cons(NULL_TREE, buf_param, NULL_TREE));
    append_to_statement_list(printf_repr_call, &stmts);

    // finally, an abort call
    tree abort_call = build_function_call(here, abort_decl, NULL_TREE);
    append_to_statement_list(abort_call, &stmts);

    return c_build_bind_expr(here, block, stmts);
}

// cond_expr is an expression that matched is_assert_fail_cond_expr().
// this function returns a new expression that'll be used to replace it.
static tree patch_assert(tree cond_expr) {
    gcc_assert(TREE_CODE(cond_expr) == COND_EXPR);

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
            BIND_EXPR_BODY(expr) = patch_assert(body);
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
