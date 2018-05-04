#include "Simplify_Internal.h"

#include "IRMutator.h"
#include "Substitute.h"

namespace Halide {
namespace Internal {

using std::vector;
using std::pair;
using std::string;

Stmt Simplify::visit(const IfThenElse *op) {
    Expr condition = mutate(op->condition, nullptr);

    // If (true) ...
    if (is_one(condition)) {
        return mutate(op->then_case);
    }

    // If (false) ...
    if (is_zero(condition)) {
        if (op->else_case.defined()) {
            return mutate(op->else_case);
        } else {
            return Evaluate::make(0);
        }
    }

    Stmt then_case, else_case;
    {
        auto f = scoped_truth(op->condition);
        // Also substitute the entire condition
        then_case = substitute(op->condition, const_true(condition.type().lanes()), op->then_case);
        then_case = mutate(then_case);
    }
    {
        auto f = scoped_falsehood(op->condition);
        else_case = substitute(op->condition, const_false(condition.type().lanes()), op->else_case);
        else_case = mutate(else_case);
    }

    // If both sides are no-ops, bail out.
    if (is_no_op(then_case) && is_no_op(else_case)) {
        return then_case;
    }

    if (condition.same_as(op->condition) &&
        then_case.same_as(op->then_case) &&
        else_case.same_as(op->else_case)) {
        return op;
    } else {
        return IfThenElse::make(condition, then_case, else_case);
    }
}

Stmt Simplify::visit(const AssertStmt *op) {
    Expr cond = mutate(op->condition, nullptr);

    // The message is only evaluated when the condition is false
    Expr message;
    {
        auto f = scoped_falsehood(cond);
        message = mutate(op->message, nullptr);
    }

    if (is_zero(cond)) {
        // Usually, assert(const-false) should generate a warning;
        // in at least one case (specialize_fail()), we want to suppress
        // the warning, because the assertion is generated internally
        // by Halide and is expected to always fail.
        const Call *call = message.as<Call>();
        const bool const_false_conditions_expected =
            call && call->name == "halide_error_specialize_fail";
        if (!const_false_conditions_expected) {
            user_warning << "This pipeline is guaranteed to fail an assertion at runtime with error: \n"
                         << message << "\n";
        }
    } else if (is_one(cond)) {
        return Evaluate::make(0);
    }

    if (cond.same_as(op->condition) && message.same_as(op->message)) {
        return op;
    } else {
        return AssertStmt::make(cond, message);
    }
}

Stmt Simplify::visit(const For *op) {
    ConstBounds min_bounds, extent_bounds;
    Expr new_min = mutate(op->min, &min_bounds);
    Expr new_extent = mutate(op->extent, &extent_bounds);

    bool bounds_tracked = false;
    if (min_bounds.min_defined || (min_bounds.max_defined && extent_bounds.max_defined)) {
        min_bounds.max += extent_bounds.max - 1;
        min_bounds.max_defined &= extent_bounds.max_defined;
        bounds_tracked = true;
        bounds_info.push(op->name, min_bounds);
    }

    Stmt new_body = mutate(op->body);

    if (bounds_tracked) {
        bounds_info.pop(op->name);
    }

    if (is_no_op(new_body)) {
        return new_body;
    } else if (op->min.same_as(new_min) &&
               op->extent.same_as(new_extent) &&
               op->body.same_as(new_body)) {
        return op;
    } else {
        return For::make(op->name, new_min, new_extent, op->for_type, op->device_api, new_body);
    }
}

Stmt Simplify::visit(const Provide *op) {
    found_buffer_reference(op->name, op->args.size());

    vector<Expr> new_args(op->args.size());
    vector<Expr> new_values(op->values.size());
    bool changed = false;

    // Mutate the args
    for (size_t i = 0; i < op->args.size(); i++) {
        const Expr &old_arg = op->args[i];
        Expr new_arg = mutate(old_arg, nullptr);
        if (!new_arg.same_as(old_arg)) changed = true;
        new_args[i] = new_arg;
    }

    for (size_t i = 0; i < op->values.size(); i++) {
        const Expr &old_value = op->values[i];
        Expr new_value = mutate(old_value, nullptr);
        if (!new_value.same_as(old_value)) changed = true;
        new_values[i] = new_value;
    }

    if (!changed) {
        return op;
    } else {
        return Provide::make(op->name, new_values, new_args);
    }
}

Stmt Simplify::visit(const Store *op) {
    found_buffer_reference(op->name);

    Expr predicate = mutate(op->predicate, nullptr);
    Expr value = mutate(op->value, nullptr);
    Expr index = mutate(op->index, nullptr);

    const Load *load = value.as<Load>();
    const Broadcast *scalar_pred = predicate.as<Broadcast>();

    if (is_zero(predicate)) {
        // Predicate is always false
        return Evaluate::make(0);
    } else if (scalar_pred && !is_one(scalar_pred->value)) {
        return IfThenElse::make(scalar_pred->value,
                                Store::make(op->name, value, index, op->param, const_true(value.type().lanes())));
    } else if (is_undef(value) || (load && load->name == op->name && equal(load->index, index))) {
        // foo[x] = foo[x] or foo[x] = undef is a no-op
        return Evaluate::make(0);
    } else if (predicate.same_as(op->predicate) && value.same_as(op->value) && index.same_as(op->index)) {
        return op;
    } else {
        return Store::make(op->name, value, index, op->param, predicate);
    }
}

Stmt Simplify::visit(const Allocate *op) {
    std::vector<Expr> new_extents;
    bool all_extents_unmodified = true;
    for (size_t i = 0; i < op->extents.size(); i++) {
        new_extents.push_back(mutate(op->extents[i], nullptr));
        all_extents_unmodified &= new_extents[i].same_as(op->extents[i]);
    }
    Stmt body = mutate(op->body);
    Expr condition = mutate(op->condition, nullptr);
    Expr new_expr;
    if (op->new_expr.defined()) {
        new_expr = mutate(op->new_expr, nullptr);
    }
    const IfThenElse *body_if = body.as<IfThenElse>();
    if (body_if &&
        op->condition.defined() &&
        equal(op->condition, body_if->condition)) {
        // We can move the allocation into the if body case. The
        // else case must not use it.
        Stmt stmt = Allocate::make(op->name, op->type, op->memory_type,
                                   new_extents, condition, body_if->then_case,
                                   new_expr, op->free_function);
        return IfThenElse::make(body_if->condition, stmt, body_if->else_case);
    } else if (all_extents_unmodified &&
               body.same_as(op->body) &&
               condition.same_as(op->condition) &&
               new_expr.same_as(op->new_expr)) {
        return op;
    } else {
        return Allocate::make(op->name, op->type, op->memory_type,
                              new_extents, condition, body,
                              new_expr, op->free_function);
    }
}

Stmt Simplify::visit(const Evaluate *op) {
    Expr value = mutate(op->value, nullptr);

    // Rewrite Lets inside an evaluate as LetStmts outside the Evaluate.
    vector<pair<string, Expr>> lets;
    while (const Let *let = value.as<Let>()) {
        lets.push_back({let->name, let->value});
        value = let->body;
    }

    if (value.same_as(op->value)) {
        internal_assert(lets.empty());
        return op;
    } else {
        // Rewrap the lets outside the evaluate node
        Stmt stmt = Evaluate::make(value);
        for (size_t i = lets.size(); i > 0; i--) {
            stmt = LetStmt::make(lets[i-1].first, lets[i-1].second, stmt);
        }
        return stmt;
    }
}

Stmt Simplify::visit(const ProducerConsumer *op) {
    Stmt body = mutate(op->body);

    if (is_no_op(body)) {
        return Evaluate::make(0);
    } else if (body.same_as(op->body)) {
        return op;
    } else {
        return ProducerConsumer::make(op->name, op->is_producer, body);
    }
}

Stmt Simplify::visit(const Block *op) {
    Stmt first = mutate(op->first);
    Stmt rest;

    if (const AssertStmt *a = first.as<AssertStmt>()) {
        // We can assume the asserted condition is true in the
        // rest. This should propagate constraints.
        auto f = scoped_truth(a->condition);
        rest = mutate(op->rest);
    } else {
        rest = mutate(op->rest);
    }

    // Check if both halves start with a let statement.
    const LetStmt *let_first = first.as<LetStmt>();
    const LetStmt *let_rest = rest.as<LetStmt>();
    const IfThenElse *if_first = first.as<IfThenElse>();
    const IfThenElse *if_rest = rest.as<IfThenElse>();

    if (is_no_op(first) &&
        is_no_op(rest)) {
        return Evaluate::make(0);
    } else if (is_no_op(first)) {
        return rest;
    } else if (is_no_op(rest)) {
        return first;
    } else if (let_first &&
               let_rest &&
               equal(let_first->value, let_rest->value) &&
               is_pure(let_first->value)) {

        // Do both first and rest start with the same let statement (occurs when unrolling).
        Stmt new_block = mutate(Block::make(let_first->body, let_rest->body));

        // We need to make a new name since we're pulling it out to a
        // different scope.
        string var_name = unique_name('t');
        Expr new_var = Variable::make(let_first->value.type(), var_name);
        new_block = substitute(let_first->name, new_var, new_block);
        new_block = substitute(let_rest->name, new_var, new_block);

        return LetStmt::make(var_name, let_first->value, new_block);
    } else if (if_first &&
               if_rest &&
               equal(if_first->condition, if_rest->condition) &&
               is_pure(if_first->condition)) {
        // Two ifs with matching conditions
        Stmt then_case = mutate(Block::make(if_first->then_case, if_rest->then_case));
        Stmt else_case;
        if (if_first->else_case.defined() && if_rest->else_case.defined()) {
            else_case = mutate(Block::make(if_first->else_case, if_rest->else_case));
        } else if (if_first->else_case.defined()) {
            // We already simplified the body of the ifs.
            else_case = if_first->else_case;
        } else {
            else_case = if_rest->else_case;
        }
        return IfThenElse::make(if_first->condition, then_case, else_case);
    } else if (if_first &&
               if_rest &&
               !if_rest->else_case.defined() &&
               is_pure(if_first->condition) &&
               is_pure(if_rest->condition) &&
               is_one(mutate((if_first->condition && if_rest->condition) == if_rest->condition, nullptr))) {
        // Two ifs where the second condition is tighter than
        // the first condition.  The second if can be nested
        // inside the first one, because if it's true the
        // first one must also be true.
        Stmt then_case = mutate(Block::make(if_first->then_case, if_rest));
        Stmt else_case = mutate(if_first->else_case);
        return IfThenElse::make(if_first->condition, then_case, else_case);
    } else if (op->first.same_as(first) &&
               op->rest.same_as(rest)) {
        return op;
    } else {
        return Block::make(first, rest);
    }
}

Stmt Simplify::visit(const Realize *op) {
    Region new_bounds;
    bool bounds_changed;

    // Mutate the bounds
    std::tie(new_bounds, bounds_changed) = mutate_region(this, op->bounds, nullptr);

    Stmt body = mutate(op->body);
    Expr condition = mutate(op->condition, nullptr);
    if (!bounds_changed &&
        body.same_as(op->body) &&
        condition.same_as(op->condition)) {
        return op;
    }
    return Realize::make(op->name, op->types, op->memory_type, new_bounds,
                         std::move(condition), std::move(body));
}

Stmt Simplify::visit(const Prefetch *op) {
    Region new_bounds;
    bool bounds_changed;

    // Mutate the bounds
    std::tie(new_bounds, bounds_changed) = mutate_region(this, op->bounds, nullptr);

    if (!bounds_changed) {
        return op;
    }
    return Prefetch::make(op->name, op->types, new_bounds, op->param);
}

Stmt Simplify::visit(const Free *op) {
    return op;
}

}
}