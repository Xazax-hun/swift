#include "swift/AST/ASTContext.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Type.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILDebugVariable.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "clang/AST/Decl.h"
#include "clang/Analysis/Analyses/LifetimeSafety.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/ArrayRef.h"
#include <cstdint>
#include <optional>
#include <souffle/SouffleInterface.h>
#include <unordered_map>

#define DEBUG_TYPE "datalog-diagnostics"

using namespace swift;

static clang::SourceLocation getLocation(const clang::lifetimes::internal::Origin& origin) {
  if (const auto* decl = origin.getDecl()) {
    return decl->getLocation();
  }
  return origin.getExpr()->getExprLoc();
}

static bool isTrackedType(SILType type) {
  auto astType = type.getASTType();
  return astType->isUnsafeMutablePointer() || astType->isUnsafePointer();
}

struct OriginAndLoan {
  unsigned origin, loan;
};

struct CalleeInfo {
  std::map<unsigned, OriginAndLoan> paramIdxToOriginAndLoan;
  std::vector<unsigned> returnOrigins;
};

struct CallerInfo {
  SILFunction &caller;
  SILFunction &callee;
  unsigned blockID;
  std::map<unsigned, unsigned> argIdxToOrigin;
  std::optional<OriginAndLoan> returnOriginAndLoan;
};

class DatalogDiagnosticsPass : public SILModuleTransform {
public:
  DatalogDiagnosticsPass() {}

private:
  souffle::Relation *issueRelation = nullptr;
  souffle::Relation *expireRelation = nullptr;
  souffle::Relation *assignRelation = nullptr;
  souffle::Relation *returnRelation = nullptr;
  souffle::Relation *useRelation = nullptr;
  souffle::Relation *edgeRelation = nullptr;
  souffle::Relation *deallocateRelation = nullptr;
  souffle::Relation *passRelation = nullptr;
  std::unordered_map<uint32_t, SourceLoc> ClangLocToSwiftLoc;
  std::vector<SourceLoc> SwiftLocs;
  std::vector<SILValue> origins;
  std::vector<CallerInfo> callers;
  std::map<SILFunction *, CalleeInfo> callees;

  unsigned getNextSwiftLoanID() {
    static unsigned nextLoan = 0;
    return nextLoan++;
  }

  unsigned getNextSwiftOriginID(SILValue value) {
    origins.push_back(value);
    return origins.size() - 1;
  }

  std::optional<unsigned> getOriginID(SILValue value) {
    unsigned originID = 0;
    for (auto origin : origins) {
      if (origin == value)
        return originID;
      ++originID;
    }
    return std::nullopt;
  }

