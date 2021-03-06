//===-- statements.cpp ----------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "init.h"
#include "mars.h"
#include "module.h"
#include "mtype.h"
#include "port.h"
#include "gen/abi.h"
#include "gen/arrays.h"
#include "gen/classes.h"
#include "gen/coverage.h"
#include "gen/dcompute/target.h"
#include "gen/dvalue.h"
#include "gen/funcgenstate.h"
#include "gen/irstate.h"
#include "gen/llvm.h"
#include "gen/llvmhelpers.h"
#include "gen/logger.h"
#include "gen/runtime.h"
#include "gen/tollvm.h"
#include "id.h"
#include "ir/irfunction.h"
#include "ir/irmodule.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InlineAsm.h"
#include <fstream>
#include <math.h>
#include <stdio.h>

// Need to include this after the other DMD includes because of missing
// dependencies.
#include "hdrgen.h"

//////////////////////////////////////////////////////////////////////////////
// FIXME: Integrate these functions
void AsmStatement_toIR(AsmStatement *stmt, IRState *irs);
void CompoundAsmStatement_toIR(CompoundAsmStatement *stmt, IRState *p);

//////////////////////////////////////////////////////////////////////////////

// For sorting string switch cases lexicographically.
namespace {
bool compareCaseStrings(CaseStatement *lhs, CaseStatement *rhs) {
  return lhs->exp->compare(rhs->exp) < 0;
}
}

static LLValue *call_string_switch_runtime(llvm::Value *table, Expression *e) {
  Type *dt = e->type->toBasetype();
  Type *dtnext = dt->nextOf()->toBasetype();
  TY ty = dtnext->ty;
  const char *fname;
  if (ty == Tchar) {
    fname = "_d_switch_string";
  } else if (ty == Twchar) {
    fname = "_d_switch_ustring";
  } else if (ty == Tdchar) {
    fname = "_d_switch_dstring";
  } else {
    llvm_unreachable("not char/wchar/dchar");
  }

  llvm::Function *fn = getRuntimeFunction(e->loc, gIR->module, fname);

  IF_LOG {
    Logger::cout() << *table->getType() << '\n';
    Logger::cout() << *fn->getFunctionType()->getParamType(0) << '\n';
  }
  assert(table->getType() == fn->getFunctionType()->getParamType(0));

  DValue *val = toElemDtor(e);
  LLValue *llval = DtoRVal(val);
  assert(llval->getType() == fn->getFunctionType()->getParamType(1));

  LLCallSite call = gIR->CreateCallOrInvoke(fn, table, llval);

  return call.getInstruction();
}

//////////////////////////////////////////////////////////////////////////////

class ToIRVisitor : public Visitor {
  IRState *irs;

public:
  explicit ToIRVisitor(IRState *irs) : irs(irs) {}

  //////////////////////////////////////////////////////////////////////////

  // Import all functions from class Visitor
  using Visitor::visit;

  //////////////////////////////////////////////////////////////////////////

