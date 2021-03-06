// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "tools/transform.h"
#include "ir/globals.h"
#include "ir/state.h"
#include "smt/expr.h"
#include "smt/smt.h"
#include "smt/solver.h"
#include "util/config.h"
#include "util/errors.h"
#include "util/stopwatch.h"
#include "util/symexec.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

using namespace IR;
using namespace smt;
using namespace tools;
using namespace util;
using namespace std;


static bool is_undef(const expr &e) {
  if (e.isConst())
    return false;
  return check_expr(expr::mkForAll(e.vars(), expr::mkVar("#undef", e) != e)).
           isUnsat();
}

static void print_single_varval(ostream &os, State &st, const Model &m,
                                const Value *var, const Type &type,
                                const StateValue &val) {
  if (!val.isValid()) {
    os << "(invalid expr)";
    return;
  }

  // if the model is partial, we don't know for sure if it's poison or not
  // this happens if the poison constraint depends on an undef
  // however, cexs are usually triggered by the worst case, which is poison
  if (auto v = m.eval(val.non_poison);
      (!v.isConst() || v.isFalse())) {
    os << "poison";
    return;
  }

  if (auto *in = dynamic_cast<const Input*>(var)) {
    uint64_t n;
    ENSURE(m[in->getTyVar()].isUInt(n));
    if (n == 1) {
      os << "undef";
      return;
    }
    assert(n == 0);
  }

  expr partial = m.eval(val.value);
  if (is_undef(partial)) {
    os << "undef";
    return;
  }

  type.printVal(os, st, m.eval(val.value, true));

  // undef variables may not have a model since each read uses a copy
  // TODO: add intervals of possible values for ints at least?
  if (!partial.isConst()) {
    // some functions / vars may not have an interpretation because it's not
    // needed, not because it's undef
    bool found_undef = false;
    for (auto &var : partial.vars()) {
      if ((found_undef = isUndef(var)))
        break;
    }
    if (found_undef)
      os << "\t[based on undef value]";
  }
}

static void print_varval(ostream &os, State &st, const Model &m,
                         const Value *var, const Type &type,
                         const StateValue &val) {
  if (!type.isAggregateType()) {
    print_single_varval(os, st, m, var, type, val);
    return;
  }

  os << (type.isStructType() ? "{ " : "< ");
  auto agg = type.getAsAggregateType();
  for (unsigned i = 0, e = agg->numElementsConst(); i < e; ++i) {
    if (i != 0)
      os << ", ";
    print_varval(os, st, m, var, agg->getChild(i), agg->extract(val, i));
  }
  os << (type.isStructType() ? " }" : " >");
}


using print_var_val_ty = function<void(ostream&, const Model&)>;

static void error(Errors &errs, State &src_state, State &tgt_state,
                  const Result &r, const Value *var,
                  const char *msg, bool check_each_var,
                  print_var_val_ty print_var_val) {

  if (r.isInvalid()) {
    errs.add("Invalid expr", false);
    return;
  }

  if (r.isTimeout()) {
    errs.add("Timeout", false);
    return;
  }

  if (r.isError()) {
    errs.add("SMT Error: " + r.getReason(), false);
    return;
  }

  if (r.isSkip()) {
    errs.add("Skip", false);
    return;
  }

  stringstream s;
  string empty;
  auto &var_name = var ? var->getName() : empty;
  auto &m = r.getModel();

  s << msg;
  if (!var_name.empty())
    s << " for " << *var;
  s << "\n\nExample:\n";

  for (auto &[var, val, used] : src_state.getValues()) {
    (void)used;
    if (!dynamic_cast<const Input*>(var) &&
        !dynamic_cast<const ConstantInput*>(var))
      continue;
    s << *var << " = ";
    print_varval(s, src_state, m, var, var->getType(), val.first);
    s << '\n';
  }

  set<string> seen_vars;
  for (auto st : { &src_state, &tgt_state }) {
    if (!check_each_var) {
      if (st->isSource()) {
        s << "\nSource:\n";
      } else {
        s << "\nTarget:\n";
      }
    }

    for (auto &[var, val, used] : st->getValues()) {
      (void)used;
      auto &name = var->getName();
      if (name == var_name)
        break;

      if (name[0] != '%' ||
          dynamic_cast<const Input*>(var) ||
          (check_each_var && !seen_vars.insert(name).second))
        continue;

      s << *var << " = ";
      print_varval(s, const_cast<State&>(*st), m, var, var->getType(),
                   val.first);
      s << '\n';
    }

    st->getMemory().print(s, m);
  }

  print_var_val(s, m);
  errs.add(s.str(), true);
}


