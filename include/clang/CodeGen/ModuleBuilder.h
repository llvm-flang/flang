//===--- CodeGen/ModuleBuilder.h - Build LLVM from ASTs ---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines the ModuleBuilder interface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_FLANG_CODEGEN_MODULEBUILDER_H
#define LLVM_FLANG_CODEGEN_MODULEBUILDER_H

#include "flang/AST/ASTConsumer.h"
#include "flang/Basic/TargetOptions.h"
#include "flang/Frontend/CodeGenOptions.h"
#include <string>

namespace llvm {
  class LLVMContext;
  class Module;
}

namespace flang {
  class DiagnosticsEngine;
  class LangOptions;

  class CodeGenerator : public ASTConsumer {
    virtual void anchor();
  public:
    virtual llvm::Module* GetModule() = 0;
    virtual llvm::Module* ReleaseModule() = 0;
  };

  /// CreateLLVMCodeGen - Create a CodeGenerator instance.
  /// It is the responsibility of the caller to call delete on
  /// the allocated CodeGenerator instance.
  CodeGenerator *CreateLLVMCodeGen(DiagnosticsEngine &Diags,
                                   const std::string &ModuleName,
                                   const CodeGenOptions &CGO,
                                   const TargetOptions &TO,
                                   llvm::LLVMContext& C);
}

#endif