  void collectFactsFromClangFunction(souffle::SouffleProgram *prog,
                                     const clang::FunctionDecl *FD,
                                     const std::string &caller,
                                     unsigned callerReturnOrigin,
                                     unsigned callerNewLoan,
                                     unsigned callerBlockId,
                                     llvm::ArrayRef<Operand> callerOperands) {
    ASTContext& ctx = getModule()->getASTContext();
    clang::AnalysisDeclContext AC(nullptr, FD);
    AC.getCFGBuildOptions().PruneTriviallyFalseEdges = true;
    AC.getCFGBuildOptions().AddEHEdges = false;
    AC.getCFGBuildOptions().AddInitializers = true;
    AC.getCFGBuildOptions().AddImplicitDtors = true;
    AC.getCFGBuildOptions().AddTemporaryDtors = true;
    AC.getCFGBuildOptions().AddCXXNewAllocator = false;
    AC.getCFGBuildOptions().AddCXXDefaultInitExprInCtors = true;
    AC.getCFGBuildOptions().setAllAlwaysAdd();
    clang::lifetimes::internal::LifetimeSafetyAnalysis analysis(AC, nullptr);
    analysis.run();
    auto factsToBlocks = analysis.getFactToBlockMapping();
    auto name = FD->getNameAsString();
    for (auto fact : analysis.getFacts()) {
      if (auto issueFact = fact->getAs<clang::lifetimes::internal::IssueFact>()) {
        souffle::tuple issue(issueRelation);
        auto& origin = analysis.getOrigin(issueFact->getOriginID());
        issue << issueFact->getLoanID().Value << issueFact->getOriginID().Value
              << getLocation(origin).getRawEncoding()
              << factsToBlocks[issueFact]->getBlockID() << name;
        issueRelation->insert(issue);
        ClangLocToSwiftLoc[getLocation(origin).getRawEncoding()] = ctx.getClangModuleLoader()->importSourceLocation(getLocation(origin));
      } else if (auto expireFact = fact->getAs<clang::lifetimes::internal::ExpireFact>()) {
        souffle::tuple expire(expireRelation);
        expire << expireFact->getLoanID().Value
               << expireFact->getExpiryLoc().getRawEncoding()
               << factsToBlocks[expireFact]->getBlockID() << name;
        expireRelation->insert(expire);
        ClangLocToSwiftLoc[expireFact->getExpiryLoc().getRawEncoding()] = ctx.getClangModuleLoader()->importSourceLocation(expireFact->getExpiryLoc());
      } else if (auto useFact = fact->getAs<clang::lifetimes::internal::UseFact>()) {
        souffle::tuple use(useRelation);
        use << useFact->getUsedOrigin().Value
            << useFact->getUseExpr()->getExprLoc().getRawEncoding()
            << factsToBlocks[useFact]->getBlockID() << name;
        useRelation->insert(use);
        ClangLocToSwiftLoc[useFact->getUseExpr()->getExprLoc().getRawEncoding()] = ctx.getClangModuleLoader()->importSourceLocation(useFact->getUseExpr()->getExprLoc());
      } else if (auto assignFact = fact->getAs<clang::lifetimes::internal::AssignOriginFact>()) {
        souffle::tuple assign(assignRelation);
        assign << assignFact->getSrcOriginID().Value
               << assignFact->getDestOriginID().Value << name;
        assignRelation->insert(assign);
      } else if (auto returnOriginFact = fact->getAs<clang::lifetimes::internal::ReturnOfOriginFact>()) {
        souffle::tuple ret(returnRelation);
        ret << callerNewLoan << callerReturnOrigin << callerBlockId << caller
            << returnOriginFact->getReturnedOriginID().Value << name;
        returnRelation->insert(ret);
      } else if (auto passFact =
                     fact->getAs<
                         clang::lifetimes::internal::PassOfOriginFact>()) {
        for (auto &op : callerOperands) {
          auto value = op.get();
          if (!isTrackedType(value->getType()))
            continue;
          auto originID = getOriginID(value);
          if (!originID)
            continue;
          souffle::tuple pass(passRelation);
          pass << *originID << callerBlockId << caller
               << passFact->getLoanID().Value << passFact->getOriginID().Value
               << name;
          passRelation->insert(pass);
          break;
        }
      } else if (auto deleteFact =
                     fact->getAs<clang::lifetimes::internal::DeleteFact>()) {
        souffle::tuple deallocate(deallocateRelation);
        auto &origin = analysis.getOrigin(deleteFact->getOriginID());
        deallocate << deleteFact->getOriginID().Value
                   << getLocation(origin).getRawEncoding()
                   << factsToBlocks[deleteFact]->getBlockID() << name;
        deallocateRelation->insert(deallocate);
        ClangLocToSwiftLoc[getLocation(origin).getRawEncoding()] =
            ctx.getClangModuleLoader()->importSourceLocation(
                getLocation(origin));
      }
    }
    auto cfg = AC.getCFG();
    for (auto block : *cfg) {
      for (auto succ : block->succs()) {
        souffle::tuple edge(edgeRelation);
        edge << block->getBlockID() << succ->getBlockID() << name;
        edgeRelation->insert(edge);
      }
    }
  }