static expr preprocess(Transform &t, const set<expr> &qvars0,
                       const set<expr> &undef_qvars, expr && e) {
  if (hit_half_memory_limit())
    return expr::mkForAll(qvars0, move(e));

  // TODO: benchmark
  if (0) {
    expr var = expr::mkBoolVar("malloc_never_fails");
    e = expr::mkIf(var,
                   e.subst(var, true).simplify(),
                   e.subst(var, false).simplify());
  }

  // eliminate all quantified boolean vars; Z3 gets too slow with those
  auto qvars = qvars0;
  for (auto &var : qvars0) {
    if (!var.isBool())
      continue;
    e = e.subst(var, true).simplify() &&
        e.subst(var, false).simplify();
    qvars.erase(var);
  }

  // TODO: maybe try to instantiate undet_xx vars?
  if (undef_qvars.empty() || hit_half_memory_limit())
    return expr::mkForAll(qvars, move(e));

  // manually instantiate all ty_%v vars
  map<expr, expr> instances({ { move(e), true } });
  map<expr, expr> instances2;

  expr nums[3] = { expr::mkUInt(0, 2), expr::mkUInt(1, 2), expr::mkUInt(2, 2) };

  for (auto &i : t.src.getInputs()) {
    auto in = dynamic_cast<const Input*>(&i);
    if (!in)
      continue;
    auto var = in->getTyVar();

    for (auto &[e, v] : instances) {
      for (unsigned i = 0; i <= 2; ++i) {
        if (config::disable_undef_input && i == 1)
          continue;
        if (config::disable_poison_input && i == 2)
          continue;

        expr newexpr = e.subst(var, nums[i]);
        if (newexpr.eq(e)) {
          instances2[move(newexpr)] = v;
          break;
        }

        newexpr = newexpr.simplify();
        if (newexpr.isFalse())
          continue;

        // keep 'var' variables for counterexample printing
        instances2.try_emplace(move(newexpr), v && var == nums[i]);
      }
    }
    instances = move(instances2);

    // Bail out if it gets too big. It's very likely we can't solve it anyway.
    if (instances.size() >= 128 || hit_half_memory_limit())
      break;
  }

  expr insts(false);
  for (auto &[e, v] : instances) {
    insts |= expr::mkForAll(qvars, move(const_cast<expr&>(e))) && v;
  }

  // TODO: try out instantiating the undefs in forall quantifier

  return insts;
}


