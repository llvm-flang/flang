//===--- CodeGenAction.cpp - LLVM Code Generation Frontend Action ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "flang/CodeGen/CodeGenAction.h"
#include "flang/CodeGen/BackendUtil.h"
#include "flang/AST/ASTConsumer.h"
#include "flang/AST/ASTContext.h"
#include "flang/AST/DeclGroup.h"
#include "flang/CodeGen/BackendUtil.h"
#include "flang/CodeGen/ModuleBuilder.h"
#include "flang/Frontend/CompilerInstance.h"
#include "flang/Frontend/FrontendDiagnostic.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker.h"
#include "llvm/Pass.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Timer.h"

using namespace flang;
using namespace llvm;

namespace flang {
  class BackendConsumer : public ASTConsumer {
    virtual void anchor();
    DiagnosticsEngine &Diags;
    BackendAction Action;
    const CodeGenOptions &CodeGenOpts;
    const TargetOptions &TargetOpts;
    const LangOptions &LangOpts;
    raw_ostream *AsmOutStream;
    ASTContext *Context;

    Timer LLVMIRGeneration;

    OwningPtr<CodeGenerator> Gen;

    OwningPtr<llvm::Module> TheModule, LinkModule;

  public:
    BackendConsumer(BackendAction action, DiagnosticsEngine &_Diags,
                    const CodeGenOptions &compopts,
                    const TargetOptions &targetopts,
                    const LangOptions &langopts,
                    bool TimePasses,
                    const std::string &infile,
                    llvm::Module *LinkModule,
                    raw_ostream *OS,
                    LLVMContext &C) :
      Diags(_Diags),
      Action(action),
      CodeGenOpts(compopts),
      TargetOpts(targetopts),
      LangOpts(langopts),
      AsmOutStream(OS),
      Context(), 
      LLVMIRGeneration("LLVM IR Generation Time"),
      Gen(CreateLLVMCodeGen(Diags, infile, compopts, targetopts, C)),
      LinkModule(LinkModule)
    {
      llvm::TimePassesIsEnabled = TimePasses;
    }

    llvm::Module *takeModule() { return TheModule.take(); }
    llvm::Module *takeLinkModule() { return LinkModule.take(); }

    virtual void Initialize(ASTContext &Ctx) {
      Context = &Ctx;

      if (llvm::TimePassesIsEnabled)
        LLVMIRGeneration.startTimer();

      Gen->Initialize(Ctx);

      TheModule.reset(Gen->GetModule());

      if (llvm::TimePassesIsEnabled)
        LLVMIRGeneration.stopTimer();
    }

    virtual void HandleTranslationUnit(ASTContext &C) {
      {
        PrettyStackTraceString CrashInfo("Per-file LLVM IR generation");
        if (llvm::TimePassesIsEnabled)
          LLVMIRGeneration.startTimer();

        Gen->HandleTranslationUnit(C);

        if (llvm::TimePassesIsEnabled)
          LLVMIRGeneration.stopTimer();
      }

      // Silently ignore if we weren't initialized for some reason.
      if (!TheModule)
        return;

      // Make sure IR generation is happy with the module. This is released by
      // the module provider.
      llvm::Module *M = Gen->ReleaseModule();
      if (!M) {
        // The module has been released by IR gen on failures, do not double
        // free.
        TheModule.take();
        return;
      }

      assert(TheModule.get() == M &&
             "Unexpected module change during IR generation");

      // Link LinkModule into this module if present, preserving its validity.
      if (LinkModule) {
        std::string ErrorMsg;
        if (Linker::LinkModules(M, LinkModule.get(), Linker::PreserveSource,
                                &ErrorMsg)) {
          Diags.Report(diag::err_fe_cannot_link_module)
            << LinkModule->getModuleIdentifier() << ErrorMsg;
          return;
        }
      }

      EmitBackendOutput(Diags, CodeGenOpts, TargetOpts, LangOpts,
                        TheModule.get(), Action, AsmOutStream);
      
    }

  };
  
  void BackendConsumer::anchor() {}
}

//

CodeGenAction::CodeGenAction(unsigned _Act, LLVMContext *_VMContext)
  : Act(_Act), LinkModule(0),
    VMContext(_VMContext ? _VMContext : new LLVMContext),
    OwnsVMContext(!_VMContext) {}

CodeGenAction::~CodeGenAction() {
  TheModule.reset();
  if (OwnsVMContext)
    delete VMContext;
}

bool CodeGenAction::hasIRSupport() const { return true; }

void CodeGenAction::EndSourceFileAction() {
  // If the consumer creation failed, do nothing.
  if (!getCompilerInstance().hasASTConsumer())
    return;

  // If we were given a link module, release consumer's ownership of it.
  if (LinkModule)
    BEConsumer->takeLinkModule();

  // Steal the module from the consumer.
  TheModule.reset(BEConsumer->takeModule());
}

llvm::Module *CodeGenAction::takeModule() {
  return TheModule.take();
}

llvm::LLVMContext *CodeGenAction::takeLLVMContext() {
  OwnsVMContext = false;
  return VMContext;
}

static raw_ostream *GetOutputStream(CompilerInstance &CI,
                                    StringRef InFile,
                                    BackendAction Action) {
  switch (Action) {
  case Backend_EmitAssembly:
    return CI.createDefaultOutputFile(false, InFile, "s");
  case Backend_EmitLL:
    return CI.createDefaultOutputFile(false, InFile, "ll");
  case Backend_EmitBC:
    return CI.createDefaultOutputFile(true, InFile, "bc");
  case Backend_EmitNothing:
    return 0;
  case Backend_EmitMCNull:
  case Backend_EmitObj:
    return CI.createDefaultOutputFile(true, InFile, "o");
  }

  llvm_unreachable("Invalid action!");
}

ASTConsumer *CodeGenAction::CreateASTConsumer(CompilerInstance &CI,
                                              StringRef InFile) {
  BackendAction BA = static_cast<BackendAction>(Act);
  OwningPtr<raw_ostream> OS(GetOutputStream(CI, InFile, BA));
  if (BA != Backend_EmitNothing && !OS)
    return 0;

  llvm::Module *LinkModuleToUse = LinkModule;

  BEConsumer = 
      new BackendConsumer(BA, CI.getDiagnostics(),
                          CI.getCodeGenOpts(), CI.getTargetOpts(),
                          CI.getLangOpts(),
                          CI.getFrontendOpts().ShowTimers, InFile,
                          LinkModuleToUse, OS.take(), *VMContext);
  return BEConsumer;
}

void CodeGenAction::ExecuteAction() {
  // Otherwise follow the normal AST path.
  this->ASTFrontendAction::ExecuteAction();
}

//

void EmitAssemblyAction::anchor() { }
EmitAssemblyAction::EmitAssemblyAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitAssembly, _VMContext) {}

void EmitBCAction::anchor() { }
EmitBCAction::EmitBCAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitBC, _VMContext) {}

void EmitLLVMAction::anchor() { }
EmitLLVMAction::EmitLLVMAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitLL, _VMContext) {}

void EmitLLVMOnlyAction::anchor() { }
EmitLLVMOnlyAction::EmitLLVMOnlyAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitNothing, _VMContext) {}

void EmitCodeGenOnlyAction::anchor() { }
EmitCodeGenOnlyAction::EmitCodeGenOnlyAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitMCNull, _VMContext) {}

void EmitObjAction::anchor() { }
EmitObjAction::EmitObjAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitObj, _VMContext) {}
