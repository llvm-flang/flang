//===--- BackendUtil.cpp - LLVM Backend Utilities -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "flang/CodeGen/BackendUtil.h"
#include "flang/Basic/Diagnostic.h"
#include "flang/Basic/LangOptions.h"
#include "flang/Basic/TargetOptions.h"
#include "flang/Frontend/CodeGenOptions.h"
#include "flang/Frontend/FrontendDiagnostic.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Assembly/PrintModulePass.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/SchedulerRegistry.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetLibraryInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/ObjCARC.h"
#include "llvm/Transforms/Scalar.h"
using namespace flang;
using namespace llvm;

namespace {

class EmitAssemblyHelper {
  DiagnosticsEngine &Diags;
  const CodeGenOptions &CodeGenOpts;
  const flang::TargetOptions &TargetOpts;
  const LangOptions &LangOpts;
  Module *TheModule;

  Timer CodeGenerationTime;

  mutable PassManager *CodeGenPasses;
  mutable PassManager *PerModulePasses;
  mutable FunctionPassManager *PerFunctionPasses;

private:
  PassManager *getCodeGenPasses(TargetMachine *TM) const {
    if (!CodeGenPasses) {
      CodeGenPasses = new PassManager();
      CodeGenPasses->add(new DataLayout(TheModule));
      if (TM)
        TM->addAnalysisPasses(*CodeGenPasses);
    }
    return CodeGenPasses;
  }

  PassManager *getPerModulePasses(TargetMachine *TM) const {
    if (!PerModulePasses) {
      PerModulePasses = new PassManager();
      PerModulePasses->add(new DataLayout(TheModule));
      if (TM)
        TM->addAnalysisPasses(*PerModulePasses);
    }
    return PerModulePasses;
  }

  FunctionPassManager *getPerFunctionPasses(TargetMachine *TM) const {
    if (!PerFunctionPasses) {
      PerFunctionPasses = new FunctionPassManager(TheModule);
      PerFunctionPasses->add(new DataLayout(TheModule));
      if (TM)
        TM->addAnalysisPasses(*PerFunctionPasses);
    }
    return PerFunctionPasses;
  }


  void CreatePasses(TargetMachine *TM);

  /// CreateTargetMachine - Generates the TargetMachine.
  /// Returns Null if it is unable to create the target machine.
  /// Some of our clang tests specify triples which are not built
  /// into clang. This is okay because these tests check the generated
  /// IR, and they require DataLayout which depends on the triple.
  /// In this case, we allow this method to fail and not report an error.
  /// When MustCreateTM is used, we print an error if we are unable to load
  /// the requested target.
  TargetMachine *CreateTargetMachine(bool MustCreateTM);

  /// AddEmitPasses - Add passes necessary to emit assembly or LLVM IR.
  ///
  /// \return True on success.
  bool AddEmitPasses(BackendAction Action, formatted_raw_ostream &OS,
                     TargetMachine *TM);

public:
  EmitAssemblyHelper(DiagnosticsEngine &_Diags,
                     const CodeGenOptions &CGOpts,
                     const flang::TargetOptions &TOpts,
                     const LangOptions &LOpts,
                     Module *M)
    : Diags(_Diags), CodeGenOpts(CGOpts), TargetOpts(TOpts), LangOpts(LOpts),
      TheModule(M), CodeGenerationTime("Code Generation Time"),
      CodeGenPasses(0), PerModulePasses(0), PerFunctionPasses(0) {}

  ~EmitAssemblyHelper() {
    delete CodeGenPasses;
    delete PerModulePasses;
    delete PerFunctionPasses;
  }