static void check_refinement(Errors &errs, Transform &t,
                             State &src_state, State &tgt_state,
                             const Value *var, const Type &type,
                             const expr &dom_a, const State::ValTy &ap,
                             const expr &dom_b, const State::ValTy &bp,
                             bool check_each_var) {
  auto &a = ap.first;
  auto &b = bp.first;

  auto &uvars = ap.second;
  auto qvars = src_state.getQuantVars();
  qvars.insert(ap.second.begin(), ap.second.end());

  auto err = [&](const Result &r, print_var_val_ty print, const char *msg) {
    error(errs, src_state, tgt_state, r, var, msg, check_each_var, print);
  };

  auto print_value = [&](ostream &s, const Model &m) {
    s << "Source value: ";
    print_varval(s, src_state, m, var, type, a);
    s << "\nTarget value: ";
    print_varval(s, tgt_state, m, var, type, b);
  };

  AndExpr axioms = src_state.getAxioms();
  axioms.add(tgt_state.getAxioms());

  // restrict type variable from taking disabled values
  if (config::disable_undef_input || config::disable_poison_input) {
    for (auto &i : t.src.getInputs()) {
      if (auto in = dynamic_cast<const Input*>(&i)) {
        auto var = in->getTyVar();
        if (config::disable_undef_input) {
          if (config::disable_poison_input)
            axioms.add(var == 0);
          else
            axioms.add(var != 1);
        } else if (config::disable_poison_input)
          axioms.add(var.extract(1, 1) == 0);
      }
    }
  }

  // note that precondition->toSMT() may add stuff to getPre,
  // so order here matters
  // FIXME: broken handling of transformation precondition
  //src_state.startParsingPre();
  //expr pre = t.precondition ? t.precondition->toSMT(src_state) : true;
  auto pre_src_and = src_state.getPre();
  auto &pre_tgt_and = tgt_state.getPre();

  // optimization: rewrite "tgt /\ (src -> foo)" to "tgt /\ foo" if src = tgt
  pre_src_and.del(pre_tgt_and);
  expr pre_src = pre_src_and();
  expr pre_tgt = pre_tgt_and();

  expr axioms_expr = axioms();
  expr dom = dom_a && dom_b;

  pre_tgt &= src_state.getOOM()();
  pre_tgt &= !tgt_state.sinkDomain();
  pre_tgt &= src_state.getPre(true)();
  pre_tgt &= tgt_state.getPre(true)();

  auto [poison_cnstr, value_cnstr] = type.refines(src_state, tgt_state, a, b);

  auto src_mem = src_state.returnMemory();
  auto tgt_mem = tgt_state.returnMemory();
  auto [memory_cnstr, ptr_refinement0] = src_mem.refined(tgt_mem, false);
  auto &ptr_refinement = ptr_refinement0;

  if (check_expr(axioms_expr && (pre_src && pre_tgt)).isUnsat()) {
    errs.add("Precondition is always false", false);
    return;
  }

  auto mk_fml = [&](expr &&refines) -> expr {
    // from the check above we already know that
    // \exists v,v' . pre_tgt(v') && pre_src(v) is SAT (or timeout)
    // so \forall v . pre_tgt && (!pre_src(v) || refines) simplifies to:
    // (pre_tgt && !pre_src) || (!pre_src && false) ->   [assume refines=false]
    // \forall v . (pre_tgt && !pre_src(v)) ->  [\exists v . pre_src(v)]
    // false
    if (refines.isFalse())
      return move(refines);

    auto fml = pre_tgt && pre_src.implies(refines);
    return axioms_expr && preprocess(t, qvars, uvars, move(fml));
  };

  auto print_ptr_load = [&](ostream &s, const Model &m) {
    Pointer p(src_mem, m[ptr_refinement()]);
    s << "\nMismatch in " << p
      << "\nSource value: " << Byte(src_mem, m[src_mem.load(p)()])
      << "\nTarget value: " << Byte(tgt_mem, m[tgt_mem.load(p)()]);
  };

  Solver::check({
    { mk_fml(dom_a.notImplies(dom_b)),
      [&](const Result &r) {
        err(r, [](ostream&, const Model&){},
            "Source is more defined than target");
      }},
    { mk_fml(dom && !poison_cnstr),
      [&](const Result &r) {
        err(r, print_value, "Target is more poisonous than source");
      }},
    { mk_fml(dom && !value_cnstr),
      [&](const Result &r) {
        err(r, print_value, "Value mismatch");
      }},
    { mk_fml(dom && !memory_cnstr),
      [&](const Result &r) {
        err(r, print_ptr_load, "Mismatch in memory");
      }}
  });
}

static const ConversionOp* is_bitcast(const Value &v) {
  if (auto c = dynamic_cast<const ConversionOp*>(&v))
    if (c->getOp() == ConversionOp::BitCast)
      return c;
  return nullptr;
}

static bool has_nullptr(const Value *v) {
  if (dynamic_cast<const NullPointerValue*>(v) ||
      (dynamic_cast<const UndefValue*>(v) && hasPtr(v->getType())))
      // undef pointer points to the nullblk
    return true;

  if (auto agg = dynamic_cast<const AggregateValue*>(v)) {
    for (auto val : agg->getVals()) {
      if (has_nullptr(val))
        return true;
    }
  }

  return false;
}

static unsigned num_ptrs(const Type &ty) {
  unsigned n = ty.isPtrType();
  if (auto aty = ty.getAsAggregateType())
    n += aty->numPointerElements();
  return n;
}

static bool returns_local(const Value &v) {
  return dynamic_cast<const Alloc*>(&v) ||
         dynamic_cast<const Malloc*>(&v) ||
         dynamic_cast<const Calloc*>(&v);
}

