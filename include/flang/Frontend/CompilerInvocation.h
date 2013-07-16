//===-- CompilerInvocation.h - Compiler Invocation Helper Data --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_FLANG_FRONTEND_COMPILERINVOCATION_H_
#define LLVM_FLANG_FRONTEND_COMPILERINVOCATION_H_

#include "flang/Basic/LangOptions.h"
#include "flang/Basic/TargetOptions.h"
#include "flang/Frontend/CodeGenOptions.h"
#include "flang/Frontend/FrontendOptions.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace llvm {
namespace opt {
class ArgList;
}
}

namespace flang {
class CompilerInvocation;
class DiagnosticsEngine;
struct DiagnosticOptions {};

/// \brief Fill out Opts based on the options given in Args.
///
/// Args must have been created from the OptTable returned by
/// createCC1OptTable().
///
/// When errors are encountered, return false and, if Diags is non-null,
/// report the error(s).
bool ParseDiagnosticArgs(DiagnosticOptions &Opts, llvm::opt::ArgList &Args,
                         DiagnosticsEngine *Diags = 0);

class CompilerInvocationBase : public llvm::RefCountedBase<CompilerInvocation> {
protected:
  /// Options controlling the language variant.
  llvm::IntrusiveRefCntPtr<LangOptions> LangOpts;

  /// Options controlling the target.
  llvm::IntrusiveRefCntPtr<TargetOptions> TargetOpts;

public:
  CompilerInvocationBase();

  CompilerInvocationBase(const CompilerInvocationBase &X);
  
  LangOptions *getLangOpts() { return LangOpts.getPtr(); }
  const LangOptions *getLangOpts() const { return LangOpts.getPtr(); }

  TargetOptions &getTargetOpts() { return *TargetOpts.getPtr(); }
  const TargetOptions &getTargetOpts() const {
    return *TargetOpts.getPtr();
  }

};
  
/// \brief Helper class for holding the data necessary to invoke the compiler.
///
/// This class is designed to represent an abstract "invocation" of the
/// compiler, including data such as the include paths, the code generation
/// options, the warning flags, and so on.
class CompilerInvocation : public CompilerInvocationBase {
  
  /// Options controlling IRgen and the backend.
  CodeGenOptions CodeGenOpts;

  /// Options controlling the frontend itself.
  FrontendOptions FrontendOpts;

public:
  CompilerInvocation() {}

  /// @name Utility Methods
  /// @{

  /// \brief Create a compiler invocation from a list of input options.
  /// \returns true on success.
  ///
  /// \param [out] Res - The resulting invocation.
  /// \param ArgBegin - The first element in the argument vector.
  /// \param ArgEnd - The last element in the argument vector.
  /// \param Diags - The diagnostic engine to use for errors.
  static bool CreateFromArgs(CompilerInvocation &Res,
                             const char* const *ArgBegin,
                             const char* const *ArgEnd,
                             DiagnosticsEngine &Diags);

  /// \brief Get the directory where the compiler headers
  /// reside, relative to the compiler binary (found by the passed in
  /// arguments).
  ///
  /// \param Argv0 - The program path (from argv[0]), for finding the builtin
  /// compiler path.
  /// \param MainAddr - The address of main (or some other function in the main
  /// executable), for finding the builtin compiler path.
  static std::string GetResourcesPath(const char *Argv0, void *MainAddr);
  
  /// \brief Retrieve a module hash string that is suitable for uniquely 
  /// identifying the conditions under which the module was built.
  std::string getModuleHash() const;
  
  /// @}
  /// @name Option Subgroups
  /// @{

  CodeGenOptions &getCodeGenOpts() { return CodeGenOpts; }
  const CodeGenOptions &getCodeGenOpts() const {
    return CodeGenOpts;
  }

  FrontendOptions &getFrontendOpts() { return FrontendOpts; }
  const FrontendOptions &getFrontendOpts() const {
    return FrontendOpts;
  }

  /// @}
};

} // end namespace flang

#endif