  void EmitAssembly(BackendAction Action, raw_ostream *OS);
};

// We need this wrapper to access LangOpts and CGOpts from extension functions
// that we add to the PassManagerBuilder.
class PassManagerBuilderWrapper : public PassManagerBuilder {
public:
  PassManagerBuilderWrapper(const CodeGenOptions &CGOpts,
                            const LangOptions &LangOpts)
      : PassManagerBuilder(), CGOpts(CGOpts), LangOpts(LangOpts) {}
  const CodeGenOptions &getCGOpts() const { return CGOpts; }
  const LangOptions &getLangOpts() const { return LangOpts; }
private:
  const CodeGenOptions &CGOpts;
  const LangOptions &LangOpts;
};

}

static void addBoundsCheckingPass(const PassManagerBuilder &Builder,
                                    PassManagerBase &PM) {
  PM.add(createBoundsCheckingPass());
}

void EmitAssemblyHelper::CreatePasses(TargetMachine *TM) {
  unsigned OptLevel = CodeGenOpts.OptimizationLevel;
  CodeGenOptions::InliningMethod Inlining = CodeGenOpts.getInlining();

  // Handle disabling of LLVM optimization, where we want to preserve the
  // internal module before any optimization.
  if (CodeGenOpts.DisableLLVMOpts) {
    OptLevel = 0;
    Inlining = CodeGenOpts.NoInlining;
  }

  PassManagerBuilderWrapper PMBuilder(CodeGenOpts, LangOpts);
  PMBuilder.OptLevel = OptLevel;
  PMBuilder.SizeLevel = CodeGenOpts.OptimizeSize;
  PMBuilder.BBVectorize = CodeGenOpts.VectorizeBB;
  PMBuilder.SLPVectorize = CodeGenOpts.VectorizeSLP;
  PMBuilder.LoopVectorize = CodeGenOpts.VectorizeLoop;

  PMBuilder.DisableUnitAtATime = !CodeGenOpts.UnitAtATime;
  PMBuilder.DisableUnrollLoops = !CodeGenOpts.UnrollLoops;

  // Figure out TargetLibraryInfo.
  Triple TargetTriple(TheModule->getTargetTriple());
  PMBuilder.LibraryInfo = new TargetLibraryInfo(TargetTriple);
  if (!CodeGenOpts.SimplifyLibCalls)
    PMBuilder.LibraryInfo->disableAllFunctions();
  
  switch (Inlining) {
  case CodeGenOptions::NoInlining: break;
  case CodeGenOptions::NormalInlining: {
    // FIXME: Derive these constants in a principled fashion.
    unsigned Threshold = 225;
    if (CodeGenOpts.OptimizeSize == 1)      // -Os
      Threshold = 75;
    else if (CodeGenOpts.OptimizeSize == 2) // -Oz
      Threshold = 25;
    else if (OptLevel > 2)
      Threshold = 275;
    PMBuilder.Inliner = createFunctionInliningPass(Threshold);
    break;
  }
  case CodeGenOptions::OnlyAlwaysInlining:
    // Respect always_inline.
    if (OptLevel == 0)
      // Do not insert lifetime intrinsics at -O0.
      PMBuilder.Inliner = createAlwaysInlinerPass(false);
    else
      PMBuilder.Inliner = createAlwaysInlinerPass();
    break;
  }

  // Set up the per-function pass manager.
  FunctionPassManager *FPM = getPerFunctionPasses(TM);
  if (CodeGenOpts.VerifyModule)
    FPM->add(createVerifierPass());
  PMBuilder.populateFunctionPassManager(*FPM);

  // Set up the per-module pass manager.
  PassManager *MPM = getPerModulePasses(TM);

  if (!CodeGenOpts.DisableGCov &&
      (CodeGenOpts.EmitGcovArcs || CodeGenOpts.EmitGcovNotes)) {
    // Not using 'GCOVOptions::getDefault' allows us to avoid exiting if
    // LLVM's -default-gcov-version flag is set to something invalid.
    GCOVOptions Options;
    Options.EmitNotes = CodeGenOpts.EmitGcovNotes;
    Options.EmitData = CodeGenOpts.EmitGcovArcs;
    memcpy(Options.Version, CodeGenOpts.CoverageVersion, 4);
    Options.UseCfgChecksum = CodeGenOpts.CoverageExtraChecksum;
    Options.NoRedZone = CodeGenOpts.DisableRedZone;
    Options.FunctionNamesInData =
        !CodeGenOpts.CoverageNoFunctionNamesInData;
    MPM->add(createGCOVProfilerPass(Options));
    if (CodeGenOpts.getDebugInfo() == CodeGenOptions::NoDebugInfo)
      MPM->add(createStripSymbolsPass(true));
  }

  PMBuilder.populateModulePassManager(*MPM);
}