static bool may_be_nonlocal(Value *ptr) {
  vector<Value*> todo = { ptr };
  set<Value*> seen;
  do {
    ptr = todo.back();
    todo.pop_back();
    if (!seen.insert(ptr).second)
      continue;

    if (returns_local(*ptr))
      continue;

    if (auto gep = dynamic_cast<GEP*>(ptr)) {
      todo.emplace_back(&gep->getPtr());
      continue;
    }

    if (auto c = is_bitcast(*ptr)) {
      todo.emplace_back(&c->getValue());
      continue;
    }

    if (auto phi = dynamic_cast<Phi*>(ptr)) {
      for (auto &op : phi->operands())
        todo.emplace_back(op);
      continue;
    }
    return true;

  } while (!todo.empty());

  return false;
}

static unsigned returns_nonlocal(const Instr &inst) {
  bool rets_nonloc = false;

  if (dynamic_cast<const FnCall *>(&inst)) {
    rets_nonloc = true;
  }
  else if (auto load = dynamic_cast<const Load *>(&inst)) {
    rets_nonloc = may_be_nonlocal(&load->getPtr());
  }
  else if (auto conv = dynamic_cast<const ConversionOp *>(&inst)) {
    rets_nonloc = conv->getOp() == ConversionOp::Int2Ptr;
  }
  return rets_nonloc ? num_ptrs(inst.getType()) : 0;
}

static optional<int64_t> get_int(const Value &val) {
  if (auto i = dynamic_cast<const IntConst*>(&val)) {
    if (auto n = i->getInt())
      return *n;
  }
  return {};
}

static uint64_t max_mem_access, max_alloc_size;

static uint64_t get_globalvar_size(const Value *V) {
  if (auto cast = is_bitcast(*V))
    return get_globalvar_size(&cast->getValue());
  if (auto glb = dynamic_cast<const GlobalVariable *>(V))
    return glb->size();
  return UINT64_MAX;
}

static uint64_t max_gep(const Instr &inst) {
  if (auto conv = dynamic_cast<const ConversionOp*>(&inst)) {
    // if addresses are observed, then expose full ptr range
    if (conv->getOp() == ConversionOp::Int2Ptr ||
        conv->getOp() == ConversionOp::Ptr2Int)
      return max_mem_access = max_alloc_size = UINT64_MAX;
  }

  if (auto gep = dynamic_cast<const GEP*>(&inst)) {
    int64_t off = 0;
    for (auto &[mul, v] : gep->getIdxs()) {
      if (auto n = get_int(*v)) {
        off += mul * *n;
        continue;
      }
      return UINT64_MAX;
    }
    return abs(off);
  }
  if (auto load = dynamic_cast<const Load*>(&inst)) {
    max_mem_access = max(max_mem_access,
                         (uint64_t)Memory::getStoreByteSize(load->getType()));
    return 0;
  }
  if (auto store = dynamic_cast<const Store*>(&inst)) {
    max_mem_access
      = max(max_mem_access,
            (uint64_t)Memory::getStoreByteSize(store->getValue().getType()));
    return 0;
  }
  if (auto cpy = dynamic_cast<const Memcpy*>(&inst)) {
    if (auto bytes = get_int(cpy->getBytes()))
      max_mem_access = max(max_mem_access, (uint64_t)abs(*bytes));
    else
      max_mem_access = UINT64_MAX;
    return 0;
  }
  if (auto memset = dynamic_cast<const Memset *>(&inst)) {
    if (auto bytes = get_int(memset->getBytes()))
      max_mem_access = max(max_mem_access, (uint64_t)abs(*bytes));
    else
      max_mem_access = UINT64_MAX;
    return 0;
  }
  if (auto slen = dynamic_cast<const Strlen *>(&inst)) {
    max_mem_access = max(max_mem_access,
                         get_globalvar_size(slen->getPointer()));
    return 0;
  }

  Value *size;
  uint64_t mul = 1;
  if (auto alloc = dynamic_cast<const Alloc*>(&inst)) {
    size = &alloc->getSize();
    if (alloc->getMul()) {
      if (auto n = get_int(*alloc->getMul())) {
        mul = *n;
      } else {
        max_alloc_size = UINT64_MAX;
      }
    }
  } else if (auto alloc = dynamic_cast<const Malloc*>(&inst)) {
    size = &alloc->getSize();
  } else if (auto alloc = dynamic_cast<const Calloc*>(&inst)) {
    size = &alloc->getSize();
    if (auto n = get_int(alloc->getNum())) {
      mul = *n;
    } else {
      max_alloc_size = UINT64_MAX;
    }
  } else {
    return 0;
  }

  if (auto bytes = get_int(*size)) {
    max_alloc_size = max(max_alloc_size, mul * abs(*bytes));
  } else {
    max_alloc_size = UINT64_MAX;
  }
  return 0;
}

