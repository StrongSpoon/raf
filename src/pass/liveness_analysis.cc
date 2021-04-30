/*!
 * Copyright (c) 2020 by Contributors
 * \file liveness_analysis.cc
 * \brief A pass for analyzing tensor liveness.
 */
#include "liveness_analysis.h"
#include <vector>
#include "mnm/op.h"
#include "mnm/ir.h"
#include "mnm/pass.h"
#include "tvm/ir/type_functor.h"
#include "./let_list.h"
#include "./common.h"

namespace mnm {
namespace pass {

using PackedLiveInMap = Map<Var, Array<Var>>;

namespace liveness_analysis {

MapVSet LivenessAnalyzer::Run() {
  Expr body;
  FormCheck(func_->body);
  if (failure_) {
    return live_;
  }

  for (const auto& var : func_->params) {
    auto var_type = var->checked_type();
    if (var_type.as<TensorTypeNode>()) {
      Var tvar = CreateTensor("param");
      Init(var, tvar);
    } else if (const auto& tuple_type = var_type.as<TupleTypeNode>()) {
      Array<Var> fields;
      for (const auto& field : tuple_type->fields) {
        CHECK(field.as<TensorTypeNode>())
            << "Expected a tensor in tuple parameter, but got " << field->GetTypeKey();
        Var tvar = CreateTensor("param");
        fields.push_back(tvar);
      }
      Init(var, Merge(fields));
      vtuple_.Set(var, fields);
    } else {
      LOG(FATAL) << "NotImplementedError: Unsupported parameter type: " << var->GetTypeKey();
    }
  }

  // forward analysis
  Forward(func_->body);

  // backward analysis
  Var dummy = CreateNull();
  live_[dummy] = {};
  Backward(func_->body, dummy);

  // init find
  for (const auto& kv : vset_) {
    const Var& var = kv.first;
    const auto& tensors = GetTensorVars(var);
    if (tensors.size() == 1 && *tensors.begin() == var) {
      union_find_forest_[var] = var;
    }
  }

  // init inv
  for (const auto& kv : live_) {
    const Var& k = kv.first;
    const VSet& vs = kv.second;
    for (const auto& v : vs) {
      inv_live_[v].insert(k);
    }
  }

  // mandatory memory sharing
  CHECK_EQ(var_out_.size(), var_in_.size());
  int m = var_out_.size();
  for (int i = 0; i < m; ++i) {
    Var fout = *GetTensorVars(var_out_[i]).begin();
    Var fin = *GetTensorVars(var_in_[i]).begin();
    CHECK(fout.defined());
    CHECK(fin.defined());
    fout = Find(fout);
    fin = Find(fin);
    if (fout != fin && Intersect(fout, fin)) {
      // the mandatory inplace update is invalid
      // something goes wrong here
      LOG(WARNING) << "Mandatory memory sharing between " << fin << " and " << fout
                   << " is invalid. Such cases cannot be handled by "
                   << "the liveness_analysis pass.";
      failure_ = true;
    } else {
      // the mandatory inplace update is valid
      Unite(fin, fout);
    }
  }

  return live_;
}

void LivenessAnalyzer::FormChecker::VisitExpr_(const CallNode* node) {
  if (!node->op.as<OpNode>()) {
    // Do not support closure yet.
    analyzer_->failure_ = true;
  } else {
    const Array<Expr>& args = node->args;
    Array<Var> vargs;
    for (const auto& arg : node->args) {
      if (arg.as<VarNode>() == nullptr && arg.as<ConstantNode>() == nullptr) {
        // Only support ANF with exceptions of Constant
        analyzer_->failure_ = true;
      }
    }
  }
}

void LivenessAnalyzer::FormChecker::VisitExpr_(const IfNode* node) {
  if (node->cond.as<VarNode>() == nullptr) {
    // Only support ANF.
    analyzer_->failure_ = true;
  }
}

void LivenessAnalyzer::ForwardAnalyzer::VisitExpr_(const VarNode* node) {
  auto vars = analyzer_->GetTensorVars(GetRef<Var>(node));
  CHECK_EQ(vars.size(), 1U);
  analyzer_->Init(let_var_, vars[0]);
}

void LivenessAnalyzer::ForwardAnalyzer::VisitExpr_(const FunctionNode* node) {
  /*!
   * When a closure is used, the value of the captured variables are required.
   * For example, in
   * fn {
   *   let %closure = {
   *     %b1 = %a1 + %a1
   *     %b1
   *   }
   *   %closure  // here %a1 is used, and thus cannot be inplace rewritten
   * }
   * when the closure is invoked/returned, the value of %a1 (captured variables) is needed.
   */
  Function f = GetRef<Function>(node);
  Array<Var> free_vars = FreeVars(f);
  analyzer_->Init(let_var_, analyzer_->Merge(free_vars));
}

void LivenessAnalyzer::ForwardAnalyzer::VisitExpr_(const CallNode* node) {
  if (node->op.as<OpNode>()) {
    Var dummy = analyzer_->CreateTensorVar(node->checked_type());
    analyzer_->Init(let_var_, dummy);
  } else {
    LOG(FATAL) << "NotImplementedError: Calling unsupported type: " << node->op->GetTypeKey();
  }
}

void LivenessAnalyzer::ForwardAnalyzer::VisitExpr_(const TupleNode* node) {
  Array<Var> fields;
  for (const auto& field : node->fields) {
    Var var;
    if (field.as<VarNode>()) {
      // Ignore constant fields (e.g., NoGradValue)
      var = Downcast<Var>(field);
    }
    fields.push_back(var);
  }
  analyzer_->Init(let_var_, analyzer_->Merge(fields));
  analyzer_->vtuple_.Set(let_var_, fields);
}

void LivenessAnalyzer::ForwardAnalyzer::VisitExpr_(const TupleGetItemNode* node) {
  auto tuple = Downcast<Var>(node->tuple);
  CHECK(analyzer_->vtuple_.find(tuple) != analyzer_->vtuple_.end());
  Var var = analyzer_->vtuple_.at(tuple)[node->index];
  analyzer_->Init(let_var_, var);
}

void LivenessAnalyzer::ForwardAnalyzer::VisitExpr_(const IfNode* node) {
  Expr true_branch = node->true_branch;
  Expr false_branch = node->false_branch;
  Var true_ret = analyzer_->Forward(true_branch);
  Var false_ret = analyzer_->Forward(false_branch);
  Var ret = analyzer_->CreateTensorVar(node->checked_type());
  // mandatory memory sharing if condition is true
  Match(ret, true_ret);
  // mandatory memory sharing if condition is false
  Match(ret, false_ret);
  analyzer_->Init(let_var_, ret);
}

void LivenessAnalyzer::ForwardAnalyzer::Match(Var v1, Var v2) {
  if (analyzer_->vtuple_.count(v1) > 0) {
    CHECK(analyzer_->vtuple_.find(v1) != analyzer_->vtuple_.end());
    CHECK(analyzer_->vtuple_.find(v2) != analyzer_->vtuple_.end());
    Array<Var> v1t = analyzer_->vtuple_.at(v1);
    Array<Var> v2t = analyzer_->vtuple_.at(v2);
    Array<Var> fields;
    CHECK_EQ(v1t.size(), v2t.size());
    for (size_t i = 0; i < v1t.size(); ++i) {
      Match(v1t[i], v2t[i]);
    }
  } else {
    analyzer_->var_out_.push_back(v1);
    analyzer_->var_in_.push_back(v2);
  }
}

Var LivenessAnalyzer::ForwardAnalyzer::Run() {
  const auto& vars = ell_->vars;
  const auto& exprs = ell_->exprs;
  CHECK_EQ(vars.size(), exprs.size());
  int n = exprs.size();

  // Forward analysis
  for (int i = 0; i < n; ++i) {
    let_var_ = vars[i];

    // We need to handle OpNode here, because all OpNodes with the same op point to
    // the same reference, so only the first OpNode will be visited.
    if (exprs[i].as<OpNode>()) {
      analyzer_->Init(let_var_, analyzer_->CreateNull("op"));
    }
    ExprVisitor::VisitExpr(exprs[i]);
  }
  return ell_->ret;
}

void LivenessAnalyzer::BackwardAnalyzer::VisitExpr_(const VarNode* node) {
  auto vars = analyzer_->GetTensorVars(GetRef<Var>(node));
  CHECK_EQ(vars.size(), 1U);
  analyzer_->live_[let_var_] = analyzer_->vset_[MergeLive(vars[0])];
}

void LivenessAnalyzer::BackwardAnalyzer::VisitExpr_(const FunctionNode* node) {
  Function f = GetRef<Function>(node);
  Array<Var> free_vars = FreeVars(f);
  analyzer_->live_[let_var_] = analyzer_->vset_[MergeLive(let_var_)];
}

void LivenessAnalyzer::BackwardAnalyzer::VisitExpr_(const CallNode* node) {
  if (node->op.as<OpNode>()) {
    const Array<Expr>& args = node->args;
    Array<Var> vargs;
    for (const auto& arg : node->args) {
      if (arg.as<VarNode>()) {
        // use %arg
        vargs.push_back(Downcast<Var>(arg));
      } else if (arg.as<ConstantNode>() || arg.as<OpNode>()) {
        // use nothing
      } else {
        LOG(FATAL) << "NotImplementedError: unsupported args: " << arg->GetTypeKey();
      }
    }
    Var d1 = analyzer_->Merge(vargs);
    Var d2 = MergeLive(d1, let_var_);
    analyzer_->live_[let_var_] = analyzer_->vset_[d2];
  } else {
    LOG(FATAL) << "NotImplementedError: Calling unsupported type: " << node->op->GetTypeKey();
  }
}

void LivenessAnalyzer::BackwardAnalyzer::VisitExpr_(const TupleNode* node) {
  analyzer_->live_[let_var_] = analyzer_->vset_[MergeLive(let_var_)];
}

void LivenessAnalyzer::BackwardAnalyzer::VisitExpr_(const TupleGetItemNode* node) {
  analyzer_->live_[let_var_] = analyzer_->vset_[MergeLive(let_var_)];
}

void LivenessAnalyzer::BackwardAnalyzer::VisitExpr_(const IfNode* node) {
  Var free_true = analyzer_->Merge(FreeVars(node->true_branch));
  Var free_false = analyzer_->Merge(FreeVars(node->false_branch));
  analyzer_->live_[let_var_] = analyzer_->vset_[MergeLive(
      analyzer_->Merge({free_true, free_false, Downcast<Var>(node->cond)}), let_var_)];
  VisitBranch(node->true_branch, let_var_);
  VisitBranch(node->false_branch, let_var_);
}

void LivenessAnalyzer::BackwardAnalyzer::VisitBranch(const Expr& branch, const Var& def) {
  Var total_next = analyzer_->CreateTensorVar("if");
  // get total live-out variables of true_branch
  analyzer_->vset_[total_next] = analyzer_->live_[next_var_];
  // remove the tensors defined at this line
  Var branch_next = analyzer_->Remove(total_next, def);
  analyzer_->live_[branch_next] = analyzer_->vset_[branch_next];
  analyzer_->Backward(branch, branch_next);
}

void LivenessAnalyzer::BackwardAnalyzer::Run(Var next_var) {
  const auto& vars = ell_->vars;
  const auto& exprs = ell_->exprs;
  CHECK_EQ(vars.size(), exprs.size());
  int n = exprs.size();

  // Backward analysis
  next_var_ = next_var;
  Var dummy = analyzer_->CreateNull();
  analyzer_->live_[dummy] = analyzer_->vset_[MergeLive(ell_->ret)];
  for (int i = n - 1; i >= 0; --i) {
    let_var_ = vars[i];
    next_var_ = i == n - 1 ? dummy : vars[i + 1];

    // We need to handle OpNode here, because all OpNodes with the same op point to
    // the same reference, so only the first OpNode will be visited.
    if (exprs[i].as<OpNode>()) {
      auto dummy_vars = analyzer_->GetTensorVars(next_var_);
      Var d1 = analyzer_->Merge(dummy_vars);
      Var d2 = MergeLive(d1, next_var_);
      analyzer_->live_[let_var_] = analyzer_->vset_[d2];
    } else {
      CHECK_GT(analyzer_->live_.count(next_var_), 0);
    }
    ExprVisitor::VisitExpr(exprs[i]);
  }
}

Var LivenessAnalyzer::Forward(const Expr& e) {
  return ForwardAnalyzer(e, this).Run();
}

void LivenessAnalyzer::Backward(const Expr& e, const Var& next_var) {
  BackwardAnalyzer(e, this).Run(next_var);
}

void LivenessAnalyzer::FormCheck(const Expr& e) {
  FormChecker(e, this).Run();
}

Var LivenessAnalyzer::CreateTensorVar(const Type& type) {
  return VarCreator(this).Run(type);
}

}  // namespace liveness_analysis

liveness_analysis::MapVSet LivenessAnalysis(const IRModule& mod) {
  auto entry = mod->GetGlobalVar("main");
  auto func = Downcast<Function>(mod->Lookup(entry));
  auto la = liveness_analysis::LivenessAnalyzer(func);
  return la.Run();
}

// Put the live in set to an Array as std::unordered_set is not in the object system.
PackedLiveInMap LivenessAnalysisPacked(const IRModule& mod) {
  PackedLiveInMap ret;
  auto res = LivenessAnalysis(mod);
  for (const auto& it : res) {
    Array<Var> vars;
    for (const auto& var : it.second) {
      vars.push_back(var);
    }
    ret.Set(it.first, vars);
  }
  return ret;
}

MNM_REGISTER_GLOBAL("mnm.pass_.LivenessAnalysis").set_body_typed(LivenessAnalysisPacked);

}  // namespace pass
}  // namespace mnm