TargetMachine *EmitAssemblyHelper::CreateTargetMachine(bool MustCreateTM) {
  // Create the TargetMachine for generating code.
  std::string Error;
  std::string Triple = TheModule->getTargetTriple();
  const llvm::Target *TheTarget = TargetRegistry::lookupTarget(Triple, Error);
  if (!TheTarget) {
    if (MustCreateTM)
      Diags.Report(diag::err_fe_unable_to_create_target) << Error;
    return 0;
  }

  // FIXME: Expose these capabilities via actual APIs!!!! Aside from just
  // being gross, this is also totally broken if we ever care about
  // concurrency.

  TargetMachine::setAsmVerbosityDefault(CodeGenOpts.AsmVerbose);

  TargetMachine::setFunctionSections(CodeGenOpts.FunctionSections);
  TargetMachine::setDataSections    (CodeGenOpts.DataSections);

  // FIXME: Parse this earlier.
  llvm::CodeModel::Model CM;
  if (CodeGenOpts.CodeModel == "small") {
    CM = llvm::CodeModel::Small;
  } else if (CodeGenOpts.CodeModel == "kernel") {
    CM = llvm::CodeModel::Kernel;
  } else if (CodeGenOpts.CodeModel == "medium") {
    CM = llvm::CodeModel::Medium;
  } else if (CodeGenOpts.CodeModel == "large") {
    CM = llvm::CodeModel::Large;
  } else {
    assert(CodeGenOpts.CodeModel.empty() && "Invalid code model!");
    CM = llvm::CodeModel::Default;
  }

  SmallVector<const char *, 16> BackendArgs;
  BackendArgs.push_back("clang"); // Fake program name.
  if (!CodeGenOpts.DebugPass.empty()) {
    BackendArgs.push_back("-debug-pass");
    BackendArgs.push_back(CodeGenOpts.DebugPass.c_str());
  }
  if (!CodeGenOpts.LimitFloatPrecision.empty()) {
    BackendArgs.push_back("-limit-float-precision");
    BackendArgs.push_back(CodeGenOpts.LimitFloatPrecision.c_str());
  }
  if (llvm::TimePassesIsEnabled)
    BackendArgs.push_back("-time-passes");
  for (unsigned i = 0, e = CodeGenOpts.BackendOptions.size(); i != e; ++i)
    BackendArgs.push_back(CodeGenOpts.BackendOptions[i].c_str());
  if (CodeGenOpts.NoGlobalMerge)
    BackendArgs.push_back("-global-merge=false");
  BackendArgs.push_back(0);
  llvm::cl::ParseCommandLineOptions(BackendArgs.size() - 1,
                                    BackendArgs.data());

  std::string FeaturesStr;
  if (TargetOpts.Features.size()) {
    SubtargetFeatures Features;
    for (std::vector<std::string>::const_iterator
           it = TargetOpts.Features.begin(),
           ie = TargetOpts.Features.end(); it != ie; ++it)
      Features.AddFeature(*it);
    FeaturesStr = Features.getString();
  }

  llvm::Reloc::Model RM = llvm::Reloc::Default;
  if (CodeGenOpts.RelocationModel == "static") {
    RM = llvm::Reloc::Static;
  } else if (CodeGenOpts.RelocationModel == "pic") {
    RM = llvm::Reloc::PIC_;
  } else {
    assert(CodeGenOpts.RelocationModel == "dynamic-no-pic" &&
           "Invalid PIC model!");
    RM = llvm::Reloc::DynamicNoPIC;
  }

  CodeGenOpt::Level OptLevel = CodeGenOpt::Default;
  switch (CodeGenOpts.OptimizationLevel) {
  default: break;
  case 0: OptLevel = CodeGenOpt::None; break;
  case 3: OptLevel = CodeGenOpt::Aggressive; break;
  }

  llvm::TargetOptions Options;

  // Set frame pointer elimination mode.
  if (!CodeGenOpts.DisableFPElim) {
    Options.NoFramePointerElim = false;
    Options.NoFramePointerElimNonLeaf = false;
  } else if (CodeGenOpts.OmitLeafFramePointer) {
    Options.NoFramePointerElim = false;
    Options.NoFramePointerElimNonLeaf = true;
  } else {
    Options.NoFramePointerElim = true;
    Options.NoFramePointerElimNonLeaf = true;
  }

  if (CodeGenOpts.UseInitArray)
    Options.UseInitArray = true;

  // Set float ABI type.
  if (CodeGenOpts.FloatABI == "soft" || CodeGenOpts.FloatABI == "softfp")
    Options.FloatABIType = llvm::FloatABI::Soft;
  else if (CodeGenOpts.FloatABI == "hard")
    Options.FloatABIType = llvm::FloatABI::Hard;
  else {
    assert(CodeGenOpts.FloatABI.empty() && "Invalid float abi!");
    Options.FloatABIType = llvm::FloatABI::Default;
  }

  // Set FP fusion mode.
  switch (CodeGenOpts.getFPContractMode()) {
  case CodeGenOptions::FPC_Off:
    Options.AllowFPOpFusion = llvm::FPOpFusion::Strict;
    break;
  case CodeGenOptions::FPC_On:
    Options.AllowFPOpFusion = llvm::FPOpFusion::Standard;
    break;
  case CodeGenOptions::FPC_Fast:
    Options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    break;
  }

  Options.LessPreciseFPMADOption = CodeGenOpts.LessPreciseFPMAD;
  Options.NoInfsFPMath = CodeGenOpts.NoInfsFPMath;
  Options.NoNaNsFPMath = CodeGenOpts.NoNaNsFPMath;
  Options.NoZerosInBSS = CodeGenOpts.NoZeroInitializedInBSS;
  Options.UnsafeFPMath = CodeGenOpts.UnsafeFPMath;
  Options.UseSoftFloat = CodeGenOpts.SoftFloat;
  Options.StackAlignmentOverride = CodeGenOpts.StackAlignment;
  Options.RealignStack = CodeGenOpts.StackRealignment;
  Options.DisableTailCalls = CodeGenOpts.DisableTailCalls;
  Options.TrapFuncName = CodeGenOpts.TrapFuncName;
  //Options.PositionIndependentExecutable = LangOpts.PIELevel != 0;
  Options.EnableSegmentedStacks = CodeGenOpts.EnableSegmentedStacks;

  TargetMachine *TM = TheTarget->createTargetMachine(Triple, TargetOpts.CPU,
                                                     FeaturesStr, Options,
                                                     RM, CM, OptLevel);

  if (CodeGenOpts.RelaxAll)
    TM->setMCRelaxAll(true);
  if (CodeGenOpts.SaveTempLabels)
    TM->setMCSaveTempLabels(true);
  if (CodeGenOpts.NoDwarf2CFIAsm)
    TM->setMCUseCFI(false);
  if (!CodeGenOpts.NoDwarfDirectoryAsm)
    TM->setMCUseDwarfDirectory(true);
  if (CodeGenOpts.NoExecStack)
    TM->setMCNoExecStack(true);

  return TM;
}