static bool has_sub_byte(const Type &t) {
  if (auto agg = t.getAsAggregateType()) {
    if (t.isVectorType()) {
      auto &elemTy = agg->getChild(0);
      return elemTy.isPtrType() ? false : (elemTy.bits() % 8);
    }

    for (unsigned i = 0, e = agg->numElementsConst(); i != e;  ++i) {
      if (has_sub_byte(agg->getChild(i)))
        return true;
    }
  }
  return false;
}

static uint64_t get_access_size(const Type &ty) {
  if (auto agg = ty.getAsAggregateType()) {
    uint64_t sz = 1;
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      auto n = get_access_size(agg->getChild(i));
      sz = i == 0 ? n : gcd(sz, n);
    }
    return sz;
  }
  if (ty.isPtrType())
    return bits_program_pointer / 8;
  return divide_up(ty.bits(), 8);
}

static uint64_t get_access_size(const Instr &inst) {
  if (auto i = dynamic_cast<const Calloc*>(&inst)) {
    does_int_mem_access = true;
    if (auto n = get_int(i->getNum()))
      if (auto sz = get_int(i->getSize()))
        // assume calloc is 8 bytes aligned
        return gcd(8, *n * *sz);
    return 1;
  }

  if (dynamic_cast<const Strlen*>(&inst))
    return 1;

  if (auto i = dynamic_cast<const Memcpy*>(&inst)) {
#if 0
    if (auto bytes = get_int(i->getBytes()))
      return gcd(gcd(i->getSrcAlign(), i->getDstAlign()), *bytes);
#endif
    // FIXME: memcpy doesn't have multi-byte support
    (void)i;
    does_ptr_mem_access = true;
    return 1;
  }

  if (auto i = dynamic_cast<const Malloc*>(&inst)) {
    // FIXME: memcpy doesn't have multi-byte support
    if (i->isRealloc())
      return 1;
  }

  if (auto i = dynamic_cast<const Memset*>(&inst)) {
    does_int_mem_access = true;
    if (auto bytes = get_int(i->getBytes()))
      return gcd(i->getAlign(), *bytes);
    return 1;
  }

  Type *value_ty;
  unsigned align;
  if (auto st = dynamic_cast<const Store*>(&inst)) {
    value_ty = &st->getValue().getType();
    align = st->getAlign();
    does_ptr_store |= hasPtr(*value_ty);
  } else if (auto ld = dynamic_cast<const Load*>(&inst)) {
    value_ty = &ld->getType();
    align = ld->getAlign();
  } else if (auto cast = is_bitcast(inst)) {
    value_ty = &cast->getType();
    align = 64; // just some high value
  } else
    return 0;

  does_ptr_mem_access |= hasPtr(*value_ty);
  does_int_mem_access |= value_ty->enforcePtrOrVectorType().isFalse();

  return gcd(align, get_access_size(*value_ty));
}

