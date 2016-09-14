// Copyright 2010-2014 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "flatzinc/presolve.h"
#include "base/strutil.h"
#include "base/map_util.h"
#include "util/saturated_arithmetic.h"

DECLARE_bool(fz_logging);
DECLARE_bool(fz_verbose);
namespace operations_research {
namespace fz {
namespace {
// TODO(user): accept variables fixed to 0 or 1.
bool Has01Values(IntegerVariable* var) {
  return var->domain.Min() == 0 && var->domain.Max() == 1;
}

bool Is0Or1(int64 value) { return !(value & 0xfffffffffffffffe); }

template <class T>
bool IsArrayBoolean(const std::vector<T>& values) {
  for (int i = 0; i < values.size(); ++i) {
    if (values[i] != 0 && values[i] != 1) {
      return false;
    }
  }
  return true;
}

template <class T>
bool OnlyOne0OrOnlyOne1(const std::vector<T>& values) {
  int num_zero = 0;
  int num_one = 0;
  for (T val : values) {
    if (val) {
      num_one++;
    } else {
      num_zero++;
    }
    if (num_one > 1 && num_zero > 1) {
      return false;
    }
  }
  return true;
}
}  // namespace

// For the author's reference, here is an indicative list of presolve rules
// that
// should eventually be implemented.
//
// Presolve rule:
//   - table_int -> intersect variables domains with tuple set.
//
// TODO(user):
//   - store dependency graph of constraints -> variable to speed up presolve.
//   - use the same dependency graph to speed up variable substitution.
//   - add more check when presolving out a variable or a constraint.

// ----- Rule helpers -----
//
// This method wraps each rule, calls it and log its effect.
bool Presolver::ApplyRule(Constraint* ct, const std::string& rule_name,
                          std::function<bool(Constraint* ct, std::string*)> rule) {
  const std::string before = ct->DebugString();
  std::string log;
  const bool modified = rule(ct, &log);
  if (modified) {
    FZVLOG << "Apply rule " << rule_name << " on " << before << FZENDL;
    if (!log.empty()) {
      FZVLOG << "  - log: " << log << FZENDL;
    }
    if (!ct->active) {
      FZVLOG << "  - constraint is now inactive" << FZENDL;
    } else {
      const std::string after = ct->DebugString();
      if (after != before) {
        FZVLOG << "  - constraint is modified to " << after << FZENDL;
      }
    }
    return true;
  }
  return false;
}

// ----- Presolve rules -----

// Note on documentation
//
// In order to document presolve rules, we will use the following naming
// convention:
//   - x, x1, xi, y, y1, yi denote integer variables
//   - b, b1, bi denote boolean variables
//   - c, c1, ci denote integer constants
//   - t, t1, ti denote boolean constants
//   - => x after a constraint denotes the target variable of this constraint.
// Arguments are listed in order.

// Propagates cast constraint.
// Rule 1:
// Input: bool2int(b, c) or bool2int(t, x)
// Output: int_eq(...)
//
// Rule 2:
// Input: bool2int(b, x)
// Action: Replace all instances of x by b.
// Output: inactive constraint
bool Presolver::PresolveBool2Int(Constraint* ct, std::string* log) {
  if (ct->arguments[0].HasOneValue() || ct->arguments[1].HasOneValue()) {
    // Rule 1.
    log->append(
        "simplifying bool2int with one variable assigned to a single value");
    ct->type = "int_eq";
    return true;
  } else {
    // Rule 2.
    ct->MarkAsInactive();
    AddVariableSubstition(ct->arguments[1].Var(), ct->arguments[0].Var());
    return true;
  }
}

// Presolve equality constraint: int_eq
//
// Rule 1:
// Input : int_eq(x, 0) && x == y - z (stored in difference_map_).
// Output: int_eq(y, z)
//
// Rule 2:
// Input : int_eq(x, c)
// Action: Reduce domain of x to {c}
// Output: inactive constraint.
//
// Rule 3:
// Input : int_eq(x1, x2)
// Action: Pick x1 or x2, and replace all occurrences by the other. The prefered
//         direction is replace x2 by x1, unless x2 is already the target
//         variable of another constraint, because a variable cannot be the
//         target of 2 constraints.
// Output: inactive constraint.
//
// Rule 4:
// Input : int_eq(c, x)
// Action: Reduce domain of x to {c}
// Output: inactive constraint.
//
// Rule 5:
// Input : int_eq(c1, c2)
// Output: inactive constraint if c1 == c2, and do nothing if c1 != c2.
// TODO(user): reorder rules?
bool Presolver::PresolveIntEq(Constraint* ct, std::string* log) {
  // Rule 1
  if (ct->arguments[0].type == Argument::INT_VAR_REF &&
      ct->arguments[1].type == Argument::INT_VALUE &&
      ct->arguments[1].Value() == 0 &&
      ContainsKey(difference_map_, ct->arguments[0].Var())) {
    log->append("propagate equality");
    ct->arguments[0].Var()->domain.IntersectWithInterval(0, 0);

    log->append(", transform null differences");
    const std::pair<IntegerVariable*, IntegerVariable*>& diff =
        FindOrDie(difference_map_, ct->arguments[0].Var());
    ct->arguments[0].variables[0] = diff.first;
    ct->arguments[1].type = Argument::INT_VAR_REF;
    ct->arguments[1].values.clear();
    ct->arguments[1].variables.push_back(diff.second);
    return true;
  }
  if (ct->arguments[0].IsVariable()) {
    if (ct->arguments[1].HasOneValue()) {
      // Rule 2.
      const int64 value = ct->arguments[1].Value();
      log->append("propagate equality");
      ct->arguments[0].Var()->domain.IntersectWithInterval(value, value);
      ct->MarkAsInactive();
      return true;
    } else if (ct->arguments[1].IsVariable()) {
      // Rule 3.
      ct->MarkAsInactive();
      AddVariableSubstition(ct->arguments[0].Var(), ct->arguments[1].Var());
      return true;
    }
  } else if (ct->arguments[0].HasOneValue()) {  // Arg0 is an integer value.
    const int64 value = ct->arguments[0].Value();
    if (ct->arguments[1].IsVariable()) {
      // Rule 4.
      log->append("propagate equality");
      ct->arguments[1].Var()->domain.IntersectWithInterval(value, value);
      ct->MarkAsInactive();
      return true;
    } else if (ct->arguments[1].HasOneValue() &&
               value == ct->arguments[1].Value()) {
      // Rule 5.
      // No-op, removing.
      ct->MarkAsInactive();
      return false;
    }
  }
  return false;
}

// Propagates inequality constraint.
// Input : int_ne(x, c) or int_ne(c, x)
// Action: remove c from the domain of x.
// Output: inactive constraint if the removal was successful
//         (domain is not too large to remove a value).
bool Presolver::PresolveIntNe(Constraint* ct, std::string* log) {
  if (ct->presolve_propagation_done) {
    return false;
  }
  if ((ct->arguments[0].IsVariable() && ct->arguments[1].HasOneValue() &&
       (!ct->arguments[0].Var()->domain.Contains(ct->arguments[1].Value()) ||
        ct->arguments[0].Var()->domain.RemoveValue(
            ct->arguments[1].Value()))) ||
      (ct->arguments[1].IsVariable() && ct->arguments[0].HasOneValue() &&
       (!ct->arguments[1].Var()->domain.Contains(ct->arguments[0].Value()) ||
        ct->arguments[1].Var()->domain.RemoveValue(
            ct->arguments[0].Value())))) {
    log->append("remove value from variable domain");
    ct->MarkAsInactive();
    return true;
  }
  return false;
}

// Bound propagation on comparisons: int_le, bool_le, int_lt, bool_lt,
//                                   int_ge, bool_ge, int_gt, bool_gt.
//
// Rule 1:
// Input : int_XX(c1, c2) or bool_xx(c1, c2) with xx = lt, le, gt, ge
// Output: True or False constraint
// Rule 2:
// Input : int_xx(x, c) or int_xx(c, x) or bool_xx(x, c) or bool_xx(c, x)
//          with xx == lt, le, gt, ge
// Action: Reduce domain of x.
// Output: constraint is inactive.
//
// Rule 3:
// Input : int_xx(x, y) or bool_xx(x, y) with xx == lt, le, gt, ge.
// Action: Reduce domain of x and y.
// Output: constraint is still active.
bool Presolver::PresolveInequalities(Constraint* ct, std::string* log) {
  const std::string& id = ct->type;
  if (ct->arguments[0].variables.empty() &&
      ct->arguments[1].variables.empty()) {
    // Rule 1
    const int64 left = ct->arguments[0].Value();
    const int64 right = ct->arguments[1].Value();
    bool result = true;
    if (id == "int_le" || id == "bool_le") {
      result = left <= right;
    } else if (id == "int_lt" || id == "bool_lt") {
      result = left < right;
    } else if (id == "int_ge" || id == "bool_ge") {
      result = left >= right;
    } else if (id == "int_gt" || id == "bool_gt") {
      result = left > right;
    }
    if (result) {
      log->append("propagate bounds");
      ct->MarkAsInactive();
    } else {
      ct->SetAsFalse();
    }
    return true;
  }

  if (ct->arguments[0].IsVariable() && ct->arguments[1].HasOneValue()) {
    // Rule 2 where the 'var' is the left operand, eg. var <= 5
    IntegerVariable* const var = ct->arguments[0].Var();
    const int64 value = ct->arguments[1].Value();
    if (id == "int_le" || id == "bool_le") {
      var->domain.IntersectWithInterval(kint64min, value);
    } else if (id == "int_lt" || id == "bool_lt") {
      var->domain.IntersectWithInterval(kint64min, value - 1);
    } else if (id == "int_ge" || id == "bool_ge") {
      var->domain.IntersectWithInterval(value, kint64max);
    } else if (id == "int_gt" || id == "bool_gt") {
      var->domain.IntersectWithInterval(value + 1, kint64max);
    }
    ct->MarkAsInactive();
    return true;
  } else if (ct->arguments[0].HasOneValue() && ct->arguments[1].IsVariable()) {
    // Rule 2 where the 'var' is the right operand, eg 5 <= var
    IntegerVariable* const var = ct->arguments[1].Var();
    const int64 value = ct->arguments[0].Value();
    if (id == "int_le" || id == "bool_le") {
      var->domain.IntersectWithInterval(value, kint64max);
    } else if (id == "int_lt" || id == "bool_lt") {
      var->domain.IntersectWithInterval(value + 1, kint64max);
    } else if (id == "int_ge" || id == "bool_ge") {
      var->domain.IntersectWithInterval(kint64min, value);
    } else if (id == "int_gt" || id == "bool_gt") {
      var->domain.IntersectWithInterval(kint64min, value - 1);
    }
    ct->MarkAsInactive();
    return true;
  }
  // Rule 3.
  IntegerVariable* const left = ct->arguments[0].Var();
  const int64 left_min = left->domain.Min();
  const int64 left_max = left->domain.Max();
  IntegerVariable* const right = ct->arguments[1].Var();
  const int64 right_min = right->domain.Min();
  const int64 right_max = right->domain.Max();
  bool modified = false;
  if (id == "int_le" || id == "bool_le") {
    left->domain.IntersectWithInterval(kint64min, right_max);
    right->domain.IntersectWithInterval(left_min, kint64max);
    modified = left_max > right_max || right_min < left_min;
  } else if (id == "int_lt" || id == "bool_lt") {
    left->domain.IntersectWithInterval(kint64min, right_max - 1);
    right->domain.IntersectWithInterval(left_min + 1, kint64max);
    modified = left_max >= right_max || right_min <= left_min;
  } else if (id == "int_ge" || id == "bool_ge") {
    left->domain.IntersectWithInterval(right_min, kint64max);
    right->domain.IntersectWithInterval(kint64min, left_max);
    modified = right_max > left_max || left_min < right_min;
  } else if (id == "int_gt" || id == "bool_gt") {
    left->domain.IntersectWithInterval(right_min + 1, kint64max);
    right->domain.IntersectWithInterval(kint64min, left_max - 1);
    modified = right_max >= left_max || left_min <= right_min;
  }
  return modified;
}

// A reified constraint is a constraint that has been casted into a boolean
// variable that represents its status.
// Thus x == 3 can be reified into b == (x == 3).
//
// Rule 1:
// Input : int_xx_reif(arg1, arg2, true) or
//         int_lin_xx_reif(arg1, arg2, c, true)
//         with xx = eq, ne, le, lt, ge, gt
// Output: int_xx(arg1, arg2) or int_lin_xx(arg1, arg2, c)
//
// Rule 2:
// Input : int_xx_reif(arg1, arg2, false) or
//         int_lin_xx_reif(arg1, arg2, c, false)
//         with xx = eq, ne, le, lt, ge, gt
// Output: int_yy(arg1, arg2) or int_lin_yy(arg1, arg2, c)
//         with yy = opposite(xx). i.e. eq -> ne, le -> gt...
bool Presolver::Unreify(Constraint* ct, std::string* log) {
  const int last_argument = ct->arguments.size() - 1;
  if (!ct->arguments[last_argument].HasOneValue()) {
    return false;
  }
  DCHECK(HasSuffixString(ct->type, "_reif")) << ct->DebugString();
  ct->type.resize(ct->type.size() - 5);
  ct->RemoveTargetVariable();
  if (ct->arguments[last_argument].Value() == 1) {
    // Rule 1.
    log->append("unreify constraint");
    ct->RemoveTargetVariable();
    ct->arguments.pop_back();
  } else if (ct->type == "set_in" || ct->type == "set_not_in") {
    // Rule 2.
    log->append("unreify and reverse constraint");
    ct->RemoveTargetVariable();
    ct->arguments.pop_back();
    ct->type.resize(ct->type.size() - 2);
    ct->type += "not_in";
  } else {
    // Rule 2.
    log->append("unreify and reverse constraint");
    ct->RemoveTargetVariable();
    ct->arguments.pop_back();
    // Extract the 'operation' suffix of ct->type ("le", "eq", ...); i.e. the
    // last two characters.
    DCHECK_GT(ct->type.size(), 3);
    const std::string op = ct->type.substr(ct->type.size() - 2);
    ct->type.resize(ct->type.size() - 2);
    DCHECK(ct->type == "int_" || ct->type == "bool_" || ct->type == "int_lin_")
        << ct->type;
    // Now, change "op" to the inverse operation. The prefix of ct->type is
    // unchanged.
    if (op == "ne")
      ct->type += "eq";
    else if (op == "eq")
      ct->type += "ne";
    else if (op == "le")
      ct->type += "gt";
    else if (op == "lt")
      ct->type += "ge";
    else if (op == "ge")
      ct->type += "lt";
    else if (op == "gt")
      ct->type += "le";
  }
  return true;
}

// Propagates the values of set_in
// Input : set_in(x, [c1..c2]) or set_in(x, {c1, .., cn})
// Action: Intersect the domain of x with the set of values.
// Output: inactive constraint.
// note: set_in(x1, {x2, ...}) is plain illegal so we don't bother with it.
bool Presolver::PresolveSetIn(Constraint* ct, std::string* log) {
  if (ct->arguments[0].IsVariable()) {
    // IntersectDomainWith() will DCHECK that the second argument is a set
    // of constant values.
    log->append("propagate set on variable domain");
    IntersectDomainWith(ct->arguments[1], &ct->arguments[0].Var()->domain);
    ct->MarkAsInactive();
    // TODO(user): Returns true iff the intersection yielded some domain
    // reduction.
    return true;
  }
  return false;
}

// Propagates bound product.
// Input : int_times(c1, c2, x)
// Action: reduce domain of x to {c1 * c2}
// Output: inactive constraint.
bool Presolver::PresolveIntTimes(Constraint* ct, std::string* log) {
  if (ct->arguments[0].HasOneValue() && ct->arguments[1].HasOneValue() &&
      ct->arguments[2].IsVariable() && !ct->presolve_propagation_done) {
    log->append("propagate constants");
    const int64 value = ct->arguments[0].Value() * ct->arguments[1].Value();
    const int64 safe_value =
        CapProd(ct->arguments[0].Value(), ct->arguments[1].Value());
    if (value == safe_value) {
      ct->presolve_propagation_done = true;
      if (ct->arguments[2].Var()->domain.Contains(value)) {
        ct->arguments[2].Var()->domain.IntersectWithInterval(value, value);
        ct->MarkAsInactive();
        return true;
      } else {
        log->append(
            "  - product is not compatible with variable domain, "
            "ignoring presolve");
        // TODO(user): Treat failure correctly.
      }
    } else {
      log->append("  - product overflows, ignoring presolve");
      // TODO(user): Treat overflow correctly.
    }
  }
  return false;
}

// Propagates bound division.
// Input : int_div(c1, c2, x) (c2 != 0)
// Action: reduce domain of x to {c1 / c2}
// Output: inactive constraint.
bool Presolver::PresolveIntDiv(Constraint* ct, std::string* log) {
  if (ct->arguments[0].HasOneValue() && ct->arguments[1].HasOneValue() &&
      ct->arguments[2].IsVariable() && !ct->presolve_propagation_done &&
      ct->arguments[1].Value() != 0) {
    log->append("propagate constants");
    const int64 value = ct->arguments[0].Value() / ct->arguments[1].Value();
    ct->presolve_propagation_done = true;
    if (ct->arguments[2].Var()->domain.Contains(value)) {
      ct->arguments[2].Var()->domain.IntersectWithInterval(value, value);
      ct->MarkAsInactive();
      return true;
    } else {
      log->append(
          "  - division is not compatible with variable domain, "
          "ignoring presolve");
      // TODO(user): Treat failure correctly.
    }
  }
  // TODO(user): Catch c2 = 0 case and set the model to invalid.
  return false;
}

// Simplifies and reduces array_bool_or
//
// Rule 1:
// Input : array_bool_or([b1], b2)
// Output: bool_eq(b1, b2)
//
// Rule 2:
// Input : array_bool_or([b1, .., bn], false) or
//         array_bool_or([b1, .., bn], b0) with b0 assigned to false
// Action: Assign false to b1, .., bn
// Output: inactive constraint.
//
// Rule 3:
// Input : array_bool_or([b1, .., true, .., bn], b0)
// Action: Assign b0 to true
// Output: inactive constraint.
//
// Rule 4:
// Input : array_bool_or([false, .., false], b0), the array can be empty.
// Action: Assign b0 to false
// Output: inactive constraint.
//
// Rule 5:
// Input : array_bool_or([b1, .., false, bn], b0) or
//         array_bool_or([b1, .., bi, .., bn], b0) with bi assigned to false
// Action: Remove variables assigned to false values, or false constants.
// Output: array_bool_or([b1, .., bi-1, bi+1, .., bn], b0)
bool Presolver::PresolveArrayBoolOr(Constraint* ct, std::string* log) {
  if (ct->arguments[0].variables.size() == 1) {
    // Rule 1.
    ct->type = "bool_eq";
    ct->arguments[0].type = Argument::INT_VAR_REF;
    return true;
  }
  if (!ct->presolve_propagation_done && ct->arguments[1].HasOneValue() &&
      ct->arguments[1].Value() == 0) {
    // Rule 2.
    // TODO(user): Support empty domains correctly, and remove this test.
    for (const IntegerVariable* const var : ct->arguments[0].variables) {
      if (!var->domain.Contains(0)) {
        return false;
      }
    }
    log->append("propagate constants");
    for (IntegerVariable* const var : ct->arguments[0].variables) {
      var->domain.IntersectWithInterval(0, 0);
    }
    ct->MarkAsInactive();
    return true;
  }
  bool has_bound_true_value = false;
  std::vector<IntegerVariable*> unbound;
  for (IntegerVariable* const var : ct->arguments[0].variables) {
    if (var->domain.HasOneValue()) {
      has_bound_true_value |= var->domain.Min() == 1;
    } else {
      unbound.push_back(var);
    }
  }
  if (has_bound_true_value) {
    // Rule 3.
    if (!ct->arguments[1].HasOneValue()) {
      log->append("propagate target variable to true");
      ct->arguments[1].variables[0]->domain.IntersectWithInterval(1, 1);
      ct->MarkAsInactive();
      return true;
    } else if (ct->arguments[1].HasOneValue() &&
               ct->arguments[1].Value() == 1) {
      ct->MarkAsInactive();
      return true;
    }
    return false;
    // TODO(user): Simplify code once we support empty domains.
  }
  if (unbound.empty()) {
    // Rule 4.
    if (!ct->arguments[1].HasOneValue()) {
      // TODO(user): Simplify code once we support empty domains.
      log->append("propagate target variable to false");
      ct->arguments[1].variables[0]->domain.IntersectWithInterval(0, 0);
      ct->MarkAsInactive();
      return true;
    }
    return false;
  }
  if (unbound.size() < ct->arguments[0].variables.size()) {
    // Rule 5.
    log->append("Reduce array");
    ct->arguments[0].variables.swap(unbound);
    return true;
  }
  return false;
}

// Simplifies and reduces array_bool_and
//
// Rule 1:
// Input : array_bool_and([b1], b2)
// Output: bool_eq(b1, b2)
//
// Rule 2:
// Input : array_bool_and([b1, .., bn], true)
// Action: Assign b1, .., bn to true
// Output: inactive constraint.
//
// Rule 3:
// Input : array_bool_and([b1, .., false, .., bn], b0)
// Action: Assign b0 to false
// Output: inactive constraint.
//
// Rule 4:
// Input : array_bool_and([true, .., true], b0)
// Action: Assign b0 to true
// Output: inactive constraint.
//
// Rule 5:
// Input : array_bool_and([b1, .., true, bn], b0)
// Action: Remove all the true values.
// Output: array_bool_and([b1, .., bi-1, bi+1, .., bn], b0)
bool Presolver::PresolveArrayBoolAnd(Constraint* ct, std::string* log) {
  if (ct->arguments[0].variables.size() == 1) {
    // Rule 1.
    ct->type = "bool_eq";
    ct->arguments[0].type = Argument::INT_VAR_REF;
    return true;
  }
  if (!ct->presolve_propagation_done && ct->arguments[1].HasOneValue() &&
      ct->arguments[1].Value() == 1) {
    // Rule 2.
    // TODO(user): Simplify the code once we support empty domains.
    for (const IntegerVariable* const var : ct->arguments[0].variables) {
      if (!var->domain.Contains(1)) {
        return false;
      }
    }
    log->append("propagate constants");
    for (IntegerVariable* const var : ct->arguments[0].variables) {
      var->domain.IntersectWithInterval(1, 1);
    }
    ct->presolve_propagation_done = true;
    ct->MarkAsInactive();
    return true;
  }
  int has_bound_false_value = 0;
  std::vector<IntegerVariable*> unbound;
  for (IntegerVariable* const var : ct->arguments[0].variables) {
    if (var->domain.HasOneValue()) {
      has_bound_false_value |= var->domain.Max() == 0;
    } else {
      unbound.push_back(var);
    }
  }
  if (has_bound_false_value) {
    // TODO(user): Simplify the code once we support empty domains.
    if (!ct->arguments[1].HasOneValue()) {
      // Rule 3.
      log->append("propagate target variable to false");
      ct->arguments[1].variables[0]->domain.IntersectWithInterval(0, 0);
      ct->MarkAsInactive();
      return true;
    } else if (ct->arguments[1].HasOneValue() &&
               ct->arguments[1].Value() == 0) {
      ct->MarkAsInactive();
      return true;
    }
    return false;
  }
  if (unbound.empty()) {
    // Rule 4.
    if (!ct->arguments[1].HasOneValue()) {
      log->append("propagate target variable to true");
      ct->arguments[1].variables[0]->domain.IntersectWithInterval(1, 1);
      ct->MarkAsInactive();
      return true;
    }
    return false;
  }
  if (unbound.size() < ct->arguments[0].variables.size()) {
    log->append("reduce array");
    ct->arguments[0].variables.swap(unbound);
    return true;
  }
  return false;
}

// Simplifies bool_XX_reif(b1, b2, b3)  (which means b3 = (b1 XX b2)) when the
// middle value is bound.
// Input: bool_XX_reif(b1, t, b2), where XX is "eq" or "ne".
// Output: bool_YY(b1, b2) where YY is "eq" or "not" depending on XX and t.
bool Presolver::PresolveBoolEqNeReif(Constraint* ct, std::string* log) {
  DCHECK(ct->type == "bool_eq_reif" || ct->type == "bool_ne_reif");
  if (ct->arguments[1].HasOneValue()) {
    log->append("simplify constraint");
    const int64 value = ct->arguments[1].Value();
    // Remove boolean value argument.
    ct->RemoveArg(1);
    // Change type.
    ct->type = ((ct->type == "bool_eq_reif" && value == 1) ||
                (ct->type == "bool_ne_reif" && value == 0))
                   ? "bool_eq"
                   : "bool_not";
    return true;
  }
  if (ct->arguments[0].HasOneValue()) {
    log->append("simplify constraint");
    const int64 value = ct->arguments[0].Value();
    // Remove boolean value argument.
    ct->RemoveArg(0);
    // Change type.
    ct->type = ((ct->type == "bool_eq_reif" && value == 1) ||
                (ct->type == "bool_ne_reif" && value == 0))
                   ? "bool_eq"
                   : "bool_not";
    return true;
  }
  return false;
}

// Transform int_lin_gt (which means ScalProd(arg1[], arg2[]) > c) into
// int_lin_ge.
// Input : int_lin_gt(arg1, arg2, c)
// Output: int_lin_ge(arg1, arg2, c + 1)
bool Presolver::PresolveIntLinGt(Constraint* ct, std::string* log) {
  CHECK_EQ(Argument::INT_VALUE, ct->arguments[2].type);
  if (ct->arguments[2].Value() != kint64max) {
    ct->arguments[2].values[0]++;
    ct->type = "int_lin_ge";
    return true;
  }
  // TODO(user): fail (the model is impossible: a * b > kint64max can be
  // considered as impossible; because it would imply an overflow; which we
  // reject.
  return false;
}

// Transform int_lin_lt into int_lin_le.
// Input : int_lin_lt(arg1, arg2, c)
// Output: int_lin_le(arg1, arg2, c - 1)
bool Presolver::PresolveIntLinLt(Constraint* ct, std::string* log) {
  CHECK_EQ(Argument::INT_VALUE, ct->arguments[2].type);
  if (ct->arguments[2].Value() != kint64min) {
    ct->arguments[2].values[0]--;
    ct->type = "int_lin_le";
    return true;
  }
  // TODO(user): fail (the model is impossible: a * b < kint64min can be
  // considered as impossible; because it would imply an overflow; which we
  // reject.
  return false;
}

// Simplifies linear equations of size 1, i.e. c1 * x = c2.
// Input : int_lin_xx([c1], [x], c2) and int_lin_xx_reif([c1], [x], c2, b)
//         with (c1 == 1 or c2 % c1 == 0) and xx = eq, ne, lt, le, gt, ge
// Output: int_xx(x, c2 / c1) and int_xx_reif(x, c2 / c1, b)
bool Presolver::SimplifyUnaryLinear(Constraint* ct, std::string* log) {
  const int64 coefficient = ct->arguments[0].values[0];
  const int64 rhs = ct->arguments[2].Value();
  if (ct->arguments[0].values.size() == 1 &&
      (coefficient == 1 || (coefficient > 0 && rhs % coefficient == 0))) {
    // TODO(user): Support coefficient = 0.
    // TODO(user): Support coefficient < 0 (and reverse the inequalities).
    // TODO(user): Support rhs % coefficient != 0, and no the correct
    // rounding in the case of inequalities, of false model in the case of
    // equalities.
    log->append("remove linear part");
    // transform arguments.
    ct->arguments[0].type = Argument::INT_VAR_REF;
    ct->arguments[0].values.clear();
    ct->arguments[0].variables.push_back(ct->arguments[1].variables[0]);
    ct->arguments[1].type = Argument::INT_VALUE;
    ct->arguments[1].variables.clear();
    ct->arguments[1].values.push_back(rhs / coefficient);
    ct->RemoveArg(2);
    // Change type (remove "_lin" part).
    DCHECK(ct->type.size() >= 8 && ct->type.substr(3, 4) == "_lin");
    ct->type.erase(3, 4);
    FZVLOG << "  - " << ct->DebugString() << FZENDL;
    return true;
  }
  return false;
}

// Simplifies linear equations of size 2, i.e. x - y = 0.
// Input : int_lin_xx([1, -1], [x1, x2], 0) and
//         int_lin_xx_reif([1, -1], [x1, x2], 0, b)
//         xx = eq, ne, lt, le, gt, ge
// Output: int_xx(x1, x2) and int_xx_reif(x, x2, b)
bool Presolver::SimplifyBinaryLinear(Constraint* ct, std::string* log) {
  const int64 rhs = ct->arguments[2].Value();
  if (ct->arguments[0].values.size() != 2 || rhs != 0 ||
      ct->arguments[1].variables.size() == 0) {
    return false;
  }

  IntegerVariable* first = nullptr;
  IntegerVariable* second = nullptr;
  if (ct->arguments[0].values[0] == 1 && ct->arguments[0].values[1] == -1) {
    first = ct->arguments[1].variables[0];
    second = ct->arguments[1].variables[1];
  } else if (ct->arguments[0].values[0] == -1 &&
             ct->arguments[0].values[1] == 1) {
    first = ct->arguments[1].variables[1];
    second = ct->arguments[1].variables[0];
  } else {
    return false;
  }

  log->append("remove linear part");
  ct->arguments[0].type = Argument::INT_VAR_REF;
  ct->arguments[0].values.clear();
  ct->arguments[0].variables.push_back(first);
  ct->arguments[1].type = Argument::INT_VAR_REF;
  ct->arguments[1].variables.clear();
  ct->arguments[1].variables.push_back(second);
  ct->RemoveArg(2);
  // Change type (remove "_lin" part).
  DCHECK(ct->type.size() >= 8 && ct->type.substr(3, 4) == "_lin");
  ct->type.erase(3, 4);
  FZVLOG << "  - " << ct->DebugString() << FZENDL;
  return true;
}

// Returns false if an overflow occured.
// Used by CheckIntLinReifBounds() below: compute the bounds of the scalar
// product. If an integer overflow occurs the method returns false.
namespace {
bool ComputeLinBounds(const std::vector<int64>& coefficients,
                      const std::vector<IntegerVariable*>& variables, int64* lb,
                      int64* ub) {
  CHECK_EQ(coefficients.size(), variables.size()) << "Wrong constraint";
  *lb = 0;
  *ub = 0;
  for (int i = 0; i < coefficients.size(); ++i) {
    const IntegerVariable* const var = variables[i];
    const int64 coef = coefficients[i];
    if (coef == 0) continue;
    if (var->domain.Min() == kint64min || var->domain.Max() == kint64max) {
      return false;
    }
    const int64 vmin = var->domain.Min();
    const int64 vmax = var->domain.Max();
    const int64 min_delta =
        coef > 0 ? CapProd(vmin, coef) : CapProd(vmax, coef);
    const int64 max_delta =
        coef > 0 ? CapProd(vmax, coef) : CapProd(vmin, coef);
    *lb = CapAdd(*lb, min_delta);
    *ub = CapAdd(*ub, max_delta);
    if (*lb == kint64min || min_delta == kint64min || min_delta == kint64max ||
        max_delta == kint64min || max_delta == kint64max || *ub == kint64max) {
      // Overflow
      return false;
    }
  }
  return true;
}
}  // namespace

// Presolve: Check bounds of int_lin_eq_reif w.r.t. the boolean variable.
// Input : int_lin_eq_reif([c1, .., cn], [x1, .., xn], c0, b)
// Action: compute min and max of sum(x1 * c2) and
//         assign true to b is min == max == c0, or
//         assign false to b if min > c0 or max < c0,
//         or do nothing and keep the constraint active.
bool Presolver::CheckIntLinReifBounds(Constraint* ct, std::string* log) {
  DCHECK_EQ(ct->type, "int_lin_eq_reif");
  int64 lb = 0;
  int64 ub = 0;
  if (!ComputeLinBounds(ct->arguments[0].values, ct->arguments[1].variables,
                        &lb, &ub)) {
    log->append("overflow found when presolving");
    return false;
  }
  const int64 value = ct->arguments[2].Value();
  if (value < lb || value > ub) {
    log->append("assign boolean to false");
    ct->arguments[3].Var()->domain.IntersectWithInterval(0, 0);
    ct->MarkAsInactive();
    return true;
  } else if (value == lb && value == ub) {
    log->append("assign boolean to true");
    ct->arguments[3].Var()->domain.IntersectWithInterval(1, 1);
    ct->MarkAsInactive();
    return true;
  }
  return false;
}

// Marks target variable: int_lin_eq
// On two-variable linear equality constraints of the form -x + c0 * y = c1;
// mark x as the "target" of the constraint, i.e. the variable that is "defined"
// by the constraint. We do that only if the constraint doesn't already have a
// target variable and if x doesn't have a defining constraint.
//
// Rule 1:
// Input : int_lin_eq([[-1, c2], x1, x2], c0)
// Output: int_lin_eq([-1, c2], [x1, x2], c0) => x1, mark x1.
//
// Rule 2:
// Input : int_lin_eq([c1, -1], [x1, x2], c0)
// Output: int_lin_eq([c1, -1], [x1, x2], c0) => x2, mark x2.
bool Presolver::CreateLinearTarget(Constraint* ct, std::string* log) {
  if (ct->target_variable != nullptr) return false;

  for (const int var_index : {0, 1}) {
    if (ct->arguments[0].values.size() == 2 &&
        ct->arguments[0].values[var_index] == -1 &&
        ct->arguments[1].variables[var_index]->defining_constraint == nullptr &&
        !ct->arguments[1].variables[var_index]->domain.HasOneValue()) {
      // Rule 1.
      StringAppendF(log, "mark variable index %i as target", var_index);
      IntegerVariable* const var = ct->arguments[1].variables[var_index];
      var->defining_constraint = ct;
      ct->target_variable = var;
      return true;
    }
  }
  return false;
}

// Propagates: array_int_element
// Rule 1:
// Input : array_int_element(x, [c1, .., cn], y)
// Output: array_int_element(x, [c1, .., cm], y) if all cm+1, .., cn are not
//         the domain of y.
//
// Rule 2:
// Input : array_int_element(x, [c1, .., cn], y)
// Action: Intersect the domain of y with the set of values.
bool Presolver::PresolveArrayIntElement(Constraint* ct, std::string* log) {
  if (ct->arguments[0].variables.size() == 1) {
    if (!ct->arguments[0].HasOneValue()) {
      // Rule 1.
      const int64 target_min = ct->arguments[2].HasOneValue()
                                   ? ct->arguments[2].Value()
                                   : ct->arguments[2].Var()->domain.Min();
      const int64 target_max = ct->arguments[2].HasOneValue()
                                   ? ct->arguments[2].Value()
                                   : ct->arguments[2].Var()->domain.Max();

      int64 last_index = ct->arguments[1].values.size();
      last_index = std::min(ct->arguments[0].Var()->domain.Max(), last_index);

      while (last_index >= 1) {
        const int64 value = ct->arguments[1].values[last_index - 1];
        if (value < target_min || value > target_max) {
          last_index--;
        } else {
          break;
        }
      }

      int64 first_index = 1;
      first_index = std::max(ct->arguments[0].Var()->domain.Min(), first_index);
      while (first_index <= last_index) {
        const int64 value = ct->arguments[1].values[first_index - 1];
        if (value < target_min || value > target_max) {
          first_index++;
        } else {
          break;
        }
      }

      if (last_index < ct->arguments[0].Var()->domain.Max() ||
          first_index > ct->arguments[0].Var()->domain.Min()) {
        StringAppendF(log, "filter index to [%" GG_LL_FORMAT "d..%" GG_LL_FORMAT
                           "d] and reduce array to size %" GG_LL_FORMAT "d",
                      first_index, last_index, last_index);
        ct->arguments[0].Var()->domain.IntersectWithInterval(first_index,
                                                             last_index);
        ct->arguments[1].values.resize(last_index);
        return true;
      }
    }
  }
  if (ct->arguments[2].IsVariable() && !ct->presolve_propagation_done) {
    // Rule 2.
    log->append("propagate domain");
    IntersectDomainWith(ct->arguments[1], &ct->arguments[2].Var()->domain);
    ct->presolve_propagation_done = true;
    return true;
  }
  return false;
}

// Reverses a linear constraint: with negative coefficients.
// Rule 1:
// Input : int_lin_xxx([-c1, .., -cn], [x1, .., xn], c0) or
//         int_lin_xxx_reif([-c1, .., -cn], [x1, .., xn], c0, b) or
//         with c1, cn > 0
// Output: int_lin_yyy([c1, .., cn], [c1, .., cn], c0) or
//         int_lin_yyy_reif([c1, .., cn], [c1, .., cn], c0, b)
//         with yyy is the opposite of xxx (eq -> eq, ne -> ne, le -> ge,
//                                          lt -> gt, ge -> le, gt -> lt)
//
// Rule 2:
// Input: int_lin_xxx[[c1, .., cn], [c'1, .., c'n], c0]  (no variables)
// Output: inactive or false constraint.
//
// Rule 3:
// Input: int_lin_xxx_reif[[c1, .., cn], [c'1, .., c'n], c0]  (no variables)
// Output: bool_eq(c0, true or false).
bool Presolver::PresolveLinear(Constraint* ct, std::string* log) {
  if (ct->arguments[0].values.empty()) {
    return false;
  }
  // Rule 2.
  if (ct->arguments[1].variables.empty()) {
    log->append("rewrite constant linear equation");
    CHECK(!ct->arguments[1].values.empty());
    int64 scalprod = 0;
    for (int i = 0; i < ct->arguments[0].values.size(); ++i) {
      scalprod += ct->arguments[0].values[i] * ct->arguments[1].values[i];
    }
    const int64 rhs = ct->arguments[2].Value();
    if (ct->type == "int_lin_eq") {
      if (scalprod == rhs) {
        ct->MarkAsInactive();
      } else {
        ct->SetAsFalse();
      }
    } else if (ct->type == "int_lin_le") {
      if (scalprod <= rhs) {
        ct->MarkAsInactive();
      } else {
        ct->SetAsFalse();
      }
    } else if (ct->type == "int_lin_ge") {
      if (scalprod >= rhs) {
        ct->MarkAsInactive();
      } else {
        ct->SetAsFalse();
      }
    } else if (ct->type == "int_lin_ne") {
      if (scalprod != rhs) {
        ct->MarkAsInactive();
      } else {
        ct->SetAsFalse();
      }
      // Rule 3
    } else if (ct->type == "int_lin_eq_reif") {
      ct->type = "bool_eq";
      ct->arguments[0] = ct->arguments[3];
      ct->arguments.resize(1);
      ct->arguments.push_back(Argument::IntegerValue(scalprod == rhs));
    } else if (ct->type == "int_lin_ge") {
      if (scalprod >= rhs) {
        ct->MarkAsInactive();
      } else {
        ct->SetAsFalse();
      }
    } else if (ct->type == "int_lin_ge_reif") {
      ct->type = "bool_eq";
      ct->arguments[0] = ct->arguments[3];
      ct->arguments.resize(1);
      ct->arguments.push_back(Argument::IntegerValue(scalprod >= rhs));
    } else if (ct->type == "int_lin_le") {
      if (scalprod <= rhs) {
        ct->MarkAsInactive();
      } else {
        ct->SetAsFalse();
      }
    } else if (ct->type == "int_lin_le_reif") {
      ct->type = "bool_eq";
      ct->arguments[0] = ct->arguments[3];
      ct->arguments.resize(1);
      ct->arguments.push_back(Argument::IntegerValue(scalprod <= rhs));
    } else if (ct->type == "int_lin_ne_reif") {
      ct->type = "bool_eq";
      ct->arguments[0] = ct->arguments[3];
      ct->arguments.resize(1);
      ct->arguments.push_back(Argument::IntegerValue(scalprod != rhs));
    }
    return true;
  }

  // Rule 1.
  for (const int64 coef : ct->arguments[0].values) {
    if (coef > 0) {
      return false;
    }
  }
  if (ct->target_variable != nullptr) {
    for (IntegerVariable* const var : ct->arguments[1].variables) {
      if (var == ct->target_variable) {
        return false;
      }
    }
  }
  log->append("reverse constraint");
  for (int64& coef : ct->arguments[0].values) {
    coef *= -1;
  }
  ct->arguments[2].values[0] *= -1;
  if (ct->type == "int_lin_le") {
    ct->type = "int_lin_ge";
  } else if (ct->type == "int_lin_lt") {
    ct->type = "int_lin_gt";
  } else if (ct->type == "int_lin_ge") {
    ct->type = "int_lin_le";
  } else if (ct->type == "int_lin_gt") {
    ct->type = "int_lin_lt";
  } else if (ct->type == "int_lin_le_reif") {
    ct->type = "int_lin_ge_reif";
  } else if (ct->type == "int_lin_ge_reif") {
    ct->type = "int_lin_le_reif";
  }
  return true;
}

// Regroup linear term with the same variable.
// Input : int_lin_xxx([c1, .., cn], [x1, .., xn], c0) with xi = xj
// Output: int_lin_xxx([c1, .., ci + cj, .., cn], [x1, .., xi, .., xn], c0)
bool Presolver::RegroupLinear(Constraint* ct, std::string* log) {
  if (ct->arguments[1].variables.empty()) {
    // Only constants, or size == 0.
    return false;
  }
  hash_map<const IntegerVariable*, int64> coefficients;
  const int original_size = ct->arguments[0].values.size();
  for (int i = 0; i < original_size; ++i) {
    coefficients[ct->arguments[1].variables[i]] += ct->arguments[0].values[i];
  }
  if (coefficients.size() != original_size) {  // Duplicate variables.
    log->append("regroup variables");
    hash_set<const IntegerVariable*> processed;
    int index = 0;
    int zero = 0;
    for (int i = 0; i < original_size; ++i) {
      IntegerVariable* fz_var = ct->arguments[1].variables[i];
      const int64 coefficient = coefficients[fz_var];
      if (!ContainsKey(processed, fz_var)) {
        processed.insert(fz_var);
        if (coefficient != 0) {
          ct->arguments[1].variables[index] = fz_var;
          ct->arguments[0].values[index] = coefficient;
          index++;
        } else {
          zero++;
        }
      }
    }
    CHECK_EQ(index + zero, coefficients.size());
    ct->arguments[0].values.resize(index);
    ct->arguments[1].variables.resize(index);
    return true;
  }
  return false;
}

// Bound propagation: int_lin_eq, int_lin_le, int_lin_ge
//
// Rule 1:
// Input : int_lin_xx([c1, .., cn], [x1, .., xn],  c0) with ci >= 0 and
//         xi are variables with positive domain.
// Action: if xx = eq or le, intersect the domain of xi with [0, c0 / ci]
//
// Rule 2:
// Input : int_lin_xx([c1], [x1], c0) with c1 >= 0, and xx = eq, ge.
// Action: intersect the domain of x1 with [c0/c1, kint64max]
bool Presolver::PropagatePositiveLinear(Constraint* ct, std::string* log) {
  const int64 rhs = ct->arguments[2].Value();
  if (ct->presolve_propagation_done || rhs < 0 ||
      ct->arguments[1].variables.empty()) {
    return false;
  }
  for (const int64 coef : ct->arguments[0].values) {
    if (coef < 0) {
      return false;
    }
  }
  for (IntegerVariable* const var : ct->arguments[1].variables) {
    if (var->domain.Min() < 0) {
      return false;
    }
  }
  bool modified = false;
  if (ct->type != "int_lin_ge") {
    // Rule 1.
    log->append("propagate constants");
    for (int i = 0; i < ct->arguments[0].values.size(); ++i) {
      const int64 coef = ct->arguments[0].values[i];
      if (coef > 0) {
        IntegerVariable* const var = ct->arguments[1].variables[i];
        const int64 bound = rhs / coef;
        if (bound < var->domain.Max()) {
          StringAppendF(log, ", intersect %s with [0..%" GG_LL_FORMAT "d]",
                        var->DebugString().c_str(), bound);
          var->domain.IntersectWithInterval(0, bound);
          modified = true;
        }
      }
    }
  } else if (ct->arguments[0].values.size() == 1 &&
             ct->arguments[0].values[0] > 0) {
    // Rule 2.
    const int64 coef = ct->arguments[0].values[0];
    IntegerVariable* const var = ct->arguments[1].variables[0];
    const int64 bound = (rhs + coef - 1) / coef;
    if (bound > var->domain.Min()) {
      StringAppendF(log, ", intersect %s with [%" GG_LL_FORMAT "d .. INT_MAX]",
                    var->DebugString().c_str(), bound);
      var->domain.IntersectWithInterval(bound, kint64max);
      ct->MarkAsInactive();
      modified = true;
    }
  }
  ct->presolve_propagation_done = true;
  return modified;
}

// Minizinc flattens 2d element constraints (x = A[y][z]) into 1d element
// constraint with an affine mapping between y, z and the new index.
// This rule stores the mapping to reconstruct the 2d element constraint.
// This mapping can involve 1 or 2 variables dependening if y or z in A[y][z]
// is a constant in the model).
bool Presolver::PresolveStoreMapping(Constraint* ct, std::string* log) {
  if (ct->arguments[1].variables.empty()) {
    // Constant linear constraint (no variables).
    return false;
  }
  if (ct->arguments[0].values.size() == 2 &&
      ct->arguments[1].variables[0] == ct->target_variable &&
      ct->arguments[0].values[0] == -1 &&
      !ContainsKey(affine_map_, ct->target_variable) &&
      ct->strong_propagation) {
    affine_map_[ct->target_variable] =
        AffineMapping(ct->arguments[1].variables[1], ct->arguments[0].values[1],
                      -ct->arguments[2].Value(), ct);
    log->append("store affine mapping");
    return true;
  }
  if (ct->arguments[0].values.size() == 2 &&
      ct->arguments[1].variables[1] == ct->target_variable &&
      ct->arguments[0].values[1] == -1 &&
      !ContainsKey(affine_map_, ct->target_variable)) {
    affine_map_[ct->target_variable] =
        AffineMapping(ct->arguments[1].variables[0], ct->arguments[0].values[0],
                      -ct->arguments[2].Value(), ct);
    log->append("store affine mapping");
    return true;
  }
  if (ct->arguments[0].values.size() == 3 &&
      ct->arguments[1].variables[0] == ct->target_variable &&
      ct->arguments[0].values[0] == -1 && ct->arguments[0].values[2] == 1 &&
      !ContainsKey(array2d_index_map_, ct->target_variable) &&
      ct->strong_propagation) {
    array2d_index_map_[ct->target_variable] = Array2DIndexMapping(
        ct->arguments[1].variables[1], ct->arguments[0].values[1],
        ct->arguments[1].variables[2], -ct->arguments[2].Value(), ct);
    log->append("store affine mapping");
    return true;
  }
  if (ct->arguments[0].values.size() == 3 &&
      ct->arguments[1].variables[0] == ct->target_variable &&
      ct->arguments[0].values[0] == -1 && ct->arguments[0].values[1] == 1 &&
      !ContainsKey(array2d_index_map_, ct->target_variable) &&
      ct->strong_propagation) {
    array2d_index_map_[ct->target_variable] = Array2DIndexMapping(
        ct->arguments[1].variables[2], ct->arguments[0].values[2],
        ct->arguments[1].variables[1], -ct->arguments[2].Value(), ct);
    log->append("store affine mapping");
    return true;
  }
  if (ct->arguments[0].values.size() == 3 &&
      ct->arguments[1].variables[2] == ct->target_variable &&
      ct->arguments[0].values[2] == -1 && ct->arguments[0].values[1] == 1 &&
      !ContainsKey(array2d_index_map_, ct->target_variable)) {
    array2d_index_map_[ct->target_variable] = Array2DIndexMapping(
        ct->arguments[1].variables[0], ct->arguments[0].values[0],
        ct->arguments[1].variables[1], -ct->arguments[2].Value(), ct);
    log->append("store affine mapping");
    return true;
  }
  if (ct->arguments[0].values.size() == 3 &&
      ct->arguments[1].variables[2] == ct->target_variable &&
      ct->arguments[0].values[2] == -1 && ct->arguments[0].values[0] == 1 &&
      !ContainsKey(array2d_index_map_, ct->target_variable)) {
    array2d_index_map_[ct->target_variable] = Array2DIndexMapping(
        ct->arguments[1].variables[1], ct->arguments[0].values[1],
        ct->arguments[1].variables[0], -ct->arguments[2].Value(), ct);
    log->append("store affine mapping");
    return true;
  }
  return false;
}

namespace {
bool IsIncreasingContiguous(const std::vector<int64>& values) {
  for (int i = 0; i < values.size() - 1; ++i) {
    if (values[i + 1] != values[i] + 1) {
      return false;
    }
  }
  return true;
}
}  // namespace

// Rewrite array element: array_int_element:
//
// Rule1:
// Input : array_int_element(x0, [c1, .., cn], y) with x0 = a * x + b
// Output: array_int_element(x, [c_a1, .., c_am], b) with a * i = b = ai
//
// Rule 2:
// Input : array_int_element(x, [c1, .., cn], y) with x = a * x1 + x2 + b
// Output: array_int_element([x1, x2], [c_a1, .., c_am], b, [a, b])
//         to be interpreted by the extraction process.
// Rule3:
// Input : array_int_element(x, [c1, .., cn], y) with x fixed one value.
// Output: int_eq(b, c_x.Value())
//
// Rule 4:
// Input : array_int_element(x, [c1, .., cn], y) with x0 ci = c0 + i
// Output: int_lin_eq([-1, 1], [y, x], 1 - c)  (e.g. y = x + c - 1)
bool Presolver::PresolveSimplifyElement(Constraint* ct, std::string* log) {
  if (ct->arguments[0].variables.size() > 1) {
    return false;
  }
  IntegerVariable* const index_var = ct->arguments[0].Var();
  if (ContainsKey(affine_map_, index_var)) {
    // Rule 1.
    const AffineMapping& mapping = affine_map_[index_var];
    const Domain& domain = mapping.variable->domain;
    if (domain.is_interval && domain.values.empty()) {
      // Invalid case. Ignore it.
      return false;
    }
    if (domain.values[0] == 0 && mapping.coefficient == 1 &&
        mapping.offset > 1 && index_var->domain.is_interval) {
      log->append("reduce constraint");
      // Simple translation
      const int offset = mapping.offset - 1;
      const int size = ct->arguments[1].values.size();
      for (int i = 0; i < size - offset; ++i) {
        ct->arguments[1].values[i] = ct->arguments[1].values[i + offset];
      }
      ct->arguments[1].values.resize(size - offset);
      affine_map_[index_var].constraint->arguments[2].values[0] = -1;
      affine_map_[index_var].offset = 1;
      index_var->domain.values[0] -= offset;
      index_var->domain.values[1] -= offset;
      return true;
    } else if (mapping.offset + mapping.coefficient > 0 &&
               domain.values[0] > 0) {
      const std::vector<int64>& values = ct->arguments[1].values;
      std::vector<int64> new_values;
      for (int64 i = 1; i <= domain.values.back(); ++i) {
        const int64 index = i * mapping.coefficient + mapping.offset - 1;
        if (index < 0) {
          return false;
        }
        if (index > values.size()) {
          break;
        }
        new_values.push_back(values[index]);
      }
      // Rewrite constraint.
      log->append("simplify constraint");
      ct->arguments[0].variables[0] = mapping.variable;
      ct->arguments[0].variables[0]->domain.IntersectWithInterval(
          1, new_values.size());
      // TODO(user): Encapsulate argument setters.
      ct->arguments[1].values.swap(new_values);
      if (ct->arguments[1].values.size() == 1) {
        ct->arguments[1].type = Argument::INT_VALUE;
      }
      // Reset propagate flag.
      ct->presolve_propagation_done = false;
      // Mark old index var and affine constraint as presolved out.
      mapping.constraint->MarkAsInactive();
      index_var->active = false;
      return true;
    }
  }
  if (ContainsKey(array2d_index_map_, index_var)) {
    log->append("rewrite as a 2d element");
    const Array2DIndexMapping& mapping = array2d_index_map_[index_var];
    // Rewrite constraint.
    ct->arguments[0].variables[0] = mapping.variable1;
    ct->arguments[0].variables.push_back(mapping.variable2);
    ct->arguments[0].type = Argument::INT_VAR_REF_ARRAY;
    std::vector<int64> coefs;
    coefs.push_back(mapping.coefficient);
    coefs.push_back(1);
    ct->arguments.push_back(Argument::IntegerList(coefs));
    ct->arguments.push_back(Argument::IntegerValue(mapping.offset));
    if (ct->target_variable != nullptr) {
      ct->RemoveTargetVariable();
    }
    index_var->active = false;
    mapping.constraint->MarkAsInactive();
    // TODO(user): Check if presolve is valid.
    return true;
  }
  if (index_var->domain.HasOneValue()) {
    // Rule 3.
    const int64 index = index_var->domain.values[0] - 1;
    const int64 value = ct->arguments[1].values[index];
    // Rewrite as equality.
    ct->type = "int_eq";
    ct->arguments[0].variables.clear();
    ct->arguments[0].values.push_back(value);
    ct->arguments[0].type = Argument::INT_VALUE;
    ct->RemoveArg(1);
    FZVLOG << "  -> " << ct->DebugString() << FZENDL;
    return true;
  }
  if (index_var->domain.is_interval && index_var->domain.values.size() == 2 &&
      index_var->domain.Max() < ct->arguments[1].values.size()) {
    // Reduce array of values.
    ct->arguments[1].values.resize(index_var->domain.Max());
    ct->presolve_propagation_done = false;
    log->append("reduce array");
    return true;
  }
  if (IsIncreasingContiguous(ct->arguments[1].values)) {
    // Rule 4.
    const int64 start = ct->arguments[1].values.front();
    IntegerVariable* const index = ct->arguments[0].Var();
    IntegerVariable* const target = ct->arguments[2].Var();
    log->append("linearize constraint");

    if (start == 1) {
      ct->type = "int_eq";
      ct->RemoveArg(1);
    } else {
      // Rewrite constraint into a int_lin_eq
      ct->type = "int_lin_eq";
      ct->arguments[0].type = Argument::INT_LIST;
      ct->arguments[0].variables.clear();
      ct->arguments[0].values.push_back(-1);
      ct->arguments[0].values.push_back(1);
      ct->arguments[1].type = Argument::INT_VAR_REF_ARRAY;
      ct->arguments[1].values.clear();
      ct->arguments[1].variables.push_back(target);
      ct->arguments[1].variables.push_back(index);
      ct->arguments[2].type = Argument::INT_VALUE;
      ct->arguments[2].variables.clear();
      ct->arguments[2].values.push_back(1 - start);
    }

    return true;
  }
  return false;
}

// Simplifies array_var_int_element
//
// Rule1:
// Input : array_var_int_element(x0, [x1, .., xn], y) with xi(1..n) having one
//         value
// Output: array_int_element(x0, [x1.Value(), .., xn.Value()], y)
//
// Rule2:
// Input : array_var_int_element(x0, [x1, .., xn], y) with x0 = a * x + b
// Output: array_var_int_element(x, [x_a1, .., x_an], b) with a * i = b = ai
bool Presolver::PresolveSimplifyExprElement(Constraint* ct, std::string* log) {
  bool all_integers = true;
  for (IntegerVariable* const var : ct->arguments[1].variables) {
    if (!var->domain.HasOneValue()) {
      all_integers = false;
      break;
    }
  }
  if (all_integers) {
    // Rule 1:
    log->append("rewrite constraint as array_int_element");
    ct->type = "array_int_element";
    ct->arguments[1].type = Argument::INT_LIST;
    for (int i = 0; i < ct->arguments[1].variables.size(); ++i) {
      ct->arguments[1].values.push_back(
          ct->arguments[1].variables[i]->domain.Min());
    }
    ct->arguments[1].variables.clear();
    return true;
  }
  IntegerVariable* const index_var = ct->arguments[0].Var();
  if (index_var->domain.HasOneValue()) {
    // Rule 2.
    // Arrays are 1 based.
    const int64 position = index_var->domain.Min() - 1;
    IntegerVariable* const expr = ct->arguments[1].variables[position];
    // Index is fixed, rewrite constraint into an equality.
    log->append("simplify element as one index is constant");
    ct->type = "int_eq";
    ct->arguments[0].variables[0] = expr;
    ct->RemoveArg(1);
    return true;
  } else if (ContainsKey(affine_map_, index_var)) {
    const AffineMapping& mapping = affine_map_[index_var];
    const Domain& domain = mapping.variable->domain;
    if ((domain.is_interval && domain.values.empty()) ||
        domain.values[0] != 1 || mapping.offset + mapping.coefficient <= 0) {
      // Invalid case. Ignore it.
      return false;
    }
    const std::vector<IntegerVariable*>& vars = ct->arguments[1].variables;
    std::vector<IntegerVariable*> new_vars;
    for (int64 i = domain.values.front(); i <= domain.values.back(); ++i) {
      const int64 index = i * mapping.coefficient + mapping.offset - 1;
      if (index < 0) {
        return false;
      }
      if (index >= vars.size()) {
        break;
      }
      new_vars.push_back(vars[index]);
    }
    // Rewrite constraint.
    log->append("simplify constraint");
    ct->arguments[0].variables[0] = mapping.variable;
    // TODO(user): Encapsulate argument setters.
    ct->arguments[1].variables.swap(new_vars);
    // Reset propagate flag.
    ct->presolve_propagation_done = false;
    // Mark old index var and affine constraint as presolved out.
    mapping.constraint->MarkAsInactive();
    index_var->active = false;
    return true;
  }
  if (index_var->domain.is_interval && index_var->domain.values.size() == 2 &&
      index_var->domain.Max() < ct->arguments[1].variables.size()) {
    // Reduce array of variables.
    ct->arguments[1].variables.resize(index_var->domain.Max());
    ct->presolve_propagation_done = false;
    log->append("reduce array");
    return true;
  }
  return false;
}

// Propagate reified comparison: int_eq_reif, int_ge_reif, int_le_reif:
//
// Rule1:
// Input : int_xx_reif(x, x, b) or bool_eq_reif(b1, b1, b)
// Action: Set b to true if xx in {le, ge, eq}, or false otherwise.
// Output: inactive constraint.
//
// Rule 2:
// Input: int_eq_reif(b1, c, b0) or bool_eq_reif(b1, c, b0)
//        or int_eq_reif(c, b1, b0) or bool_eq_reif(c, b1, b0)
// Outout: bool_eq(b1, b0) or bool_not(b1, b0) depending on the parity.
//
// Rule 3:
// Input : int_xx_reif(x, c, b) or bool_xx_reif(b1, t, b) or
//         int_xx_reif(c, x, b) or bool_xx_reif(t, b2, b)
// Action: Assign b to true or false if this can be decided from the of x and
//         c, or the comparison of b1/b2 with t.
// Output: inactive constraint of b was assigned a value.
bool Presolver::PropagateReifiedComparisons(Constraint* ct, std::string* log) {
  const std::string& id = ct->type;
  if (ct->arguments[0].type == Argument::INT_VAR_REF &&
      ct->arguments[1].type == Argument::INT_VAR_REF &&
      ct->arguments[0].variables[0] == ct->arguments[1].variables[0]) {
    // Rule 1.
    const bool value =
        (id == "int_eq_reif" || id == "int_ge_reif" || id == "int_le_reif" ||
         id == "bool_eq_reif" || id == "bool_ge_reif" || id == "bool_le_reif");
    if ((ct->arguments[2].HasOneValue() &&
         ct->arguments[2].Value() == static_cast<int64>(value)) ||
        !ct->arguments[2].HasOneValue()) {
      log->append("propagate boolvar to value");
      CHECK_EQ(Argument::INT_VAR_REF, ct->arguments[2].type);
      ct->arguments[2].variables[0]->domain.IntersectWithInterval(value, value);
      ct->MarkAsInactive();
      return true;
    }
  }
  IntegerVariable* var = nullptr;
  int64 value = 0;
  bool reverse = false;
  if (ct->arguments[0].type == Argument::INT_VAR_REF &&
      ct->arguments[1].HasOneValue()) {
    var = ct->arguments[0].Var();
    value = ct->arguments[1].Value();
  } else if (ct->arguments[1].type == Argument::INT_VAR_REF &&
             ct->arguments[0].HasOneValue()) {
    var = ct->arguments[1].Var();
    value = ct->arguments[0].Value();
    reverse = true;
  }
  if (var != nullptr) {
    if (Has01Values(var) && (id == "int_eq_reif" || id == "int_ne_reif" ||
                             id == "bool_eq_reif" || id == "bool_ne_reif") &&
        (value == 0 || value == 1)) {
      // Rule 2.
      bool parity = (id == "int_eq_reif" || id == "bool_eq_reif");
      if (value == 0) {
        parity = !parity;
      }
      log->append("simplify constraint");
      Argument target = ct->arguments[2];
      ct->arguments.clear();
      ct->arguments.push_back(Argument::IntVarRef(var));
      ct->arguments.push_back(target);
      ct->type = parity ? "bool_eq" : "bool_not";
    } else {
      // Rule 3.
      int state = 2;  // 0 force_false, 1 force true, 2 unknown.
      if (id == "int_eq_reif" || id == "bool_eq_reif") {
        if (var->domain.Contains(value)) {
          if (var->domain.HasOneValue()) {
            state = 1;
          }
        } else {
          state = 0;
        }
      } else if (id == "int_ne_reif" || id == "bool_ne_reif") {
        if (var->domain.Contains(value)) {
          if (var->domain.HasOneValue()) {
            state = 0;
          }
        } else {
          state = 1;
        }
      } else if ((((id == "int_lt_reif" || id == "bool_lt_reif") && reverse) ||
                  ((id == "int_gt_reif" || id == "bool_gt_reif") &&
                   !reverse)) &&
                 !var->domain.IsAllInt64()) {  // int_gt
        if (var->domain.Min() > value) {
          state = 1;
        } else if (var->domain.Max() <= value) {
          state = 0;
        }
      } else if ((((id == "int_lt_reif" || id == "bool_lt_reif") && !reverse) ||
                  ((id == "int_gt_reif" || id == "bool_gt_reif") && reverse)) &&
                 !var->domain.IsAllInt64()) {  // int_lt
        if (var->domain.Max() < value) {
          state = 1;
        } else if (var->domain.Min() >= value) {
          state = 0;
        }
      } else if ((((id == "int_le_reif" || id == "bool_le_reif") && reverse) ||
                  ((id == "int_ge_reif" || id == "bool_ge_reif") &&
                   !reverse)) &&
                 !var->domain.IsAllInt64()) {  // int_ge
        if (var->domain.Min() >= value) {
          state = 1;
        } else if (var->domain.Max() < value) {
          state = 0;
        }
      } else if ((((id == "int_le_reif" || id == "bool_le_reif") && !reverse) ||
                  ((id == "int_ge_reif" || id == "bool_ge_reif") && reverse)) &&
                 !var->domain.IsAllInt64()) {  // int_le
        if (var->domain.Max() <= value) {
          state = 1;
        } else if (var->domain.Min() > value) {
          state = 0;
        }
      }
      if (state != 2) {
        StringAppendF(log, "assign boolvar to %s",
                      state == 0 ? "false" : "true");
        ct->arguments[2].Var()->domain.IntersectWithInterval(state, state);
        ct->MarkAsInactive();
        return true;
      }
    }
  }
  return false;
}

// Stores the existence of int_eq_reif(x, y, b)
bool Presolver::StoreIntEqReif(Constraint* ct, std::string* log) {
  if (ct->arguments[0].type == Argument::INT_VAR_REF &&
      ct->arguments[1].type == Argument::INT_VAR_REF &&
      ct->arguments[2].type == Argument::INT_VAR_REF) {
    IntegerVariable* const first = ct->arguments[0].Var();
    IntegerVariable* const second = ct->arguments[1].Var();
    IntegerVariable* const boolvar = ct->arguments[2].Var();
    if (ContainsKey(int_eq_reif_map_, first) &&
        ContainsKey(int_eq_reif_map_[first], second)) {
      return false;
    }
    log->append("store eq_var info");
    int_eq_reif_map_[first][second] = boolvar;
    int_eq_reif_map_[second][first] = boolvar;
    return true;
  }
  return false;
}

// Merge symmetrical int_eq_reif and int_ne_reif
// Input: int_eq_reif(x, y, b1) && int_ne_reif(x, y, b2)
// Output: int_eq_reif(x, y, b1) && bool_not(b1, b2)
bool Presolver::SimplifyIntNeReif(Constraint* ct, std::string* log) {
  if (ct->arguments[0].type == Argument::INT_VAR_REF &&
      ct->arguments[1].type == Argument::INT_VAR_REF &&
      ct->arguments[2].type == Argument::INT_VAR_REF &&
      ContainsKey(int_eq_reif_map_, ct->arguments[0].Var()) &&
      ContainsKey(int_eq_reif_map_[ct->arguments[0].Var()],
                  ct->arguments[1].Var())) {
    log->append("merge constraint with opposite constraint");
    IntegerVariable* const opposite =
        int_eq_reif_map_[ct->arguments[0].Var()][ct->arguments[1].Var()];
    ct->arguments[0].variables[0] = opposite;
    ct->arguments[1].variables[0] = ct->arguments[2].Var();
    ct->RemoveArg(2);
    ct->type = "bool_not";
    return true;
  }
  return false;
}

// Remove abs from int_le_reif.
// Input : int_le_reif(x, 0, b) or int_le_reif(x,c, b) with x == abs(y)
// Output: int_eq_reif(y, 0, b) or set_in_reif(y, [-c, c], b)
bool Presolver::RemoveAbsFromIntLeReif(Constraint* ct, std::string* log) {
  if (ct->arguments[1].HasOneValue() &&
      ContainsKey(abs_map_, ct->arguments[0].Var())) {
    log->append("remove abs from constraint");
    ct->arguments[0].variables[0] = abs_map_[ct->arguments[0].Var()];
    const int64 value = ct->arguments[1].Value();
    if (value == 0) {
      ct->type = "int_eq_reif";
      return true;
    } else {
      ct->type = "set_in_reif";
      ct->arguments[1].type = Argument::INT_INTERVAL;
      ct->arguments[1].values[0] = -value;
      ct->arguments[1].values.push_back(value);
      // set_in_reif does not implement reification.
      ct->RemoveTargetVariable();
      return true;
    }
  }
  return false;
}

// Propagate bool_xor
// Rule 1:
// Input : bool_xor(t, b1, b2)
// Action: bool_not(b1, b2) if t = true, bool_eq(b1, b2) if t = false.
//
// Rule 2:
// Input : bool_xor(b1, t, b2)
// Action: bool_not(b1, b2) if t = true, bool_eq(b1, b2) if t = false.
//
// Rule 3:
// Input : bool_xor(b1, b2, t)
// Action: bool_not(b1, b2) if t = true, bool_eq(b1, b2) if t = false.
bool Presolver::PresolveBoolXor(Constraint* ct, std::string* log) {
  if (ct->arguments[0].HasOneValue()) {
    // Rule 1.
    const int64 value = ct->arguments[0].Value();
    log->append("simplify constraint");
    ct->RemoveArg(0);
    ct->type = value == 1 ? "bool_not" : "bool_eq";
    FZVLOG << "   -> " << ct->DebugString() << FZENDL;
    return true;
  }
  if (ct->arguments[1].HasOneValue()) {
    // Rule 2.
    const int64 value = ct->arguments[1].Value();
    log->append("simplify constraint");
    ct->RemoveArg(1);
    ct->type = value == 1 ? "bool_not" : "bool_eq";
    FZVLOG << "   -> " << ct->DebugString() << FZENDL;
    return true;
  }
  if (ct->arguments[2].HasOneValue()) {
    // Rule 3.
    const int64 value = ct->arguments[2].Value();
    log->append("simplify constraint");
    ct->RemoveArg(2);
    ct->type = value == 1 ? "bool_not" : "bool_eq";
    FZVLOG << "   -> " << ct->DebugString() << FZENDL;
    return true;
  }
  return false;
}

// Propagates bool_not
//
// Rule 1:
// Input : bool_not(t, b)
// Action: assign not(t) to b
// Output: inactive constraint.
//
// Rule 2:
// Input : bool_not(b, t)
// Action: assign not(t) to b
// Output: inactive constraint.
//
// Rule 3:
// Input : bool_not(b1, b2)
// Output: bool_not(b1, b2) => b1 if b1 is not already a target variable.
//
// Rule 4:
// Input : bool_not(b1, b2)
// Output: bool_not(b1, b2) => b2 if b2 is not already a target variable.
bool Presolver::PresolveBoolNot(Constraint* ct, std::string* log) {
  if (ct->arguments[0].HasOneValue() && ct->arguments[1].IsVariable()) {
    const int64 value = ct->arguments[0].Value() == 0;
    log->append("propagate constants");
    ct->arguments[1].Var()->domain.IntersectWithInterval(value, value);
    ct->MarkAsInactive();
    return true;
  } else if (ct->arguments[1].HasOneValue() && ct->arguments[0].IsVariable()) {
    const int64 value = ct->arguments[1].Value() == 0;
    log->append("propagate constants");
    ct->arguments[0].Var()->domain.IntersectWithInterval(value, value);
    ct->MarkAsInactive();
    return true;
  } else if (ct->target_variable == nullptr &&
             ct->arguments[0].Var()->defining_constraint == nullptr &&
             !ct->arguments[0].Var()->domain.HasOneValue()) {
    log->append("set target variable");
    IntegerVariable* const var = ct->arguments[0].Var();
    ct->target_variable = var;
    var->defining_constraint = ct;
    return true;
  } else if (ct->target_variable == nullptr &&
             ct->arguments[1].Var()->defining_constraint == nullptr &&
             !ct->arguments[1].Var()->domain.HasOneValue()) {
    log->append("set target variable");
    IntegerVariable* const var = ct->arguments[1].Var();
    ct->target_variable = var;
    var->defining_constraint = ct;
    return true;
  }
  return false;
}

// Simplify bool_clause
//
// Rule 1:
// Input: bool_clause([b1][b2])
// Output: bool_le(b2, b1)
//
// Rule 2:
// Input: bool_clause([t][b])
// Output: Mark constraint as inactive if t is true.
//         bool_eq(b, false) if t is false.
//
// Rule 3:
// Input: bool_clause([b1, .., bn][t])
// Output: Mark constraint as inactive if t is false.
//         array_array_or([b1, .. ,bn]) if t is true.
bool Presolver::PresolveBoolClause(Constraint* ct, std::string* log) {
  // Rule 1.
  if (ct->arguments[0].variables.size() == 1 &&
      ct->arguments[1].variables.size() == 1) {
    log->append("simplify constraint");
    std::swap(ct->arguments[0].variables[0], ct->arguments[1].variables[0]);
    ct->arguments[0].type = Argument::INT_VAR_REF;
    ct->arguments[1].type = Argument::INT_VAR_REF;
    ct->type = "bool_le";
    FZVLOG << "  to " << ct->DebugString() << FZENDL;
    return true;
  }
  // Rule 2.
  if (ct->arguments[0].variables.size() == 0 &&
      ct->arguments[0].values.size() == 1 &&
      ct->arguments[1].variables.size() == 1) {
    log->append("simplify constraint");
    const int64 value = ct->arguments[0].values.front();
    if (value) {
      ct->MarkAsInactive();
      return true;
    } else {
      ct->arguments[0].type = Argument::INT_VAR_REF;
      ct->arguments[0].variables = ct->arguments[1].variables;
      ct->arguments[0].values.clear();
      ct->arguments[1].type = Argument::INT_VALUE;
      ct->arguments[1].variables.clear();
      ct->arguments[1].values.push_back(0);
      ct->type = "bool_eq";
      FZVLOG << "  to " << ct->DebugString() << FZENDL;
      return true;
    }
  }
  // Rule 3.
  if (ct->arguments[1].variables.size() == 0 &&
      ct->arguments[1].values.size() == 1) {
    log->append("simplify constraint");
    const int64 value = ct->arguments[1].values.front();
    if (value) {
      if (ct->arguments[0].variables.size() > 1) {
        ct->type = "array_bool_or";
        FZVLOG << "  to " << ct->DebugString() << FZENDL;
        return true;
      } else if (ct->arguments[0].variables.size() == 1) {
        ct->arguments[0].type = Argument::INT_VAR_REF;
        ct->arguments[1].type = Argument::INT_VALUE;
        ct->type = "bool_eq";
        FZVLOG << "  to " << ct->DebugString() << FZENDL;
        return true;
      }
    } else {
      ct->MarkAsInactive();
      return true;
    }
  }
  return false;
}

// Simplify boolean formula: int_lin_eq
//
// Rule 1:
// Input : int_lin_eq_reif([1, 1], [b1, b2], 1, b0)
// Output: bool_ne_reif(b1, b2, b0)
//
// Rule 2:
// Input : int_lin_eq_reif([1, 1], [false, b2], 1, b0)
// Output: bool_eq(b2, b0)
//
// Rule 3:
// Input : int_lin_eq_reif([1, 1], [true, b2], 1, b0)
// Output: bool_not(b2, b0)
//
// Rule 4:
// Input : int_lin_eq_reif([1, 1], [b1, false], 1, b0)
// Output: bool_eq(b1, b0)
//
// Rule 5:
// Input : int_lin_eq_reif([1, 1], [b1, true], 1, b0)
// Output: bool_not(b1, b0)
bool Presolver::SimplifyIntLinEqReif(Constraint* ct, std::string* log) {
  if (ct->arguments[0].values.size() == 2 && ct->arguments[0].values[0] == 1 &&
      ct->arguments[0].values[1] == 1 && ct->arguments[2].Value() == 1) {
    IntegerVariable* const left = ct->arguments[1].variables[0];
    IntegerVariable* const right = ct->arguments[1].variables[1];
    IntegerVariable* const target = ct->arguments[3].Var();
    if (Has01Values(ct->arguments[1].variables[0]) &&
        Has01Values(ct->arguments[1].variables[1])) {
      // Rule 1.
      log->append("rewrite constraint to bool_ne_reif");
      ct->type = "bool_ne_reif";
      ct->arguments[0].type = Argument::INT_VAR_REF;
      ct->arguments[0].values.clear();
      ct->arguments[0].variables.push_back(left);
      ct->arguments[1].type = Argument::INT_VAR_REF;
      ct->arguments[1].variables.clear();
      ct->arguments[1].variables.push_back(right);
      ct->arguments[2].type = Argument::INT_VAR_REF;
      ct->arguments[2].values.clear();
      ct->arguments[2].variables.push_back(target);
      ct->RemoveArg(3);
      FZVLOG << " -> " << ct->DebugString() << FZENDL;
      return true;
    }
    if (Has01Values(right) && left->domain.HasOneValue() &&
        Is0Or1(left->domain.Min())) {
      if (left->domain.Min() == 0) {
        // Rule 2.
        log->append("rewrite constraint to bool_eq");
        ct->type = "bool_eq";
        ct->arguments[0].type = Argument::INT_VAR_REF;
        ct->arguments[0].values.clear();
        ct->arguments[0].variables.push_back(right);
        ct->arguments[1].type = Argument::INT_VAR_REF;
        ct->arguments[1].variables.clear();
        ct->arguments[1].variables.push_back(target);
        ct->RemoveArg(3);
        ct->RemoveArg(2);
        FZVLOG << " -> " << ct->DebugString() << FZENDL;
        return true;
      } else {
        // Rule 3.
        log->append("rewrite constraint to bool_not");
        ct->type = "bool_not";
        ct->arguments[0].type = Argument::INT_VAR_REF;
        ct->arguments[0].values.clear();
        ct->arguments[0].variables.push_back(right);
        ct->arguments[1].type = Argument::INT_VAR_REF;
        ct->arguments[1].variables.clear();
        ct->arguments[1].variables.push_back(target);
        ct->RemoveArg(3);
        ct->RemoveArg(2);
        FZVLOG << " -> " << ct->DebugString() << FZENDL;
        return true;
      }
    } else if (Has01Values(left) && right->domain.HasOneValue() &&
               Is0Or1(right->domain.Min())) {
      if (right->domain.Min() == 0) {
        // Rule 4.
        log->append("rewrite constraint to bool_eq");
        ct->type = "bool_eq";
        ct->arguments[0].type = Argument::INT_VAR_REF;
        ct->arguments[0].values.clear();
        ct->arguments[0].variables.push_back(left);
        ct->arguments[1].type = Argument::INT_VAR_REF;
        ct->arguments[1].variables.clear();
        ct->arguments[1].variables.push_back(target);
        ct->RemoveArg(3);
        ct->RemoveArg(2);
        FZVLOG << " -> " << ct->DebugString() << FZENDL;
        return true;
      } else {
        // Rule 5.
        log->append("rewrite constraint to bool_not");
        ct->type = "bool_not";
        ct->arguments[0].type = Argument::INT_VAR_REF;
        ct->arguments[0].values.clear();
        ct->arguments[0].variables.push_back(left);
        ct->arguments[1].type = Argument::INT_VAR_REF;
        ct->arguments[1].variables.clear();
        ct->arguments[1].variables.push_back(target);
        ct->RemoveArg(3);
        ct->RemoveArg(2);
        FZVLOG << " -> " << ct->DebugString() << FZENDL;
        return true;
      }
    }
  }
  return false;
}

// Remove target variable from int_mod if bound.
//
// Input : int_mod(x1, x2, x3)  => x3
// Output: int_mod(x1, x2, x3) if x3 has only one value.
bool Presolver::PresolveIntMod(Constraint* ct, std::string* log) {
  if (ct->target_variable != nullptr &&
      ct->arguments[2].Var() == ct->target_variable &&
      ct->arguments[2].HasOneValue()) {
    ct->target_variable->defining_constraint = nullptr;
    ct->target_variable = nullptr;
    return true;
  }
  return false;
}

#define CALL_TYPE(ct, t, method, changed)                                   \
  if (ct->active && ct->type == t) {                                        \
    changed |= ApplyRule(ct, #method, [this](Constraint* ct, std::string* log) { \
      return method(ct, log);                                               \
    });                                                                     \
  }
#define CALL_PREFIX(ct, t, method, changed)                                 \
  if (ct->active && ::operations_research::HasPrefixString(ct->type, t)) { \
    changed |= ApplyRule(ct, #method, [this](Constraint* ct, std::string* log) { \
      return method(ct, log);                                               \
    });                                                                     \
  }