bool EmitAssemblyHelper::AddEmitPasses(BackendAction Action,
                                       formatted_raw_ostream &OS,
                                       TargetMachine *TM) {

  // Create the code generator passes.
  PassManager *PM = getCodeGenPasses(TM);

  // Add LibraryInfo.
  llvm::Triple TargetTriple(TheModule->getTargetTriple());
  TargetLibraryInfo *TLI = new TargetLibraryInfo(TargetTriple);
  if (!CodeGenOpts.SimplifyLibCalls)
    TLI->disableAllFunctions();
  PM->add(TLI);

  // Add Target specific analysis passes.
  TM->addAnalysisPasses(*PM);

  // Normal mode, emit a .s or .o file by running the code generator. Note,
  // this also adds codegenerator level optimization passes.
  TargetMachine::CodeGenFileType CGFT = TargetMachine::CGFT_AssemblyFile;
  if (Action == Backend_EmitObj)
    CGFT = TargetMachine::CGFT_ObjectFile;
  else if (Action == Backend_EmitMCNull)
    CGFT = TargetMachine::CGFT_Null;
  else
    assert(Action == Backend_EmitAssembly && "Invalid action!");

  if (TM->addPassesToEmitFile(*PM, OS, CGFT,
                              /*DisableVerify=*/!CodeGenOpts.VerifyModule)) {
    Diags.Report(diag::err_fe_unable_to_interface_with_target);
    return false;
  }

  return true;
}