static void calculateAndInitConstants(Transform &t) {
  const auto &globals_tgt = t.tgt.getGlobalVars();
  const auto &globals_src = t.src.getGlobalVars();
  unsigned num_globals_src = globals_src.size();
  unsigned num_globals = num_globals_src;

  // FIXME: varies among address spaces
  bits_program_pointer = t.src.bitsPointers();
  assert(bits_program_pointer > 0 && bits_program_pointer <= 64);
  assert(bits_program_pointer == t.tgt.bitsPointers());
  heap_block_alignment = 8;

  for (auto GVT : globals_tgt) {
    auto I = find_if(globals_src.begin(), globals_src.end(),
      [GVT](auto *GV) -> bool { return GVT->getName() == GV->getName(); });
    if (I == globals_src.end())
      ++num_globals;
  }

  unsigned num_ptrinputs = 0;
  for (auto &arg : t.src.getInputs()) {
    num_ptrinputs += num_ptrs(arg.getType());
  }

  // The number of instructions that can return a pointer to a non-local block.
  num_max_nonlocals_inst = 0;
  // The number of local blocks.
  unsigned num_locals_src = 0, num_locals_tgt = 0;
  uint64_t max_gep_src = 0, max_gep_tgt = 0;
  max_alloc_size = 0;
  max_mem_access = 0;

  for (auto BB : t.src.getBBs()) {
    for (auto &i : BB->instrs()) {
      if (returns_local(i))
        ++num_locals_src;
      else
        num_max_nonlocals_inst += returns_nonlocal(i);
      max_gep_src = add_saturate(max_gep_src, max_gep(i));
    }
  }
  for (auto BB : t.tgt.getBBs()) {
    for (auto &i : BB->instrs()) {
      if (returns_local(i))
        ++num_locals_tgt;
      else
        num_max_nonlocals_inst += returns_nonlocal(i);
      max_gep_tgt = add_saturate(max_gep_tgt, max_gep(i));
    }
  }
  num_locals = max(num_locals_src, num_locals_tgt);

  uint64_t min_global_size = UINT64_MAX;
  for (auto glbs : { &globals_src, &globals_tgt}) {
    for (auto &glb : *glbs) {
      auto sz = max(glb->size(), (uint64_t)1u);
      max_mem_access = max(sz, max_mem_access);
      min_global_size = min_global_size != UINT64_MAX
                          ? gcd(sz, min_global_size)
                          : sz;
      min_global_size = gcd(min_global_size, glb->getAlignment());
    }
  }

  bool nullptr_is_used = false;
  has_int2ptr      = false;
  has_ptr2int      = false;
  has_malloc       = false;
  has_free         = false;
  has_fncall       = false;
  has_null_block   = false;
  does_ptr_store   = false;
  does_ptr_mem_access = false;
  does_int_mem_access = false;

  // Mininum access size (in bytes)
  uint64_t min_access_size = 8;
  bool does_mem_access = false;
  bool has_ptr_load = false;
  does_sub_byte_access = false;

  for (auto fn : { &t.src, &t.tgt }) {
    for (auto BB : fn->getBBs()) {
      for (auto &I : BB->instrs()) {
        for (auto op : I.operands()) {
          nullptr_is_used |= has_nullptr(op);
          does_sub_byte_access |= has_sub_byte(op->getType());
        }

        if (auto conv = dynamic_cast<const ConversionOp*>(&I)) {
          has_int2ptr |= conv->getOp() == ConversionOp::Int2Ptr;
          has_ptr2int |= conv->getOp() == ConversionOp::Ptr2Int;
        }

        if (auto alloc = dynamic_cast<const Alloc*>(&I))
          has_dead_allocas |= alloc->initDead();

        if (auto alloc = dynamic_cast<const Malloc*>(&I)) {
          has_malloc |= true;
          has_free |= alloc->isRealloc();
        }

        has_malloc |= dynamic_cast<const Calloc*>(&I) != nullptr;
        has_free   |= dynamic_cast<const Free*>(&I) != nullptr;
        has_fncall |= dynamic_cast<const FnCall*>(&I) != nullptr;
        if (auto *load = dynamic_cast<const Load*>(&I))
          has_ptr_load |= hasPtr(load->getType());

        does_sub_byte_access |= has_sub_byte(I.getType());

        if (auto accsz = get_access_size(I)) {
          min_access_size = gcd(min_access_size, accsz);
          does_mem_access = true;
        }
      }
    }
  }

  // check if null block is needed
  // Global variables cannot be null pointers
  has_null_block = num_ptrinputs > 0 || nullptr_is_used || has_malloc ||
                  has_ptr_load || has_fncall;

  num_nonlocals_src = num_globals_src + num_ptrinputs + num_max_nonlocals_inst +
                      has_null_block;

  // Allow at least one non-const global for calls to change
  num_nonlocals_src += has_fncall;

  num_nonlocals = num_nonlocals_src + num_globals - num_globals_src;

  if (!does_int_mem_access && !does_ptr_mem_access && has_fncall)
    does_int_mem_access = true;

  auto has_attr = [&](Input::Attribute a) -> bool {
    for (auto fn : { &t.src, &t.tgt }) {
      for (auto &v : fn->getInputs()) {
        auto i = dynamic_cast<const Input*>(&v);
        if (i && i->hasAttribute(a))
          return true;
      }
    }
    return false;
  };
  // The number of bits needed to encode pointer attributes
  // nonnull and byval isn't encoded in ptr attribute bits
  bool has_byval = has_attr(Input::ByVal);
  has_nocapture = has_attr(Input::NoCapture);
  has_readonly = has_attr(Input::ReadOnly);
  has_readnone = has_attr(Input::ReadNone);
  bits_for_ptrattrs = has_nocapture + has_readonly + has_readnone;

  // ceil(log2(maxblks)) + 1 for local bit
  bits_for_bid = max(1u, ilog2_ceil(max(num_locals, num_nonlocals), false))
                   + (num_locals && num_nonlocals);

  // reserve a multiple of 4 for the number of offset bits to make SMT &
  // counterexamples more readable
  // Allow an extra bit for the sign
  auto max_geps
    = ilog2_ceil(add_saturate(max(max_gep_src, max_gep_tgt), max_mem_access),
                 true) + 1;
  bits_for_offset = min(round_up(max_geps, 4), (uint64_t)t.src.bitsPtrOffset());

  // we need an extra bit because 1st bit of size is always 0
  bits_size_t = ilog2_ceil(max_alloc_size, true);
  bits_size_t = min(max(bits_for_offset, bits_size_t)+1, bits_program_pointer);

  // size of byte
  if (num_globals != 0) {
    if (does_mem_access)
      min_access_size = gcd(min_global_size, min_access_size);
    else {
      min_access_size = min_global_size;
      while (min_access_size > 8) {
        if (min_access_size % 2) {
          min_access_size = 1;
          break;
        }
        min_access_size /= 2;
      }
    }
  }
  if (has_byval)
    min_access_size = 1;
  bits_byte = 8 * ((does_mem_access || num_globals != 0)
                     ? (unsigned)min_access_size : 1);

  strlen_unroll_cnt = 10;

  little_endian = t.src.isLittleEndian();

  if (config::debug)
    config::dbg() << "num_max_nonlocals_inst: " << num_max_nonlocals_inst
                  << "\nnum_locals: " << num_locals
                  << "\nnum_nonlocals_src: " << num_nonlocals_src
                  << "\nnum_nonlocals: " << num_nonlocals
                  << "\nbits_for_bid: " << bits_for_bid
                  << "\nbits_for_offset: " << bits_for_offset
                  << "\nbits_size_t: " << bits_size_t
                  << "\nbits_program_pointer: " << bits_program_pointer
                  << "\nmax_alloc_size: " << max_alloc_size
                  << "\nmin_access_size: " << min_access_size
                  << "\nmax_mem_access: " << max_mem_access
                  << "\nbits_byte: " << bits_byte
                  << "\nstrlen_unroll_cnt: " << strlen_unroll_cnt
                  << "\nlittle_endian: " << little_endian
                  << "\nnullptr_is_used: " << nullptr_is_used
                  << "\nhas_int2ptr: " << has_int2ptr
                  << "\nhas_ptr2int: " << has_ptr2int
                  << "\nhas_malloc: " << has_malloc
                  << "\nhas_free: " << has_free
                  << "\nhas_null_block: " << has_null_block
                  << "\ndoes_ptr_store: " << does_ptr_store
                  << "\ndoes_ptr_mem_access: " << does_ptr_mem_access
                  << "\ndoes_int_mem_access: " << does_int_mem_access
                  << "\ndoes_sub_byte_access: " << does_sub_byte_access
                  << "\n";
}