  void collectFromFunction(souffle::SouffleProgram *prog,
                           SILFunction &function) {
    if (function.empty())
      return;
    auto name = function.getName().str();
    CalleeInfo info;
    bool hadPointerArgOrResult = false;
    for (auto arg : function.getArguments()) {
      if (isTrackedType(arg->getType())) {
        hadPointerArgOrResult = true;
        info.paramIdxToOriginAndLoan[arg->getIndex()] =
            OriginAndLoan{getNextSwiftOriginID(arg), getNextSwiftLoanID()};
      }
    }
    for (SILBasicBlock &block : function) {
      for (auto succ : block.getSuccessorBlocks()) {
        souffle::tuple edge(edgeRelation);
        edge << (unsigned)block.getDebugID() << (unsigned)succ->getDebugID()
             << name;
        edgeRelation->insert(edge);
      }
      for (auto pred : block.getPredecessorBlocks()) {
        for (auto &phi : block.phis()) {
          auto op = phi.getIncomingPhiOperand(pred);
          if (!op)
            continue;
          auto originID = getOriginID(op->get());
          if (!originID)
            continue;
          auto toOrigin = getNextSwiftOriginID(&phi);
          souffle::tuple assign(assignRelation);
          assign << *originID << toOrigin << name;
          assignRelation->insert(assign);
        }
      }
      for (SILInstruction &inst : block) {
        if (isa<DebugValueInst>(&inst))
          continue;
        auto location = inst.getLoc().getSourceLoc();
        if (auto returnInst = dyn_cast<ReturnInst>(&inst)) {
          auto originID = getOriginID(returnInst->getOperand());
          if (!originID)
            break;
          hadPointerArgOrResult = true;
          info.returnOrigins.push_back(*originID);
        }
        if (auto fas = FullApplySite::isa(&inst)) {
          SILFunction *callee = fas.getCalleeFunction();
          if (!callee)
            continue;
          if (const auto *clangDecl = callee->getClangDecl()) {
            if (const auto *FD = dyn_cast<clang::FunctionDecl>(clangDecl)) {
              if (FD->isDefined()) {
                auto result = fas->getResult(0);
                collectFactsFromClangFunction(
                    prog, FD, name, getNextSwiftOriginID(result),
                    getNextSwiftLoanID(), block.getDebugID(),
                    inst.getAllOperands());
              }
            }
            continue;
          }
          auto declRef = callee->getDeclRef();
          if (auto afd = declRef.getAbstractFunctionDecl()) {
            if (!afd->getName().isSpecial()) {
              if (afd->getNameStr() == "allocate") {
                auto result = fas->getResult(0);
                SwiftLocs.push_back(location);
                souffle::tuple issue(issueRelation);
                issue << getNextSwiftLoanID() << getNextSwiftOriginID(result)
                      << static_cast<uint32_t>(SwiftLocs.size() - 1)
                      << (unsigned)block.getDebugID() << name;
                issueRelation->insert(issue);
              } else if (afd->getNameStr() == "deallocate") {
                souffle::tuple deallocate(deallocateRelation);
                auto self = fas.getSelfArgument();
                SwiftLocs.push_back(location);
                auto originID = getOriginID(self);
                if (!originID)
                  continue;
                deallocate << *originID
                           << static_cast<uint32_t>(SwiftLocs.size() - 1)
                           << (unsigned)block.getDebugID() << name;
                deallocateRelation->insert(deallocate);
              } else {
                if (callee->empty())
                  continue;
                CallerInfo info{function, *callee,
                                (unsigned)block.getDebugID()};
                for (auto [idx, value] : llvm::enumerate(fas.getArguments())) {
                  if (!isTrackedType(value->getType()))
                    continue;
                  auto originID = getOriginID(value);
                  if (!originID)
                    continue;
                  info.argIdxToOrigin[idx] = *originID;
                }
                for (auto resultTy : inst.getResultTypes()) {
                  if (isTrackedType(resultTy)) {
                    auto result = fas->getResult(0);
                    info.returnOriginAndLoan = OriginAndLoan{
                        getNextSwiftOriginID(result), getNextSwiftLoanID()};
                  }
                  break; // FIXME: multiple return types
                }
                callers.push_back(info);
              }
              continue;
            }
          }
        }
        for (auto &op : inst.getAllOperands()) {
          auto value = op.get();
          if (!isTrackedType(value->getType()))
            continue;
          auto originID = getOriginID(value);
          if (!originID)
            continue;
          souffle::tuple use(useRelation);
          SwiftLocs.push_back(location);
          use << *originID << static_cast<uint32_t>(SwiftLocs.size() - 1)
              << (unsigned)block.getDebugID() << name;
          useRelation->insert(use);
        }
      }
    }
    if (hadPointerArgOrResult)
      callees.insert(std::make_pair(&function, info));
  }