#define CALL_SUFFIX(ct, t, method, changed)                                  \
  if (ct->active && ::operations_research::HasSuffixString(ct->type, t)) { \
    changed |= ApplyRule(ct, "method", [this](Constraint* ct, std::string* log) { \
      return method(ct, log);                                                \
    });                                                                      \
  }

// Main presolve rule caller.
bool Presolver::PresolveOneConstraint(Constraint* ct) {
  bool changed = false;
  CALL_SUFFIX(ct, "_reif", Unreify, changed);
  CALL_TYPE(ct, "bool2int", PresolveBool2Int, changed);
  CALL_TYPE(ct, "int_le", PresolveInequalities, changed);
  CALL_TYPE(ct, "int_lt", PresolveInequalities, changed);
  CALL_TYPE(ct, "int_ge", PresolveInequalities, changed);
  CALL_TYPE(ct, "int_gt", PresolveInequalities, changed);
  CALL_TYPE(ct, "bool_le", PresolveInequalities, changed);
  CALL_TYPE(ct, "bool_lt", PresolveInequalities, changed);
  CALL_TYPE(ct, "bool_ge", PresolveInequalities, changed);
  CALL_TYPE(ct, "bool_gt", PresolveInequalities, changed);

  // TODO(user): use CALL_TYPE macro.
  if (ct->type == "int_abs" && !ContainsKey(abs_map_, ct->arguments[1].Var())) {
    // Stores abs() map.
    FZVLOG << "Stores abs map for " << ct->DebugString() << FZENDL;
    abs_map_[ct->arguments[1].Var()] = ct->arguments[0].Var();
    changed = true;
  }
  CALL_TYPE(ct, "int_eq_reif", StoreIntEqReif, changed);
  CALL_TYPE(ct, "int_ne_reif", SimplifyIntNeReif, changed);
  // TODO(user): use CALL_TYPE macro.
  if ((ct->type == "int_eq_reif" || ct->type == "int_ne_reif" ||
       ct->type == "int_ne") &&
      ct->arguments[1].HasOneValue() && ct->arguments[1].Value() == 0 &&
      ContainsKey(abs_map_, ct->arguments[0].Var())) {
    // Simplifies int_eq and int_ne with abs
    // Input : int_eq(x, 0) or int_ne(x, 0) with x == abs(y)
    // Output: int_eq(y, 0) or int_ne(y, 0)
    FZVLOG << "Remove abs() from " << ct->DebugString() << FZENDL;
    ct->arguments[0].variables[0] = abs_map_[ct->arguments[0].Var()];
    changed = true;
  }
  CALL_TYPE(ct, "int_le_reif", RemoveAbsFromIntLeReif, changed);
  CALL_TYPE(ct, "int_eq", PresolveIntEq, changed);
  CALL_TYPE(ct, "bool_eq", PresolveIntEq, changed);
  CALL_TYPE(ct, "int_ne", PresolveIntNe, changed);
  CALL_TYPE(ct, "bool_not", PresolveIntNe, changed);
  CALL_TYPE(ct, "set_in", PresolveSetIn, changed);
  CALL_TYPE(ct, "array_bool_and", PresolveArrayBoolAnd, changed);
  CALL_TYPE(ct, "array_bool_or", PresolveArrayBoolOr, changed);
  CALL_TYPE(ct, "bool_eq_reif", PresolveBoolEqNeReif, changed);
  CALL_TYPE(ct, "bool_ne_reif", PresolveBoolEqNeReif, changed);
  CALL_TYPE(ct, "bool_xor", PresolveBoolXor, changed);
  CALL_TYPE(ct, "bool_not", PresolveBoolNot, changed);
  CALL_TYPE(ct, "bool_clause", PresolveBoolClause, changed);
  CALL_TYPE(ct, "int_div", PresolveIntDiv, changed);
  CALL_TYPE(ct, "int_times", PresolveIntTimes, changed);
  CALL_TYPE(ct, "int_lin_gt", PresolveIntLinGt, changed);
  CALL_TYPE(ct, "int_lin_lt", PresolveIntLinLt, changed);
  CALL_PREFIX(ct, "int_lin_", PresolveLinear, changed);
  CALL_PREFIX(ct, "int_lin_", RegroupLinear, changed);
  CALL_PREFIX(ct, "int_lin_", SimplifyUnaryLinear, changed);
  CALL_PREFIX(ct, "int_lin_", SimplifyBinaryLinear, changed);
  CALL_TYPE(ct, "int_lin_eq", PropagatePositiveLinear, changed);
  CALL_TYPE(ct, "int_lin_le", PropagatePositiveLinear, changed);
  CALL_TYPE(ct, "int_lin_ge", PropagatePositiveLinear, changed);
  CALL_TYPE(ct, "int_lin_eq", CreateLinearTarget, changed);
  CALL_TYPE(ct, "int_lin_eq", PresolveStoreMapping, changed);
  CALL_TYPE(ct, "int_lin_eq_reif", CheckIntLinReifBounds, changed);
  CALL_TYPE(ct, "int_lin_eq_reif", SimplifyIntLinEqReif, changed);
  CALL_TYPE(ct, "array_int_element", PresolveSimplifyElement, changed);
  CALL_TYPE(ct, "array_int_element", PresolveArrayIntElement, changed);
  CALL_TYPE(ct, "array_var_int_element", PresolveSimplifyExprElement, changed);
  CALL_TYPE(ct, "int_eq_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "int_ne_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "int_le_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "int_lt_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "int_ge_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "int_gt_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "bool_eq_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "bool_ne_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "bool_le_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "bool_lt_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "bool_ge_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "bool_gt_reif", PropagateReifiedComparisons, changed);
  CALL_TYPE(ct, "int_mod", PresolveIntMod, changed);
  // Last rule: if the target variable of a constraint is fixed, removed it
  // the target part.
  if (ct->target_variable != nullptr &&
      ct->target_variable->domain.HasOneValue()) {
    FZVLOG << "Remove target variable from " << ct->DebugString()
           << " as it is fixed to a single value" << FZENDL;
    ct->target_variable->defining_constraint = nullptr;
    ct->target_variable = nullptr;
    changed = true;
  }
  return changed;
}

#undef CALL_TYPE
#undef CALL_PREFIX
#undef CALL_SUFFIX

// Stores all pairs of variables appearing in an int_ne(x, y) constraint.
void Presolver::StoreDifference(Constraint* ct) {
  if (ct->arguments[2].Value() == 0 && ct->arguments[0].values.size() == 3) {
    // Looking for a difference var.
    if ((ct->arguments[0].values[0] == 1 && ct->arguments[0].values[1] == -1 &&
         ct->arguments[0].values[2] == 1) ||
        (ct->arguments[0].values[0] == -1 && ct->arguments[0].values[1] == 1 &&
         ct->arguments[0].values[2] == -1)) {
      FZVLOG << "Store differences from " << ct->DebugString() << FZENDL;
      difference_map_[ct->arguments[1].variables[0]] = std::make_pair(
          ct->arguments[1].variables[2], ct->arguments[1].variables[1]);
      difference_map_[ct->arguments[1].variables[2]] = std::make_pair(
          ct->arguments[1].variables[0], ct->arguments[1].variables[1]);
    }
  }
}

void Presolver::MergeIntEqNe(Model* model) {
  hash_map<const IntegerVariable*, hash_map<int64, IntegerVariable*>>
      int_eq_reif_map;
  hash_map<const IntegerVariable*, hash_map<int64, IntegerVariable*>>
      int_ne_reif_map;
  for (Constraint* const ct : model->constraints()) {
    if (!ct->active) continue;
    if (ct->type == "int_eq_reif" && ct->arguments[2].values.empty()) {
      IntegerVariable* var = nullptr;
      int64 value = 0;
      if (ct->arguments[0].values.empty() &&
          ct->arguments[1].variables.empty()) {
        var = ct->arguments[0].Var();
        value = ct->arguments[1].Value();
      } else if (ct->arguments[1].values.empty() &&
                 ct->arguments[0].variables.empty()) {
        var = ct->arguments[1].Var();
        value = ct->arguments[0].Value();
      }
      if (var != nullptr) {
        IntegerVariable* boolvar = ct->arguments[2].Var();
        IntegerVariable* stored = FindPtrOrNull(int_eq_reif_map[var], value);
        if (stored == nullptr) {
          FZVLOG << "Store " << ct->DebugString() << FZENDL;
          int_eq_reif_map[var][value] = boolvar;
        } else {
          FZVLOG << "Merge " << ct->DebugString() << FZENDL;
          ct->MarkAsInactive();
          AddVariableSubstition(stored, boolvar);
        }
      }
    }

    if (ct->type == "int_ne_reif" && ct->arguments[2].values.empty()) {
      IntegerVariable* var = nullptr;
      int64 value = 0;
      if (ct->arguments[0].values.empty() &&
          ct->arguments[1].variables.empty()) {
        var = ct->arguments[0].Var();
        value = ct->arguments[1].Value();
      } else if (ct->arguments[1].values.empty() &&
                 ct->arguments[0].variables.empty()) {
        var = ct->arguments[1].Var();
        value = ct->arguments[0].Value();
      }
      if (var != nullptr) {
        IntegerVariable* boolvar = ct->arguments[2].Var();
        IntegerVariable* stored = FindPtrOrNull(int_ne_reif_map[var], value);
        if (stored == nullptr) {
          FZVLOG << "Store " << ct->DebugString() << FZENDL;
          int_ne_reif_map[var][value] = boolvar;
        } else {
          FZVLOG << "Merge " << ct->DebugString() << FZENDL;
          ct->MarkAsInactive();
          AddVariableSubstition(stored, boolvar);
        }
      }
    }
  }
}

void Presolver::FirstPassModelScan(Model* model) {
  for (Constraint* const ct : model->constraints()) {
    if (!ct->active) continue;
    if (ct->type == "int_lin_eq") {
      StoreDifference(ct);
    }
  }

  // Collect decision variables.
  std::vector<IntegerVariable*> vars;
  for (const Annotation& ann : model->search_annotations()) {
    ann.AppendAllIntegerVariables(&vars);
  }
  decision_variables_.insert(vars.begin(), vars.end());
}

bool Presolver::Run(Model* model) {
  // Rebuild var_constraint map if empty.
  if (var_to_constraints_.empty()) {
    for (Constraint* const ct : model->constraints()) {
      for (const Argument& arg : ct->arguments) {
        for (IntegerVariable* const var : arg.variables) {
          var_to_constraints_[var].insert(ct);
        }
      }
    }
  }

  FirstPassModelScan(model);

  MergeIntEqNe(model);
  if (!var_representative_map_.empty()) {
    // Some new substitutions were introduced. Let's process them.
    SubstituteEverywhere(model);
    var_representative_map_.clear();
  }

  bool changed_since_start = false;
  // Let's presolve the bool2int predicates first.
  for (Constraint* const ct : model->constraints()) {
    if (ct->active && ct->type == "bool2int") {
      std::string log;
      changed_since_start |= PresolveBool2Int(ct, &log);
    }
  }
  if (!var_representative_map_.empty()) {
    // Some new substitutions were introduced. Let's process them.
    SubstituteEverywhere(model);
    var_representative_map_.clear();
  }

  // Apply the rest of the presolve rules.
  for (;;) {
    bool changed = false;
    var_representative_map_.clear();
    for (Constraint* const ct : model->constraints()) {
      if (ct->active) {
        changed |= PresolveOneConstraint(ct);
      }
      if (!var_representative_map_.empty()) {
        break;
      }
    }
    if (!var_representative_map_.empty()) {
      // Some new substitutions were introduced. Let's process them.
      DCHECK(changed);
      changed = true;  // To be safe in opt mode.
      SubstituteEverywhere(model);
      var_representative_map_.clear();
    }
    changed_since_start |= changed;
    if (!changed) break;
  }
  return changed_since_start;
}

// ----- Substitution support -----

void Presolver::AddVariableSubstition(IntegerVariable* from,
                                      IntegerVariable* to) {
  CHECK(from != nullptr);
  CHECK(to != nullptr);
  // Apply the substitutions, if any.
  from = FindRepresentativeOfVar(from);
  to = FindRepresentativeOfVar(to);
  if (to->temporary) {
    // Let's switch to keep a non temporary as representative.
    IntegerVariable* tmp = to;
    to = from;
    from = tmp;
  }
  if (from != to) {
    FZVLOG << "Mark " << from->DebugString() << " as equivalent to "
           << to->DebugString() << FZENDL;
    if (from->defining_constraint != nullptr &&
        to->defining_constraint != nullptr) {
      FZVLOG << "  - break target_variable on "
             << from->defining_constraint->DebugString() << FZENDL;
      from->defining_constraint->RemoveTargetVariable();
    }
    CHECK(to->Merge(from->name, from->domain, from->defining_constraint,
                    from->temporary));
    from->active = false;
    var_representative_map_[from] = to;
  }
}

IntegerVariable* Presolver::FindRepresentativeOfVar(IntegerVariable* var) {
  if (var == nullptr) return nullptr;
  IntegerVariable* start_var = var;
  // First loop: find the top parent.
  for (;;) {
    IntegerVariable* parent =
        FindWithDefault(var_representative_map_, var, var);
    if (parent == var) break;
    var = parent;
  }
  // Second loop: attach all the path to the top parent.
  while (start_var != var) {
    IntegerVariable* const parent = var_representative_map_[start_var];
    var_representative_map_[start_var] = var;
    start_var = parent;
  }
  return FindWithDefault(var_representative_map_, var, var);
}

void Presolver::SubstituteEverywhere(Model* model) {
  // Collected impacted constraints.
  hash_set<Constraint*> impacted;
  for (const auto& p : var_representative_map_) {
    const hash_set<Constraint*>& contains = var_to_constraints_[p.first];
    impacted.insert(contains.begin(), contains.end());
  }
  // Rewrite the constraints.
  for (Constraint* const ct : impacted) {
    if (ct != nullptr && ct->active) {
      for (int i = 0; i < ct->arguments.size(); ++i) {
        Argument* argument = &ct->arguments[i];
        switch (argument->type) {
          case Argument::INT_VAR_REF:
          case Argument::INT_VAR_REF_ARRAY: {
            for (int i = 0; i < argument->variables.size(); ++i) {
              IntegerVariable* const old_var = argument->variables[i];
              IntegerVariable* const new_var = FindRepresentativeOfVar(old_var);
              if (new_var != old_var) {
                argument->variables[i] = new_var;
                var_to_constraints_[new_var].insert(ct);
              }
            }
            break;
          }
          default: {}
        }
      }
      // No need to update var_to_constraints, it should have been done already
      // in the arguments of the constraints.
      ct->target_variable = FindRepresentativeOfVar(ct->target_variable);
    }
  }
  // Rewrite the search.
  for (Annotation* const ann : model->mutable_search_annotations()) {
    SubstituteAnnotation(ann);
  }
  // Rewrite the output.
  for (OnSolutionOutput* const output : model->mutable_output()) {
    output->variable = FindRepresentativeOfVar(output->variable);
    for (int i = 0; i < output->flat_variables.size(); ++i) {
      output->flat_variables[i] =
          FindRepresentativeOfVar(output->flat_variables[i]);
    }
  }
  // Do not forget to merge domain that could have evolved asynchronously
  // during presolve.
  for (const auto& iter : var_representative_map_) {
    iter.second->domain.IntersectWithDomain(iter.first->domain);
  }
}

void Presolver::SubstituteAnnotation(Annotation* ann) {
  // TODO(user): Remove recursion.
  switch (ann->type) {
    case Annotation::ANNOTATION_LIST:
    case Annotation::FUNCTION_CALL: {
      for (int i = 0; i < ann->annotations.size(); ++i) {
        SubstituteAnnotation(&ann->annotations[i]);
      }
      break;
    }
    case Annotation::INT_VAR_REF:
    case Annotation::INT_VAR_REF_ARRAY: {
      for (int i = 0; i < ann->variables.size(); ++i) {
        ann->variables[i] = FindRepresentativeOfVar(ann->variables[i]);
      }
      break;
    }
    default: {}
  }
}

// ----- Helpers -----

void Presolver::IntersectDomainWith(const Argument& arg, Domain* domain) {
  switch (arg.type) {
    case Argument::INT_VALUE: {
      const int64 value = arg.Value();
      domain->IntersectWithInterval(value, value);
      break;
    }
    case Argument::INT_INTERVAL: {
      domain->IntersectWithInterval(arg.values[0], arg.values[1]);
      break;
    }
    case Argument::INT_LIST: {
      domain->IntersectWithListOfIntegers(arg.values);
      break;
    }
    default: { LOG(FATAL) << "Wrong domain type" << arg.DebugString(); }
  }
}

// ----- Clean up model -----

namespace {
void Regroup(Constraint* start, const std::vector<IntegerVariable*>& chain,
             const std::vector<IntegerVariable*>& carry_over) {
  // End of chain, reconstruct.
  IntegerVariable* const out = carry_over.back();
  start->arguments.pop_back();
  start->arguments[0].variables[0] = out;
  start->arguments[1].type = Argument::INT_VAR_REF_ARRAY;
  start->arguments[1].variables = chain;
  const std::string old_type = start->type;
  start->type = start->type == "int_min" ? "minimum_int" : "maximum_int";
  start->target_variable = out;
  out->defining_constraint = start;
  for (IntegerVariable* const var : carry_over) {
    if (var != carry_over.back()) {
      var->active = false;
    }
  }
  FZVLOG << "Regroup chain of " << old_type << " into " << start->DebugString()
         << FZENDL;
}

void CheckRegroupStart(Constraint* ct, Constraint** start,
                       std::vector<IntegerVariable*>* chain,
                       std::vector<IntegerVariable*>* carry_over) {
  if ((ct->type == "int_min" || ct->type == "int_max") &&
      !ct->arguments[0].variables.empty() &&
      ct->arguments[0].Var() == ct->arguments[1].Var()) {
    // This is the start of the chain.
    *start = ct;
    chain->push_back(ct->arguments[0].Var());
    carry_over->push_back(ct->arguments[2].Var());
    carry_over->back()->defining_constraint = nullptr;
  }
}

// Weight:
//  - *_reif: arity
//  - otherwise arity + 100.
int SortWeight(Constraint* ct) {
  int arity = HasSuffixString(ct->type, "_reif") ? 0 : 100;
  for (const Argument& arg : ct->arguments) {
    arity += arg.variables.size();
  }
  return arity;
}

void CleanUpVariableWithMultipleDefiningConstraints(Model* model) {
  hash_map<IntegerVariable*, std::vector<Constraint*>> ct_var_map;
  for (Constraint* const ct : model->constraints()) {
    if (ct->target_variable != nullptr) {
      ct_var_map[ct->target_variable].push_back(ct);
    }
  }

  for (auto& ct_list : ct_var_map) {
    if (ct_list.second.size() > 1) {
      // Sort by number of variables in the constraint. Prefer smaller ones.
      std::sort(ct_list.second.begin(), ct_list.second.end(),
                [](Constraint* c1, Constraint* c2) {
                  return SortWeight(c1) < SortWeight(c2);
                });
      // Keep the first constraint as the defining one.
      for (int pos = 1; pos < ct_list.second.size(); ++pos) {
        Constraint* const ct = ct_list.second[pos];
        FZVLOG << "Remove duplicate target from " << ct->DebugString()
               << FZENDL;
        ct_list.first->defining_constraint = ct;
        ct_list.second[pos]->RemoveTargetVariable();
      }
      // Reset the defining constraint.
      ct_list.first->defining_constraint = ct_list.second[0];
    }
  }
}

bool AreOnesFollowedByMinusOne(const std::vector<int64>& coeffs) {
  for (int i = 0; i < coeffs.size() - 1; ++i) {
    if (coeffs[i] != 1) {
      return false;
    }
  }
  return coeffs.back() == -1;
}

template <class T>
bool IsStrictPrefix(const std::vector<T>& v1, const std::vector<T>& v2) {
  if (v1.size() >= v2.size()) {
    return false;
  }
  for (int i = 0; i < v1.size(); ++i) {
    if (v1[i] != v2[i]) {
      return false;
    }
  }
  return true;
}
}  // namespace

void Presolver::CleanUpModelForTheCpSolver(Model* model, bool use_sat) {
  // First pass.
  for (Constraint* const ct : model->constraints()) {
    const std::string& id = ct->type;
    // Remove ignored annotations on int_lin_eq.
    if (id == "int_lin_eq" && ct->strong_propagation) {
      if (ct->arguments[0].values.size() > 3) {
        // We will use a table constraint. Remove the target variable flag.
        FZVLOG << "Remove target_variable from " << ct->DebugString() << FZENDL;
        ct->RemoveTargetVariable();
      }
    }
    if (id == "int_lin_eq" && ct->target_variable != nullptr) {
      IntegerVariable* const var = ct->target_variable;
      for (int i = 0; i < ct->arguments[0].values.size(); ++i) {
        if (ct->arguments[1].variables[i] == var) {
          if (ct->arguments[0].values[i] == -1) {
            break;
          } else if (ct->arguments[0].values[i] == 1) {
            FZVLOG << "Reverse " << ct->DebugString() << FZENDL;
            ct->arguments[2].values[0] *= -1;
            for (int j = 0; j < ct->arguments[0].values.size(); ++j) {
              ct->arguments[0].values[j] *= -1;
            }
            break;
          }
        }
      }
    }
    if (id == "array_var_int_element") {
      if (ct->target_variable != nullptr) {
        hash_set<IntegerVariable*> variables_in_array;
        for (IntegerVariable* const var : ct->arguments[1].variables) {
          variables_in_array.insert(var);
        }
        if (ContainsKey(variables_in_array, ct->target_variable)) {
          FZVLOG << "Remove target variable from " << ct->DebugString()
                 << " as it appears in the array of variables" << FZENDL;
          ct->RemoveTargetVariable();
        }
      }
    }

    // Remove target variables from constraints passed to SAT.
    if (use_sat && ct->target_variable != nullptr &&
        (id == "array_bool_and" || id == "array_bool_or" ||
         ((id == "bool_eq_reif" || id == "bool_ne_reif") &&
          !ct->arguments[1].HasOneValue()) ||
         id == "bool_le_reif" || id == "bool_ge_reif")) {
      ct->RemoveTargetVariable();
    }
    // Remove target variables from constraints that will not implement it.
    if (id == "count_reif" || id == "set_in_reif") {
      ct->RemoveTargetVariable();
    }
    // Remove target variables from element constraint.
    if ((id == "array_int_element" &&
         (!IsArrayBoolean(ct->arguments[1].values) ||
          !OnlyOne0OrOnlyOne1(ct->arguments[1].values))) ||
        id == "array_var_int_element") {
      ct->RemoveTargetVariable();
    }
  }

  // Clean up variables with multiple defining constraints.
  CleanUpVariableWithMultipleDefiningConstraints(model);

  // Second pass.
  for (Constraint* const ct : model->constraints()) {
    const std::string& id = ct->type;
    // Create new target variables with unused boolean variables.
    if (ct->target_variable == nullptr &&
        (id == "int_lin_eq_reif" || id == "int_lin_ne_reif" ||
         id == "int_lin_ge_reif" || id == "int_lin_le_reif" ||
         id == "int_lin_gt_reif" || id == "int_lin_lt_reif" ||
         id == "int_eq_reif" || id == "int_ne_reif" || id == "int_le_reif" ||
         id == "int_ge_reif" || id == "int_lt_reif" || id == "int_gt_reif")) {
      IntegerVariable* const bool_var = ct->arguments[2].Var();
      if (bool_var != nullptr && bool_var->defining_constraint == nullptr) {
        FZVLOG << "Create target_variable on " << ct->DebugString() << FZENDL;
        ct->target_variable = bool_var;
        bool_var->defining_constraint = ct;
      }
    }
  }
  // Regroup int_min and int_max into maximum_int and maximum_int.
  // The minizinc to flatzinc expander will transform x = std::max([v1, .., vn])
  // into:
  //   tmp1 = std::max(v1, v1)
  //   tmp2 = std::max(v2, tmp1)
  //   tmp3 = std::max(v3, tmp2)
  // ...
  // This code reconstructs the initial std::min(array) or std::max(array).
  Constraint* start = nullptr;
  std::vector<IntegerVariable*> chain;
  std::vector<IntegerVariable*> carry_over;
  var_to_constraints_.clear();
  for (Constraint* const ct : model->constraints()) {
    for (const Argument& arg : ct->arguments) {
      for (IntegerVariable* const var : arg.variables) {
        var_to_constraints_[var].insert(ct);
      }
    }
  }

  // First version. The start is recognized by the double var in the max.
  //   tmp1 = std::max(v1, v1)
  for (Constraint* const ct : model->constraints()) {
    if (start == nullptr) {
      CheckRegroupStart(ct, &start, &chain, &carry_over);
    } else if (ct->type == start->type &&
               ct->arguments[1].Var() == carry_over.back() &&
               var_to_constraints_[ct->arguments[0].Var()].size() <= 2) {
      chain.push_back(ct->arguments[0].Var());
      carry_over.push_back(ct->arguments[2].Var());
      ct->active = false;
      ct->target_variable = nullptr;
      carry_over.back()->defining_constraint = nullptr;
    } else {
      Regroup(start, chain, carry_over);
      // Clean
      start = nullptr;
      chain.clear();
      carry_over.clear();
      // Check again ct.
      CheckRegroupStart(ct, &start, &chain, &carry_over);
    }
  }
  // Checks left over from the loop.
  if (start != nullptr) {
    Regroup(start, chain, carry_over);
  }

  // Regroup increasing sequence of int_lin_eq([1,..,1,-1], [x1, ..., xn, yn])
  // into sequence of int_plus(x1, x2, y2), int_plus(y2, x3, y3)...
  std::vector<IntegerVariable*> current_variables;
  IntegerVariable* target_variable = nullptr;
  Constraint* first_constraint = nullptr;
  for (Constraint* const ct : model->constraints()) {
    if (target_variable == nullptr) {
      if (ct->type == "int_lin_eq" && ct->arguments[0].values.size() == 3 &&
          AreOnesFollowedByMinusOne(ct->arguments[0].values) &&
          ct->arguments[1].values.empty() && ct->arguments[2].Value() == 0) {
        FZVLOG << "Recognize assignment " << ct->DebugString() << FZENDL;
        current_variables = ct->arguments[1].variables;
        target_variable = current_variables.back();
        current_variables.pop_back();
        first_constraint = ct;
      }
    } else {
      if (ct->type == "int_lin_eq" &&
          AreOnesFollowedByMinusOne(ct->arguments[0].values) &&
          ct->arguments[0].values.size() == current_variables.size() + 2 &&
          IsStrictPrefix(current_variables, ct->arguments[1].variables)) {
        FZVLOG << "Recognize hidden int_plus " << ct->DebugString() << FZENDL;
        current_variables = ct->arguments[1].variables;
        // Rewrite ct into int_plus.
        ct->type = "int_plus";
        ct->arguments[0].type = Argument::INT_VAR_REF;
        ct->arguments[0].values.clear();
        ct->arguments[0].variables.push_back(target_variable);
        ct->arguments[1].type = Argument::INT_VAR_REF;
        ct->arguments[1].variables.clear();
        ct->arguments[1].variables.push_back(
            current_variables[current_variables.size() - 2]);
        ct->arguments[2].type = Argument::INT_VAR_REF;
        ct->arguments[2].values.clear();
        ct->arguments[2].variables.push_back(current_variables.back());
        target_variable = current_variables.back();
        current_variables.pop_back();
        // We remove the target variable to force the variable to be created
        // To break the linear sweep during propagation.
        ct->RemoveTargetVariable();
        FZVLOG << "  -> " << ct->DebugString() << FZENDL;
        // We clean the first constraint too.
        if (first_constraint != nullptr) {
          first_constraint->RemoveTargetVariable();
          first_constraint = nullptr;
        }
      } else {
        current_variables.clear();
        target_variable = nullptr;
      }
    }
  }
}
}  // namespace fz
}  // namespace operations_research