namespace tools {

TransformVerify::TransformVerify(Transform &t, bool check_each_var) :
  t(t), check_each_var(check_each_var) {
  if (check_each_var) {
    for (auto &i : t.tgt.instrs()) {
      tgt_instrs.emplace(i.getName(), &i);
    }
  }
}

Errors TransformVerify::verify() const {
  try {
    t.tgt.syncDataWithSrc(t.src);
  } catch (AliveException &ae) {
    return Errors(move(ae));
  }

  // Check sizes of global variables
  auto globals_tgt = t.tgt.getGlobalVars();
  auto globals_src = t.src.getGlobalVars();
  for (auto GVS : globals_src) {
    auto I = find_if(globals_tgt.begin(), globals_tgt.end(),
      [GVS](auto *GV) -> bool { return GVS->getName() == GV->getName(); });
    if (I == globals_tgt.end())
      continue;

    auto GVT = *I;
    if (GVS->size() != GVT->size()) {
      stringstream ss;
      ss << "Unsupported interprocedural transformation: global variable "
        << GVS->getName() << " has different size in source and target ("
        << GVS->size() << " vs " << GVT->size()
        << " bytes)";
      return { ss.str(), false };
    } else if (GVS->isConst() && !GVT->isConst()) {
      stringstream ss;
      ss << "Transformation is incorrect because global variable "
        << GVS->getName() << " is const in source but not in target";
      return { ss.str(), true };
    } else if (!GVS->isConst() && GVT->isConst()) {
      stringstream ss;
      ss << "Unsupported interprocedural transformation: global variable "
        << GVS->getName() << " is const in target but not in source";
      return { ss.str(), false };
    }
  }

  StopWatch symexec_watch;
  calculateAndInitConstants(t);
  State::resetGlobals();
  State src_state(t.src, true), tgt_state(t.tgt, false);

  try {
    sym_exec(src_state);
    tgt_state.syncSEdataWithSrc(src_state);
    sym_exec(tgt_state);
    src_state.mkAxioms(tgt_state);
  } catch (AliveException e) {
    return move(e);
  }

  symexec_watch.stop();
  if (symexec_watch.seconds() > 5) {
    cerr << "WARNING: slow vcgen! Took " << symexec_watch << '\n';
  }

  Errors errs;

  if (check_each_var) {
    for (auto &[var, val, used] : src_state.getValues()) {
      (void)used;
      auto &name = var->getName();
      if (name[0] != '%' || !dynamic_cast<const Instr*>(var))
        continue;

      // TODO: add data-flow domain tracking for Alive, but not for TV
      check_refinement(errs, t, src_state, tgt_state, var, var->getType(),
                       true, val, true, tgt_state.at(*tgt_instrs.at(name)),
                       check_each_var);
      if (errs)
        return errs;
    }
  }

  check_refinement(errs, t, src_state, tgt_state, nullptr, t.src.getType(),
                   src_state.returnDomain()(), src_state.returnVal(),
                   tgt_state.returnDomain()(), tgt_state.returnVal(),
                   check_each_var);

  return errs;
}


TypingAssignments::TypingAssignments(const expr &e) : s(true), sneg(true) {
  if (e.isTrue()) {
    has_only_one_solution = true;
  } else {
    EnableSMTQueriesTMP tmp;
    s.add(e);
    sneg.add(!e);
    r = s.check();
  }
}

TypingAssignments::operator bool() const {
  return !is_unsat && (has_only_one_solution || r.isSat());
}

void TypingAssignments::operator++(void) {
  if (has_only_one_solution) {
    is_unsat = true;
  } else {
    EnableSMTQueriesTMP tmp;
    s.block(r.getModel(), &sneg);
    r = s.check();
    assert(r.isSat() || r.isUnsat());
  }
}

TypingAssignments TransformVerify::getTypings() const {
  auto c = t.src.getTypeConstraints() && t.tgt.getTypeConstraints();

  if (t.precondition)
    c &= t.precondition->getTypeConstraints();

  // return type
  c &= t.src.getType() == t.tgt.getType();

  if (check_each_var) {
    for (auto &i : t.src.instrs()) {
      if (!i.isVoid())
        c &= i.eqType(*tgt_instrs.at(i.getName()));
    }
  }
  return { move(c) };
}

void TransformVerify::fixupTypes(const TypingAssignments &ty) {
  if (ty.has_only_one_solution)
    return;
  if (t.precondition)
    t.precondition->fixupTypes(ty.r.getModel());
  t.src.fixupTypes(ty.r.getModel());
  t.tgt.fixupTypes(ty.r.getModel());
}

void Transform::print(ostream &os, const TransformPrintOpts &opt) const {
  os << "\n----------------------------------------\n";
  if (!name.empty())
    os << "Name: " << name << '\n';
  if (precondition) {
    precondition->print(os << "Pre: ");
    os << '\n';
  }
  src.print(os, opt.print_fn_header);
  os << "=>\n";
  tgt.print(os, opt.print_fn_header);
}

ostream& operator<<(ostream &os, const Transform &t) {
  t.print(os, {});
  return os;
}

}