  void collectFacts(souffle::SouffleProgram *prog) {
    SILModule *module = getModule();
    for (SILFunction &function : *module) {
      collectFromFunction(prog, function);
    }
    // Connect callers and callees.
    for (auto &caller : callers) {
      auto it = callees.find(&caller.callee);
      if (it == callees.end())
        continue;
      auto &callee = it->second;
      for (auto [idx, OriginAndLoan] : callee.paramIdxToOriginAndLoan) {
        auto callerOriginIt = caller.argIdxToOrigin.find(idx);
        if (callerOriginIt == caller.argIdxToOrigin.end())
          continue;
        souffle::tuple pass(passRelation);
        pass << callerOriginIt->second << caller.blockID
             << caller.caller.getName().str() << OriginAndLoan.loan
             << OriginAndLoan.origin << caller.callee.getName().str();
        passRelation->insert(pass);
      }
      if (caller.returnOriginAndLoan) {
        for (auto returnOriginInCallee : callee.returnOrigins) {
          souffle::tuple ret(returnRelation);
          ret << caller.returnOriginAndLoan->loan
              << caller.returnOriginAndLoan->origin << caller.blockID
              << caller.caller.getName().str() << returnOriginInCallee
              << caller.callee.getName().str();
          returnRelation->insert(ret);
        }
      }
    }
  }

  SourceLoc translate(uint32_t encodedLoc) {
    if (encodedLoc < SwiftLocs.size()) {
      return SwiftLocs[encodedLoc];
    }

    return ClangLocToSwiftLoc[encodedLoc];
  }

  void processClangOutput(souffle::SouffleProgram *prog) {
    ASTContext& ctx = getModule()->getASTContext();
    souffle::Relation *defaultedRelation = prog->getRelation("UseDefaulted");
    uint32_t useLoc, expireLoc, issueLoc, dummy;
    for (auto& row : *defaultedRelation) {
      row >> useLoc >> dummy >> expireLoc >> dummy >> issueLoc >> dummy;
      ctx.Diags.diagnose(translate(useLoc), diag::lifetime_use_after_free);
      ctx.Diags.diagnose(translate(expireLoc), diag::lifetime_ended_here);
      ctx.Diags.diagnose(translate(issueLoc), diag::lifetime_created_here);
    }
  }

  void run() override {
    ClangLocToSwiftLoc.clear();
    if (souffle::SouffleProgram *prog = souffle::ProgramFactory::newInstance("Lifetimes")) {
      issueRelation = prog->getRelation("Issue");
      expireRelation = prog->getRelation("Expire");
      assignRelation = prog->getRelation("Assign");
      returnRelation = prog->getRelation("Return");
      useRelation = prog->getRelation("Use");
      edgeRelation = prog->getRelation("Edge");
      deallocateRelation = prog->getRelation("Deallocate");
      passRelation = prog->getRelation("Pass");
      collectFacts(prog);
      prog->run();
      prog->dumpInputs();
      prog->dumpOutputs();
      processClangOutput(prog);
      delete prog;
    }
  }
};

SILTransform *swift::createDatalogDiagnostics() {
  return new DatalogDiagnosticsPass();
}