void EmitAssemblyHelper::EmitAssembly(BackendAction Action, raw_ostream *OS) {
  TimeRegion Region(llvm::TimePassesIsEnabled ? &CodeGenerationTime : 0);
  llvm::formatted_raw_ostream FormattedOS;

  bool UsesCodeGen = (Action != Backend_EmitNothing &&
                      Action != Backend_EmitBC &&
                      Action != Backend_EmitLL);
  TargetMachine *TM = CreateTargetMachine(UsesCodeGen);
  if (UsesCodeGen && !TM) return;
  llvm::OwningPtr<TargetMachine> TMOwner(CodeGenOpts.DisableFree ? 0 : TM);
  CreatePasses(TM);

  switch (Action) {
  case Backend_EmitNothing:
    break;

  case Backend_EmitBC:
    getPerModulePasses(TM)->add(createBitcodeWriterPass(*OS));
    break;

  case Backend_EmitLL:
    FormattedOS.setStream(*OS, formatted_raw_ostream::PRESERVE_STREAM);
    getPerModulePasses(TM)->add(createPrintModulePass(&FormattedOS));
    break;

  default:
    FormattedOS.setStream(*OS, formatted_raw_ostream::PRESERVE_STREAM);
    if (!AddEmitPasses(Action, FormattedOS, TM))
      return;
  }

  // Before executing passes, print the final values of the LLVM options.
  cl::PrintOptionValues();

  // Run passes. For now we do all passes at once, but eventually we
  // would like to have the option of streaming code generation.

  if (PerFunctionPasses) {
    PrettyStackTraceString CrashInfo("Per-function optimization");

    PerFunctionPasses->doInitialization();
    for (Module::iterator I = TheModule->begin(),
           E = TheModule->end(); I != E; ++I)
      if (!I->isDeclaration())
        PerFunctionPasses->run(*I);
    PerFunctionPasses->doFinalization();
  }

  if (PerModulePasses) {
    PrettyStackTraceString CrashInfo("Per-module optimization passes");
    PerModulePasses->run(*TheModule);
  }

  if (CodeGenPasses) {
    PrettyStackTraceString CrashInfo("Code generation");
    CodeGenPasses->run(*TheModule);
  }
}

void flang::EmitBackendOutput(DiagnosticsEngine &Diags,
                              const CodeGenOptions &CGOpts,
                              const flang::TargetOptions &TOpts,
                              const LangOptions &LOpts,
                              Module *M,
                              BackendAction Action, raw_ostream *OS) {
  EmitAssemblyHelper AsmHelper(Diags, CGOpts, TOpts, LOpts, M);

  AsmHelper.EmitAssembly(Action, OS);
}