  void visit(CompoundStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("CompoundStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    for (auto s : *stmt->statements) {
      if (s) {
        s->accept(this);
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ReturnStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ReturnStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    // The LLVM value to return, or null for void returns.
    LLValue *returnValue = nullptr;

    auto &funcGen = irs->funcGen();
    IrFunction *const f = &funcGen.irFunc;
    FuncDeclaration *const fd = f->decl;
    llvm::FunctionType *funcType = f->getLLVMFuncType();

    emitInstrumentationFnLeave(fd);

    // is there a return value expression?
    if (stmt->exp || (!stmt->exp && irs->isMainFunc(f))) {
      // if the function's return type is void, it uses sret
      if (funcType->getReturnType() == LLType::getVoidTy(irs->context())) {
        assert(!f->type->isref);

        LLValue *sretPointer = getIrFunc(fd)->sretArg;
        assert(sretPointer);

        assert(!f->irFty.arg_sret->rewrite &&
               "ABI shouldn't have to rewrite sret returns");
        DLValue returnValue(f->type->next, sretPointer);

        // try to construct the return value in-place
        const auto initialCleanupScope = funcGen.scopes.currentCleanupScope();
        const bool constructed = toInPlaceConstruction(&returnValue, stmt->exp);
        if (constructed) {
          // cleanup manually (otherwise done by toElemDtor())
          if (funcGen.scopes.currentCleanupScope() != initialCleanupScope) {
            auto endbb = irs->insertBB("inPlaceSretConstruct.success");
            funcGen.scopes.runCleanups(initialCleanupScope, endbb);
            funcGen.scopes.popCleanups(initialCleanupScope);
            irs->scope() = IRScope(endbb);
          }
        } else {
          DValue *e = toElemDtor(stmt->exp);

          // store the return value unless NRVO already used the sret pointer
          if (!e->isLVal() || DtoLVal(e) != sretPointer) {
            // call postblit if the expression is a D lvalue
            // exceptions: NRVO and special __result variable (out contracts)
            bool doPostblit = !(fd->nrvo_can && fd->nrvo_var);
            if (doPostblit && stmt->exp->op == TOKvar) {
              auto ve = static_cast<VarExp *>(stmt->exp);
              if (ve->var->isResult())
                doPostblit = false;
            }

            DtoAssign(stmt->loc, &returnValue, e, TOKblit);
            if (doPostblit)
              callPostblit(stmt->loc, stmt->exp, sretPointer);
          }
        }
      } else {
        // the return type is not void, so this is a normal "register" return
        if (!stmt->exp && irs->isMainFunc(f)) {
          returnValue =
              LLConstant::getNullValue(irs->mainFunc->getReturnType());
        } else {
          if (stmt->exp->op == TOKnull) {
            stmt->exp->type = f->type->next;
          }
          DValue *dval = nullptr;
          // call postblit if necessary
          if (!f->type->isref) {
            dval = toElemDtor(stmt->exp);
            LLValue *vthis =
                (DtoIsInMemoryOnly(dval->type) ? DtoLVal(dval) : DtoRVal(dval));
            callPostblit(stmt->loc, stmt->exp, vthis);
          } else {
            Expression *ae = stmt->exp;
            dval = toElemDtor(ae);
          }
          // do abi specific transformations on the return value
          returnValue = getIrFunc(fd)->irFty.putRet(dval);
        }

        // Hack around LDC assuming structs and static arrays are in memory:
        // If the function returns a struct or a static array, and the return
        // value is a pointer to a struct or a static array, load from it
        // before returning.
        if (returnValue->getType() != funcType->getReturnType() &&
            DtoIsInMemoryOnly(f->type->next) &&
            isaPointer(returnValue->getType())) {
          Logger::println("Loading value for return");
          returnValue = DtoLoad(returnValue);
        }

        // can happen for classes and void main
        if (returnValue->getType() != funcType->getReturnType()) {
          // for the main function this only happens if it is declared as void
          // and then contains a return (exp); statement. Since the actual
          // return type remains i32, we just throw away the exp value
          // and return 0 instead
          // if we're not in main, just bitcast
          if (irs->isMainFunc(f)) {
            returnValue =
                LLConstant::getNullValue(irs->mainFunc->getReturnType());
          } else {
            returnValue =
                irs->ir->CreateBitCast(returnValue, funcType->getReturnType());
          }

          IF_LOG Logger::cout() << "return value after cast: " << *returnValue
                                << '\n';
        }
      }
    } else {
      // no return value expression means it's a void function.
      assert(funcType->getReturnType() == LLType::getVoidTy(irs->context()));
    }

    // If there are no cleanups to run, we try to keep the IR simple and
    // just directly emit the return instruction. If there are cleanups to run
    // first, we need to store the return value to a stack slot, in which case
    // we can use a shared return bb for all these cases.
    const bool useRetValSlot = funcGen.scopes.currentCleanupScope() != 0;
    const bool sharedRetBlockExists = !!funcGen.retBlock;
    if (useRetValSlot) {
      if (!sharedRetBlockExists) {
        funcGen.retBlock = irs->insertBB("return");
        if (returnValue) {
          funcGen.retValSlot =
              DtoRawAlloca(returnValue->getType(), 0, "return.slot");
        }
      }

      // Create the store to the slot at the end of our current basic
      // block, before we run the cleanups.
      if (returnValue) {
        irs->ir->CreateStore(returnValue, funcGen.retValSlot);
      }

      // Now run the cleanups.
      funcGen.scopes.runCleanups(0, funcGen.retBlock);

      irs->scope() = IRScope(funcGen.retBlock);
    }

    // If we need to emit the actual return instruction, do so.
    if (!useRetValSlot || !sharedRetBlockExists) {
      if (returnValue) {
        // Hack: the frontend generates 'return 0;' as last statement of
        // 'void main()'. But the debug location is missing. Use the end
        // of function as debug location.
        if (fd->isMain() && !stmt->loc.linnum) {
          irs->DBuilder.EmitStopPoint(fd->endloc);
        }

        irs->ir->CreateRet(useRetValSlot ? DtoLoad(funcGen.retValSlot)
                                         : returnValue);
      } else {
        irs->ir->CreateRetVoid();
      }
    }

    // Finally, create a new predecessor-less dummy bb as the current IRScope
    // to make sure we do not emit any extra instructions after the terminating
    // instruction (ret or branch to return bb), which would be illegal IR.
    irs->scope() = IRScope(irs->insertBB("dummy.afterreturn"));
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ExpStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ExpStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    if (stmt->exp) {
      elem *e;
      // a cast(void) around the expression is allowed, but doesn't require any
      // code
      if (stmt->exp->op == TOKcast && stmt->exp->type == Type::tvoid) {
        CastExp *cexp = static_cast<CastExp *>(stmt->exp);
        e = toElemDtor(cexp->e1);
      } else {
        e = toElemDtor(stmt->exp);
      }
      delete e;
    }
  }

  //////////////////////////////////////////////////////////////////////////
  
  bool dcomputeReflectMatches(CallExp *ce) {
    auto arg1 = (DComputeTarget::ID)(*ce->arguments)[0]->toInteger();
    auto arg2 = (*ce->arguments)[1]->toInteger();
    auto dct = irs->dcomputetarget;
    if (!dct) {
      return arg1 == DComputeTarget::Host;
    }
    else {
      return arg1 == dct->target &&
             (!arg2 || arg2 == static_cast<dinteger_t>(dct->tversion));
    }
  }

  void visit(IfStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("IfStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);
    auto truecount = PGO.getRegionCount(stmt);
    auto elsecount = PGO.getCurrentRegionCount() - truecount;
    auto brweights = PGO.createProfileWeights(truecount, elsecount);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);
    emitCoverageLinecountInc(stmt->loc);

    // This is a (dirty) hack to get codegen time conditional
    // compilation, on account of the fact that we are trying
    // to target multiple backends "simultaneously" with one
    // pass through the front end, to have a single "static"
    // context.
    if (stmt->condition->op == TOKcall) {
      auto ce = (CallExp *)stmt->condition;
      if (ce->f && ce->f->ident == Id::dcReflect) {
        if (dcomputeReflectMatches(ce))
          stmt->ifbody->accept(this);
        else if (stmt->elsebody)
          stmt->elsebody->accept(this);
        return;
      }
    }

    DValue *cond_e = toElemDtor(stmt->condition);
    LLValue *cond_val = DtoRVal(cond_e);

    llvm::BasicBlock *ifbb = irs->insertBB("if");
    llvm::BasicBlock *endbb = irs->insertBBAfter(ifbb, "endif");
    llvm::BasicBlock *elsebb =
        stmt->elsebody ? irs->insertBBAfter(ifbb, "else") : endbb;

    if (cond_val->getType() != LLType::getInt1Ty(irs->context())) {
      IF_LOG Logger::cout() << "if conditional: " << *cond_val << '\n';
      cond_val = DtoRVal(DtoCast(stmt->loc, cond_e, Type::tbool));
    }
    auto brinstr =
        llvm::BranchInst::Create(ifbb, elsebb, cond_val, irs->scopebb());
    PGO.addBranchWeights(brinstr, brweights);

    // replace current scope
    irs->scope() = IRScope(ifbb);

    // do scoped statements

    if (stmt->ifbody) {
      irs->DBuilder.EmitBlockStart(stmt->ifbody->loc);
      PGO.emitCounterIncrement(stmt);
      stmt->ifbody->accept(this);
      irs->DBuilder.EmitBlockEnd();
    }
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(endbb, irs->scopebb());
    }

    if (stmt->elsebody) {
      irs->scope() = IRScope(elsebb);
      irs->DBuilder.EmitBlockStart(stmt->elsebody->loc);
      stmt->elsebody->accept(this);
      if (!irs->scopereturned()) {
        llvm::BranchInst::Create(endbb, irs->scopebb());
      }
      irs->DBuilder.EmitBlockEnd();
    }

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();

    // rewrite the scope
    irs->scope() = IRScope(endbb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ScopeStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ScopeStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    if (stmt->statement) {
      irs->DBuilder.EmitBlockStart(stmt->statement->loc);
      stmt->statement->accept(this);
      irs->DBuilder.EmitBlockEnd();
    }
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(WhileStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("WhileStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // create while blocks

    llvm::BasicBlock *whilebb = irs->insertBB("whilecond");
    llvm::BasicBlock *whilebodybb = irs->insertBBAfter(whilebb, "whilebody");
    llvm::BasicBlock *endbb = irs->insertBBAfter(whilebodybb, "endwhile");

    // move into the while block
    irs->ir->CreateBr(whilebb);

    // replace current scope
    irs->scope() = IRScope(whilebb);

    // create the condition
    emitCoverageLinecountInc(stmt->condition->loc);
    DValue *cond_e = toElemDtor(stmt->condition);
    LLValue *cond_val = DtoRVal(DtoCast(stmt->loc, cond_e, Type::tbool));
    delete cond_e;

    // conditional branch
    auto branchinst =
        llvm::BranchInst::Create(whilebodybb, endbb, cond_val, irs->scopebb());
    {
      auto loopcount = PGO.getRegionCount(stmt);
      auto brweights =
          PGO.createProfileWeightsWhileLoop(stmt->condition, loopcount);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // rewrite scope
    irs->scope() = IRScope(whilebodybb);

    // while body code
    irs->funcGen().jumpTargets.pushLoopTarget(stmt, whilebb, endbb);
    PGO.emitCounterIncrement(stmt);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }
    irs->funcGen().jumpTargets.popLoopTarget();

    // loop
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(whilebb, irs->scopebb());
    }

    // rewrite the scope
    irs->scope() = IRScope(endbb);

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(DoStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("DoStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    auto entryCount = PGO.setCurrentStmt(stmt);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // create while blocks
    llvm::BasicBlock *dowhilebb = irs->insertBB("dowhile");
    llvm::BasicBlock *condbb = irs->insertBBAfter(dowhilebb, "dowhilecond");
    llvm::BasicBlock *endbb = irs->insertBBAfter(condbb, "enddowhile");

    // move into the while block
    assert(!irs->scopereturned());
    llvm::BranchInst::Create(dowhilebb, irs->scopebb());

    // replace current scope
    irs->scope() = IRScope(dowhilebb);

    // do-while body code
    irs->funcGen().jumpTargets.pushLoopTarget(stmt, condbb, endbb);
    PGO.emitCounterIncrement(stmt);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }
    irs->funcGen().jumpTargets.popLoopTarget();

    // branch to condition block
    llvm::BranchInst::Create(condbb, irs->scopebb());
    irs->scope() = IRScope(condbb);

    // create the condition
    emitCoverageLinecountInc(stmt->condition->loc);
    DValue *cond_e = toElemDtor(stmt->condition);
    LLValue *cond_val = DtoRVal(DtoCast(stmt->loc, cond_e, Type::tbool));
    delete cond_e;

    // conditional branch
    auto branchinst =
        llvm::BranchInst::Create(dowhilebb, endbb, cond_val, irs->scopebb());
    {
      // The region counter includes fallthrough from the previous statement.
      // Subtract parent count to get the true branch count of the loop
      // conditional.
      auto loopcount = PGO.getRegionCount(stmt) - entryCount;
      auto brweights =
          PGO.createProfileWeightsWhileLoop(stmt->condition, loopcount);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // rewrite the scope
    irs->scope() = IRScope(endbb);

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ForStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ForStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // start new dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // create for blocks
    llvm::BasicBlock *forbb = irs->insertBB("forcond");
    llvm::BasicBlock *forbodybb = irs->insertBBAfter(forbb, "forbody");
    llvm::BasicBlock *forincbb = irs->insertBBAfter(forbodybb, "forinc");
    llvm::BasicBlock *endbb = irs->insertBBAfter(forincbb, "endfor");

    // init
    if (stmt->_init != nullptr) {
      stmt->_init->accept(this);
    }

    // move into the for condition block, ie. start the loop
    assert(!irs->scopereturned());
    llvm::BranchInst::Create(forbb, irs->scopebb());

    // In case of loops that have been rewritten to a composite statement
    // containing the initializers and then the actual loop, we need to
    // register the former as target scope start.
    Statement *scopeStart = stmt->getRelatedLabeled();
    while (ScopeStatement *scope = scopeStart->isScopeStatement()) {
      scopeStart = scope->statement;
    }
    irs->funcGen().jumpTargets.pushLoopTarget(scopeStart, forincbb, endbb);

    // replace current scope
    irs->scope() = IRScope(forbb);

    // create the condition
    llvm::Value *cond_val;
    if (stmt->condition) {
      emitCoverageLinecountInc(stmt->condition->loc);
      DValue *cond_e = toElemDtor(stmt->condition);
      cond_val = DtoRVal(DtoCast(stmt->loc, cond_e, Type::tbool));
      delete cond_e;
    } else {
      cond_val = DtoConstBool(true);
    }

    // conditional branch
    assert(!irs->scopereturned());
    auto branchinst =
        llvm::BranchInst::Create(forbodybb, endbb, cond_val, irs->scopebb());
    {
      auto brweights = PGO.createProfileWeightsForLoop(stmt);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // rewrite scope
    irs->scope() = IRScope(forbodybb);

    // do for body code
    PGO.emitCounterIncrement(stmt);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }

    // move into the for increment block
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(forincbb, irs->scopebb());
    }
    irs->scope() = IRScope(forincbb);

    // increment
    if (stmt->increment) {
      emitCoverageLinecountInc(stmt->increment->loc);
      DValue *inc = toElemDtor(stmt->increment);
      delete inc;
    }

    // loop
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(forbb, irs->scopebb());
    }

    irs->funcGen().jumpTargets.popLoopTarget();

    // rewrite the scope
    irs->scope() = IRScope(endbb);

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(BreakStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("BreakStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // don't emit two terminators in a row
    // happens just before DMD generated default statements if the last case
    // terminates
    if (irs->scopereturned()) {
      return;
    }

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    if (stmt->ident) {
      IF_LOG Logger::println("ident = %s", stmt->ident->toChars());

      // Get the loop or break statement the label refers to
      Statement *targetStatement = stmt->target->statement;
      ScopeStatement *tmp;
      while ((tmp = targetStatement->isScopeStatement())) {
        targetStatement = tmp->statement;
      }

      irs->funcGen().jumpTargets.breakToStatement(targetStatement);
    } else {
      irs->funcGen().jumpTargets.breakToClosest();
    }

    // the break terminated this basicblock, start a new one
    llvm::BasicBlock *bb = irs->insertBB("afterbreak");
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ContinueStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ContinueStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    if (stmt->ident) {
      IF_LOG Logger::println("ident = %s", stmt->ident->toChars());

      // get the loop statement the label refers to
      Statement *targetLoopStatement = stmt->target->statement;
      ScopeStatement *tmp;
      while ((tmp = targetLoopStatement->isScopeStatement())) {
        targetLoopStatement = tmp->statement;
      }

      irs->funcGen().jumpTargets.continueWithLoop(targetLoopStatement);
    } else {
      irs->funcGen().jumpTargets.continueWithClosest();
    }

    // the continue terminated this basicblock, start a new one
    llvm::BasicBlock *bb = irs->insertBB("aftercontinue");
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(OnScopeStatement *stmt) LLVM_OVERRIDE {
    stmt->error("Internal Compiler Error: OnScopeStatement should have been "
                "lowered by frontend.");
    fatal();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(TryFinallyStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("TryFinallyStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    /*auto entryCount = */ PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    // We only need to consider exception handling/cleanup issues if there
    // is both a try and a finally block. If not, just directly emit what
    // is present.
    if (!stmt->_body || !stmt->finalbody) {
      if (stmt->_body) {
        irs->DBuilder.EmitBlockStart(stmt->_body->loc);
        stmt->_body->accept(this);
        irs->DBuilder.EmitBlockEnd();
      } else if (stmt->finalbody) {
        irs->DBuilder.EmitBlockStart(stmt->finalbody->loc);
        stmt->finalbody->accept(this);
        irs->DBuilder.EmitBlockEnd();
      }
      return;
    }

    // We'll append the "try" part to the current basic block later. No need
    // for an extra one (we'd need to branch to it unconditionally anyway).
    llvm::BasicBlock *trybb = irs->scopebb();

    llvm::BasicBlock *finallybb = irs->insertBB("finally");
    // Create a block to branch to after successfully running the try block
    // and any cleanups.
    llvm::BasicBlock *successbb =
        irs->scopereturned() ? nullptr
                             : irs->insertBBAfter(finallybb, "try.success");

    // Emit the finally block and set up the cleanup scope for it.
    irs->scope() = IRScope(finallybb);
    irs->DBuilder.EmitBlockStart(stmt->finalbody->loc);
    stmt->finalbody->accept(this);
    irs->DBuilder.EmitBlockEnd();
    CleanupCursor cleanupBefore;

    // For @compute code, don't emit any exception handling as there are no
    // exceptions anyway.
    const bool computeCode = !!irs->dcomputetarget;
    if (!computeCode) {
      cleanupBefore = irs->funcGen().scopes.currentCleanupScope();
      irs->funcGen().scopes.pushCleanup(finallybb, irs->scopebb());
    }
    // Emit the try block.
    irs->scope() = IRScope(trybb);

    assert(stmt->_body);
    irs->DBuilder.EmitBlockStart(stmt->_body->loc);
    stmt->_body->accept(this);
    irs->DBuilder.EmitBlockEnd();

    if (successbb) {
      if (!computeCode)
        irs->funcGen().scopes.runCleanups(cleanupBefore, successbb);
      irs->scope() = IRScope(successbb);
      // PGO counter tracks the continuation of the try-finally statement
      PGO.emitCounterIncrement(stmt);
    }
    if (!computeCode)
      irs->funcGen().scopes.popCleanups(cleanupBefore);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(TryCatchStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("TryCatchStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;
    assert(!irs->dcomputetarget);

    auto &PGO = irs->funcGen().pgo;

    // Emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    // We'll append the "try" part to the current basic block later. No need
    // for an extra one (we'd need to branch to it unconditionally anyway).
    llvm::BasicBlock *trybb = irs->scopebb();

    // Create a basic block to branch to after leaving the try or an
    // associated catch block successfully.
    llvm::BasicBlock *endbb = irs->insertBB("try.success.or.caught");

    irs->funcGen().scopes.pushTryCatch(stmt, endbb);

    // Emit the try block.
    irs->scope() = IRScope(trybb);

    assert(stmt->_body);
    irs->DBuilder.EmitBlockStart(stmt->_body->loc);
    stmt->_body->accept(this);
    irs->DBuilder.EmitBlockEnd();

    if (!irs->scopereturned())
      llvm::BranchInst::Create(endbb, irs->scopebb());

    irs->funcGen().scopes.popTryCatch();

    irs->scope() = IRScope(endbb);

    // PGO counter tracks the continuation of the try statement
    PGO.emitCounterIncrement(stmt);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ThrowStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ThrowStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;
    assert(!irs->dcomputetarget);

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    assert(stmt->exp);
    DValue *e = toElemDtor(stmt->exp);

    llvm::Function *fn =
        getRuntimeFunction(stmt->loc, irs->module, "_d_throw_exception");
    LLValue *arg =
        DtoBitCast(DtoRVal(e), fn->getFunctionType()->getParamType(0));

    irs->CreateCallOrInvoke(fn, arg);
    irs->ir->CreateUnreachable();

    // TODO: Should not be needed.
    llvm::BasicBlock *bb = irs->insertBB("afterthrow");
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(SwitchStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("SwitchStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &funcGen = irs->funcGen();

    auto &PGO = funcGen.pgo;
    PGO.setCurrentStmt(stmt);
    const auto incomingPGORegionCount = PGO.getCurrentRegionCount();

    irs->DBuilder.EmitStopPoint(stmt->loc);
    emitCoverageLinecountInc(stmt->loc);
    llvm::BasicBlock *const oldbb = irs->scopebb();

    // The cases of the switch statement, in codegen order. For string switches,
    // we reorder them lexicographically later to match what the _d_switch_*
    // druntime dispatch functions expect.
    auto cases = stmt->cases;
    const auto caseCount = cases->dim;

    // llvm::Values for the case indices. Might not be llvm::Constants for
    // runtime-initialised immutable globals as case indices, in which case we
    // need to emit a `br` chain instead of `switch`.
    llvm::SmallVector<llvm::Value *, 16> indices;
    indices.reserve(caseCount);
    bool useSwitchInst = true;

    // For string switches, sort the cases and emit the table data.
    llvm::Value *stringTableSlice = nullptr;
    const bool isStringSwitch = !stmt->condition->type->isintegral();
    if (isStringSwitch) {
      Logger::println("is string switch");
      assert(!irs->dcomputetarget);

      // Sort the cases, taking care not to modify the original AST.
      cases = cases->copy();
      std::sort(cases->begin(), cases->end(), compareCaseStrings);

      // Emit constants for the case values.
      llvm::SmallVector<llvm::Constant *, 16> stringConsts;
      stringConsts.reserve(caseCount);
      for (size_t i = 0; i < caseCount; ++i) {
        stringConsts.push_back(toConstElem((*cases)[i]->exp, irs));
        indices.push_back(DtoConstUint(i));
      }

      // Create internal global with the data table.
      const auto elemTy = DtoType(stmt->condition->type);
      const auto arrTy = llvm::ArrayType::get(elemTy, stringConsts.size());
      const auto arrInit = LLConstantArray::get(arrTy, stringConsts);
      const auto arr = new llvm::GlobalVariable(
          irs->module, arrTy, true, llvm::GlobalValue::InternalLinkage, arrInit,
          ".string_switch_table_data");

      // Create D slice to pass to runtime later.
      const auto arrPtr =
          llvm::ConstantExpr::getBitCast(arr, getPtrToType(elemTy));
      const auto arrLen = DtoConstSize_t(stringConsts.size());
      stringTableSlice = DtoConstSlice(arrLen, arrPtr);
    } else {
      for (auto cs : *cases) {
        if (cs->exp->op == TOKvar) {
          const auto vd =
              static_cast<VarExp *>(cs->exp)->var->isVarDeclaration();
          if (vd && (!vd->_init || !vd->isConst())) {
            indices.push_back(DtoRVal(toElemDtor(cs->exp)));
            useSwitchInst = false;
            continue;
          }
        }
        indices.push_back(toConstElem(cs->exp, irs));
      }
    }
    assert(indices.size() == caseCount);

    // body block.
    // FIXME: that block is never used
    llvm::BasicBlock *bodybb = irs->insertBB("switchbody");

    // end (break point)
    llvm::BasicBlock *endbb = irs->insertBBAfter(bodybb, "switchend");

    // default
    auto defaultTargetBB = endbb;
    if (stmt->sdefault) {
      Logger::println("has default");
      defaultTargetBB =
          funcGen.switchTargets.getOrCreate(stmt->sdefault, "default", *irs);
    }

    // do switch body
    assert(stmt->_body);
    irs->scope() = IRScope(bodybb);
    funcGen.jumpTargets.pushBreakTarget(stmt, endbb);
    stmt->_body->accept(this);
    funcGen.jumpTargets.popBreakTarget();
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(endbb, irs->scopebb());
    }

    irs->scope() = IRScope(oldbb);
    if (useSwitchInst) {
      // The case index value.
      LLValue *condVal;
      if (isStringSwitch) {
        condVal = call_string_switch_runtime(stringTableSlice, stmt->condition);
      } else {
        condVal = DtoRVal(toElemDtor(stmt->condition));
      }

      // Create switch and add the cases.
      // For PGO instrumentation, we need to add counters /before/ the case
      // statement bodies, because the counters should only count the jumps
      // directly from the switch statement and not "goto default", etc.
      llvm::SwitchInst *si;
      if (!PGO.emitsInstrumentation()) {
        si = llvm::SwitchInst::Create(condVal, defaultTargetBB, caseCount,
                                      irs->scopebb());
        for (size_t i = 0; i < caseCount; ++i) {
          si->addCase(isaConstantInt(indices[i]),
                      funcGen.switchTargets.get((*cases)[i]));
        }
      } else {
        auto switchbb = irs->scopebb();
        // Add PGO instrumentation.
        // Create "default" counter bb.
        {
          llvm::BasicBlock *defaultcntr =
              irs->insertBBBefore(defaultTargetBB, "defaultcntr");
          irs->scope() = IRScope(defaultcntr);
          PGO.emitCounterIncrement(stmt->sdefault);
          llvm::BranchInst::Create(defaultTargetBB, defaultcntr);
          // Create switch
          si = llvm::SwitchInst::Create(condVal, defaultcntr, caseCount,
                                        switchbb);
        }

        // Create and add case counter bbs.
        for (size_t i = 0; i < caseCount; ++i) {
          const auto cs = (*cases)[i];
          const auto body = funcGen.switchTargets.get(cs);

          auto casecntr = irs->insertBBBefore(body, "casecntr");
          irs->scope() = IRScope(casecntr);
          PGO.emitCounterIncrement(cs);
          llvm::BranchInst::Create(body, casecntr);
          si->addCase(isaConstantInt(indices[i]), casecntr);
        }
      }

      // Apply PGO switch branch weights:
      {
        // Get case statements execution counts from profile data.
        std::vector<uint64_t> case_prof_counts;
        case_prof_counts.push_back(
            stmt->sdefault ? PGO.getRegionCount(stmt->sdefault) : 0);
        for (auto cs : *cases) {
          auto w = PGO.getRegionCount(cs);
          case_prof_counts.push_back(w);
        }

        auto brweights = PGO.createProfileWeights(case_prof_counts);
        PGO.addBranchWeights(si, brweights);
      }
    } else {
      // We can't use switch, so we will use a bunch of br instructions
      // instead.

      DValue *cond = toElemDtor(stmt->condition);
      LLValue *condVal = DtoRVal(cond);

      llvm::BasicBlock *nextbb = irs->insertBBBefore(endbb, "checkcase");
      llvm::BranchInst::Create(nextbb, irs->scopebb());

      if (PGO.emitsInstrumentation()) {
        // Prepend extra BB to "default:" to increment profiling counter.
        llvm::BasicBlock *defaultcntr =
            irs->insertBBBefore(defaultTargetBB, "defaultcntr");
        irs->scope() = IRScope(defaultcntr);
        PGO.emitCounterIncrement(stmt->sdefault);
        llvm::BranchInst::Create(defaultTargetBB, defaultcntr);
        defaultTargetBB = defaultcntr;
      }

      irs->scope() = IRScope(nextbb);
      auto failedCompareCount = incomingPGORegionCount;
      for (size_t i = 0; i < caseCount; ++i) {
        LLValue *cmp = irs->ir->CreateICmp(llvm::ICmpInst::ICMP_EQ, indices[i],
                                           condVal, "checkcase");
        nextbb = irs->insertBBBefore(endbb, "checkcase");

        // Add case counters for PGO in front of case body
        const auto cs = (*cases)[i];
        auto casejumptargetbb = funcGen.switchTargets.get(cs);
        if (PGO.emitsInstrumentation()) {
          llvm::BasicBlock *casecntr =
              irs->insertBBBefore(casejumptargetbb, "casecntr");
          auto savedbb = irs->scope();
          irs->scope() = IRScope(casecntr);
          PGO.emitCounterIncrement(cs);
          llvm::BranchInst::Create(casejumptargetbb, casecntr);
          irs->scope() = savedbb;

          casejumptargetbb = casecntr;
        }

        // Create the comparison branch for this case
        auto branchinst = llvm::BranchInst::Create(casejumptargetbb, nextbb,
                                                   cmp, irs->scopebb());

        // Calculate and apply PGO branch weights
        {
          auto trueCount = PGO.getRegionCount(cs);
          assert(trueCount <= failedCompareCount &&
                 "Higher branch count than switch incoming count!");
          failedCompareCount -= trueCount;
          auto brweights =
              PGO.createProfileWeights(trueCount, failedCompareCount);
          PGO.addBranchWeights(branchinst, brweights);
        }

        irs->scope() = IRScope(nextbb);
      }

      llvm::BranchInst::Create(defaultTargetBB, irs->scopebb());
    }

    irs->scope() = IRScope(endbb);
    // PGO counter tracks exit point of switch statement:
    PGO.emitCounterIncrement(stmt);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(CaseStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("CaseStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &funcGen = irs->funcGen();
    auto &PGO = funcGen.pgo;
    PGO.setCurrentStmt(stmt);

    const auto body = funcGen.switchTargets.getOrCreate(stmt, "case", *irs);
    // The BB may have already been created by a `goto case` statement.
    // Move it after the current scope BB for lexical order.
    body->moveAfter(irs->scopebb());

    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(body, irs->scopebb());
    }

    irs->scope() = IRScope(body);

    assert(stmt->statement);
    irs->DBuilder.EmitBlockStart(stmt->statement->loc);
    emitCoverageLinecountInc(stmt->loc);
    if (stmt->gototarget) {
      PGO.emitCounterIncrement(PGO.getCounterPtr(stmt, 1));
    }
    stmt->statement->accept(this);
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(DefaultStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("DefaultStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &funcGen = irs->funcGen();
    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    const auto body = funcGen.switchTargets.getOrCreate(stmt, "default", *irs);
    // The BB may have already been created.
    // Move it after the current scope BB for lexical order.
    body->moveAfter(irs->scopebb());

    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(body, irs->scopebb());
    }

    irs->scope() = IRScope(body);

    assert(stmt->statement);
    irs->DBuilder.EmitBlockStart(stmt->statement->loc);
    emitCoverageLinecountInc(stmt->loc);
    if (stmt->gototarget) {
      PGO.emitCounterIncrement(PGO.getCounterPtr(stmt, 1));
    }
    stmt->statement->accept(this);
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(UnrolledLoopStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("UnrolledLoopStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // if no statements, there's nothing to do
    if (!stmt->statements || !stmt->statements->dim) {
      return;
    }

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // DMD doesn't fold stuff like continue/break, and since this isn't really a
    // loop we have to keep track of each statement and jump to the next/end
    // on continue/break

    // create end block
    llvm::BasicBlock *endbb = irs->insertBB("unrolledend");

    // create a block for each statement
    size_t nstmt = stmt->statements->dim;
    llvm::SmallVector<llvm::BasicBlock *, 4> blocks(nstmt, nullptr);
    for (size_t i = 0; i < nstmt; i++)
      blocks[i] = irs->insertBBBefore(endbb, "unrolledstmt");

    // enter first stmt
    if (!irs->scopereturned()) {
      irs->ir->CreateBr(blocks[0]);
    }

    // do statements
    Statement **stmts = stmt->statements->data;

    for (size_t i = 0; i < nstmt; i++) {
      Statement *s = stmts[i];

      // get blocks
      llvm::BasicBlock *thisbb = blocks[i];
      llvm::BasicBlock *nextbb = (i + 1 == nstmt) ? endbb : blocks[i + 1];

      // update scope
      irs->scope() = IRScope(thisbb);

      // push loop scope
      // continue goes to next statement, break goes to end
      irs->funcGen().jumpTargets.pushLoopTarget(stmt, nextbb, endbb);

      // do statement
      s->accept(this);

      // pop loop scope
      irs->funcGen().jumpTargets.popLoopTarget();

      // next stmt
      if (!irs->scopereturned()) {
        irs->ir->CreateBr(nextbb);
      }
    }

    irs->scope() = IRScope(endbb);

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ForeachStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ForeachStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // assert(arguments->dim == 1);
    assert(stmt->value != 0);
    assert(stmt->aggr != 0);
    assert(stmt->func != 0);

    // Argument* arg = static_cast<Argument*>(arguments->data[0]);
    // Logger::println("Argument is %s", arg->toChars());

    IF_LOG Logger::println("aggr = %s", stmt->aggr->toChars());

    // key
    LLType *keytype = stmt->key ? DtoType(stmt->key->type) : DtoSize_t();
    LLValue *keyvar;
    if (stmt->key) {
      keyvar = DtoRawVarDeclaration(stmt->key);
    } else {
      keyvar = DtoRawAlloca(keytype, 0, "foreachkey");
    }
    LLValue *zerokey = LLConstantInt::get(keytype, 0, false);

    // value
    IF_LOG Logger::println("value = %s", stmt->value->toPrettyChars());
    LLValue *valvar = nullptr;
    if (!stmt->value->isRef() && !stmt->value->isOut()) {
      // Create a local variable to serve as the value.
      DtoRawVarDeclaration(stmt->value);
      valvar = getIrLocal(stmt->value)->value;
    }

    // what to iterate
    DValue *aggrval = toElemDtor(stmt->aggr);

    // get length and pointer
    LLValue *niters = DtoArrayLen(aggrval);
    LLValue *val = DtoArrayPtr(aggrval);

    if (niters->getType() != keytype) {
      size_t sz1 = getTypeBitSize(niters->getType());
      size_t sz2 = getTypeBitSize(keytype);
      if (sz1 < sz2) {
        niters = irs->ir->CreateZExt(niters, keytype, "foreachtrunckey");
      } else if (sz1 > sz2) {
        niters = irs->ir->CreateTrunc(niters, keytype, "foreachtrunckey");
      } else {
        niters = irs->ir->CreateBitCast(niters, keytype, "foreachtrunckey");
      }
    }

    if (stmt->op == TOKforeach) {
      new llvm::StoreInst(zerokey, keyvar, irs->scopebb());
    } else {
      new llvm::StoreInst(niters, keyvar, irs->scopebb());
    }

    llvm::BasicBlock *condbb = irs->insertBB("foreachcond");
    llvm::BasicBlock *bodybb = irs->insertBBAfter(condbb, "foreachbody");
    llvm::BasicBlock *nextbb = irs->insertBBAfter(bodybb, "foreachnext");
    llvm::BasicBlock *endbb = irs->insertBBAfter(nextbb, "foreachend");

    llvm::BranchInst::Create(condbb, irs->scopebb());

    // condition
    irs->scope() = IRScope(condbb);

    LLValue *done = nullptr;
    LLValue *load = DtoLoad(keyvar);
    if (stmt->op == TOKforeach) {
      done = irs->ir->CreateICmpULT(load, niters);
    } else if (stmt->op == TOKforeach_reverse) {
      done = irs->ir->CreateICmpUGT(load, zerokey);
      load = irs->ir->CreateSub(load, LLConstantInt::get(keytype, 1, false));
      DtoStore(load, keyvar);
    }
    auto branchinst =
        llvm::BranchInst::Create(bodybb, endbb, done, irs->scopebb());
    {
      auto brweights = PGO.createProfileWeightsForeach(stmt);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // init body
    irs->scope() = IRScope(bodybb);
    PGO.emitCounterIncrement(stmt);

    // get value for this iteration
    LLValue *loadedKey = irs->ir->CreateLoad(keyvar);
    LLValue *gep = DtoGEP1(val, loadedKey, true);

    if (!stmt->value->isRef() && !stmt->value->isOut()) {
      // Copy value to local variable, and use it as the value variable.
      DLValue dst(stmt->value->type, valvar);
      DLValue src(stmt->value->type, gep);
      DtoAssign(stmt->loc, &dst, &src, TOKassign);
      getIrLocal(stmt->value)->value = valvar;
    } else {
      // Use the GEP as the address of the value variable.
      DtoRawVarDeclaration(stmt->value, gep);
    }

    // emit body
    irs->funcGen().jumpTargets.pushLoopTarget(stmt, nextbb, endbb);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }
    irs->funcGen().jumpTargets.popLoopTarget();

    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(nextbb, irs->scopebb());
    }

    // next
    irs->scope() = IRScope(nextbb);
    if (stmt->op == TOKforeach) {
      LLValue *load = DtoLoad(keyvar);
      load = irs->ir->CreateAdd(load, LLConstantInt::get(keytype, 1, false));
      DtoStore(load, keyvar);
    }
    llvm::BranchInst::Create(condbb, irs->scopebb());

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();

    // end
    irs->scope() = IRScope(endbb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ForeachRangeStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ForeachRangeStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // evaluate lwr/upr
    assert(stmt->lwr->type->isintegral());
    LLValue *lower = DtoRVal(toElemDtor(stmt->lwr));
    assert(stmt->upr->type->isintegral());
    LLValue *upper = DtoRVal(toElemDtor(stmt->upr));

    // handle key
    assert(stmt->key->type->isintegral());
    LLValue *keyval = DtoRawVarDeclaration(stmt->key);

    // store initial value in key
    if (stmt->op == TOKforeach) {
      DtoStore(lower, keyval);
    } else {
      DtoStore(upper, keyval);
    }

    // set up the block we'll need
    llvm::BasicBlock *condbb = irs->insertBB("foreachrange_cond");
    llvm::BasicBlock *bodybb = irs->insertBBAfter(condbb, "foreachrange_body");
    llvm::BasicBlock *nextbb = irs->insertBBAfter(bodybb, "foreachrange_next");
    llvm::BasicBlock *endbb = irs->insertBBAfter(nextbb, "foreachrange_end");

    // jump to condition
    llvm::BranchInst::Create(condbb, irs->scopebb());

    // CONDITION
    irs->scope() = IRScope(condbb);

    // first we test that lwr < upr
    lower = DtoLoad(keyval);
    assert(lower->getType() == upper->getType());
    llvm::ICmpInst::Predicate cmpop;
    if (isLLVMUnsigned(stmt->key->type)) {
      cmpop = (stmt->op == TOKforeach) ? llvm::ICmpInst::ICMP_ULT
                                       : llvm::ICmpInst::ICMP_UGT;
    } else {
      cmpop = (stmt->op == TOKforeach) ? llvm::ICmpInst::ICMP_SLT
                                       : llvm::ICmpInst::ICMP_SGT;
    }
    LLValue *cond = irs->ir->CreateICmp(cmpop, lower, upper);

    // jump to the body if range is ok, to the end if not
    auto branchinst =
        llvm::BranchInst::Create(bodybb, endbb, cond, irs->scopebb());
    {
      auto brweights = PGO.createProfileWeightsForeachRange(stmt);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // BODY
    irs->scope() = IRScope(bodybb);
    PGO.emitCounterIncrement(stmt);

    // reverse foreach decrements here
    if (stmt->op == TOKforeach_reverse) {
      LLValue *v = DtoLoad(keyval);
      LLValue *one = LLConstantInt::get(v->getType(), 1, false);
      v = irs->ir->CreateSub(v, one);
      DtoStore(v, keyval);
    }

    // emit body
    irs->funcGen().jumpTargets.pushLoopTarget(stmt, nextbb, endbb);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }
    irs->funcGen().jumpTargets.popLoopTarget();

    // jump to next iteration
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(nextbb, irs->scopebb());
    }

    // NEXT
    irs->scope() = IRScope(nextbb);

    // forward foreach increments here
    if (stmt->op == TOKforeach) {
      LLValue *v = DtoLoad(keyval);
      LLValue *one = LLConstantInt::get(v->getType(), 1, false);
      v = irs->ir->CreateAdd(v, one);
      DtoStore(v, keyval);
    }

    // jump to condition
    llvm::BranchInst::Create(condbb, irs->scopebb());

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();

    // END
    irs->scope() = IRScope(endbb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(LabelStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("LabelStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    // if it's an inline asm label, we don't create a basicblock, just emit it
    // in the asm
    if (irs->asmBlock) {
      auto a = new IRAsmStmt;
      std::stringstream label;
      printLabelName(label, mangleExact(irs->func()->decl),
                     stmt->ident->toChars());
      label << ":";
      a->code = label.str();
      irs->asmBlock->s.push_back(a);
      irs->asmBlock->internalLabels.push_back(stmt->ident);

      // disable inlining
      irs->func()->setNeverInline();
    } else {
      llvm::BasicBlock *labelBB =
          irs->insertBB(llvm::Twine("label.") + stmt->ident->toChars());
      irs->funcGen().jumpTargets.addLabelTarget(stmt->ident, labelBB);

      if (!irs->scopereturned()) {
        llvm::BranchInst::Create(labelBB, irs->scopebb());
      }

      irs->scope() = IRScope(labelBB);
    }

    PGO.emitCounterIncrement(stmt);
    // statement == nullptr when the label is at the end of function
    if (stmt->statement) {
      stmt->statement->accept(this);
    }
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(GotoStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("GotoStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    DtoGoto(stmt->loc, stmt->label);

    // TODO: Should not be needed.
    llvm::BasicBlock *bb = irs->insertBB("aftergoto");
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(GotoDefaultStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("GotoDefaultStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &funcGen = irs->funcGen();
    auto &PGO = funcGen.pgo;
    PGO.setCurrentStmt(stmt);

    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    assert(!irs->scopereturned());

    const auto defaultBB = funcGen.switchTargets.get(stmt->sw->sdefault);
    llvm::BranchInst::Create(defaultBB, irs->scopebb());

    // TODO: Should not be needed.
    llvm::BasicBlock *bb = irs->insertBB("aftergotodefault");
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(GotoCaseStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("GotoCaseStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &funcGen = irs->funcGen();
    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    assert(!irs->scopereturned());

    const auto caseBB =
        funcGen.switchTargets.getOrCreate(stmt->cs, "goto_case", *irs);
    llvm::BranchInst::Create(caseBB, irs->scopebb());

    // TODO: Should not be needed.
    llvm::BasicBlock *bb = irs->insertBB("aftergotocase");
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(WithStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("WithStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    irs->DBuilder.EmitBlockStart(stmt->loc);

    assert(stmt->exp);

    // with(..) can either be used with expressions or with symbols
    // wthis == null indicates the symbol form
    if (stmt->wthis) {
      LLValue *mem = DtoRawVarDeclaration(stmt->wthis);
      DValue *e = toElemDtor(stmt->exp);
      LLValue *val = (DtoIsInMemoryOnly(e->type) ? DtoLVal(e) : DtoRVal(e));
      DtoStore(val, mem);
    }

    if (stmt->_body) {
      stmt->_body->accept(this);
    }

    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(SwitchErrorStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("SwitchErrorStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;
    assert(!irs->dcomputetarget);
      
    auto &PGO = irs->funcGen().pgo;
    PGO.setCurrentStmt(stmt);

    llvm::Function *fn =
        getRuntimeFunction(stmt->loc, irs->module, "_d_switch_error");

    LLValue *moduleInfoSymbol =
        getIrModule(irs->func()->decl->getModule())->moduleInfoSymbol();
    LLType *moduleInfoType = DtoType(Module::moduleinfo->type);

    LLCallSite call = irs->CreateCallOrInvoke(
        fn, DtoBitCast(moduleInfoSymbol, getPtrToType(moduleInfoType)),
        DtoConstUint(stmt->loc.linnum));
    call.setDoesNotReturn();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(AsmStatement *stmt) LLVM_OVERRIDE {
    assert(!irs->dcomputetarget);
    AsmStatement_toIR(stmt, irs);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(CompoundAsmStatement *stmt) LLVM_OVERRIDE {
    assert(!irs->dcomputetarget);
    CompoundAsmStatement_toIR(stmt, irs);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ImportStatement *stmt) LLVM_OVERRIDE {
    // Empty.
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(Statement *stmt) LLVM_OVERRIDE {
    error(stmt->loc, "Statement type Statement not implemented: %s",
          stmt->toChars());
    fatal();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(PragmaStatement *stmt) LLVM_OVERRIDE {
    error(stmt->loc, "Statement type PragmaStatement not implemented: %s",
          stmt->toChars());
    fatal();
  }
};

//////////////////////////////////////////////////////////////////////////////

void Statement_toIR(Statement *s, IRState *irs) {
  ToIRVisitor v(irs);
  s->accept(&v);
}
