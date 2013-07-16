//===--- ASTUnit.h - ASTUnit utility ----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// ASTUnit utility class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_FLANG_FRONTEND_ASTUNIT_H
#define LLVM_FLANG_FRONTEND_ASTUNIT_H

#include "flang/AST/ASTContext.h"
#include "flang/AST/ASTConsumer.h"
#include "flang/Basic/LangOptions.h"
#include "flang/Basic/TargetOptions.h"
#include "flang/Sema/Sema.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Path.h"
#include <cassert>
#include <map>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace llvm {
  class MemoryBuffer;
}

namespace flang {
class ASTContext;
class ASTReader;
class CodeCompleteConsumer;
class CompilerInvocation;
class CompilerInstance;
class Decl;
class DiagnosticsEngine;
class FileEntry;
class FileManager;
class HeaderSearch;
class Preprocessor;
class SourceManager;
class TargetInfo;
class ASTFrontendAction;
class ASTDeserializationListener;

/// \brief Utility class for loading a ASTContext from an AST file.
///
class ASTUnit {
private:
  llvm::IntrusiveRefCntPtr<LangOptions>         LangOpts;
  llvm::IntrusiveRefCntPtr<DiagnosticsEngine>   Diagnostics;
  llvm::IntrusiveRefCntPtr<TargetInfo>          Target;
  llvm::IntrusiveRefCntPtr<ASTContext>          Ctx;
  llvm::IntrusiveRefCntPtr<TargetOptions>       TargetOpts;

  /// \brief The AST consumer that received information about the translation
  /// unit as it was parsed or loaded.
  llvm::OwningPtr<ASTConsumer> Consumer;
  
  /// \brief The semantic analysis object used to type-check the translation
  /// unit.
  llvm::OwningPtr<Sema> TheSema;
  
  /// Optional owned invocation, just used to make the invocation used in
  /// LoadFromCommandLine available.
  llvm::IntrusiveRefCntPtr<CompilerInvocation> Invocation;
  
  /// The name of the original source file used to generate this ASTUnit.
  std::string OriginalSourceFile;

 
  /// \brief The language options used when we load an AST file.
  LangOptions ASTFileLangOpts;
  
  ASTUnit(const ASTUnit &) LLVM_DELETED_FUNCTION;
  void operator=(const ASTUnit &) LLVM_DELETED_FUNCTION;
  
  explicit ASTUnit(bool MainFileIsAST);

  void CleanTemporaryFiles();
  bool Parse(llvm::MemoryBuffer *OverrideMainBuffer);

  /// \brief Transfers ownership of the objects (like SourceManager) from
  /// \param CI to this ASTUnit.
  void transferASTDataFromCompilerInstance(CompilerInstance &CI);

public:
  
  ~ASTUnit();

  const DiagnosticsEngine &getDiagnostics() const { return *Diagnostics; }
  DiagnosticsEngine &getDiagnostics()             { return *Diagnostics; }


  const ASTContext &getASTContext() const { return *Ctx; }
        ASTContext &getASTContext()       { return *Ctx; }

  void setASTContext(ASTContext *ctx) { Ctx = ctx; }

  bool hasSema() const { return TheSema.get() != nullptr; }
  Sema &getSema() const { 
    assert(TheSema && "ASTUnit does not have a Sema object!");
    return *TheSema; 
  }

  StringRef getOriginalSourceFileName() {
    return OriginalSourceFile;
  }


  /// \brief Get the source location for the given file:line:col triplet.
  ///
  /// The difference with SourceManager::getLocation is that this method checks
  /// whether the requested location points inside the precompiled preamble
  /// in which case the returned source location will be a "loaded" one.
  SourceLocation getLocation(const FileEntry *File,
                             unsigned Line, unsigned Col) const;

  /// \brief Get the source location for the given file:offset pair.
  SourceLocation getLocation(const FileEntry *File, unsigned Offset) const;


  llvm::MemoryBuffer *getBufferForFile(StringRef Filename,
                                       std::string *ErrorStr = 0);

  /// \brief Create a ASTUnit. Gets ownership of the passed CompilerInvocation. 
  static ASTUnit *create(CompilerInvocation *CI,
                         llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags);
  
public:
  
  /// \brief Create an ASTUnit from a source file, via a CompilerInvocation
  /// object, by invoking the optionally provided ASTFrontendAction. 
  ///
  /// \param CI - The compiler invocation to use; it must have exactly one input
  /// source file. The ASTUnit takes ownership of the CompilerInvocation object.
  ///
  /// \param Diags - The diagnostics engine to use for reporting errors; its
  /// lifetime is expected to extend past that of the returned ASTUnit.
  ///
  /// \param Action - The ASTFrontendAction to invoke. Its ownership is not
  /// transfered.
  ///
  /// \param Unit - optionally an already created ASTUnit. Its ownership is not
  /// transfered.
  ///
  /// \param Persistent - if true the returned ASTUnit will be complete.
  /// false means the caller is only interested in getting info through the
  /// provided \see Action.
  ///
  /// \param ErrAST - If non-null and parsing failed without any AST to return
  /// (e.g. because the PCH could not be loaded), this accepts the ASTUnit
  /// mainly to allow the caller to see the diagnostics.
  /// This will only receive an ASTUnit if a new one was created. If an already
  /// created ASTUnit was passed in \p Unit then the caller can check that.
  ///
  static ASTUnit *LoadFromCompilerInvocationAction(CompilerInvocation *CI,
                              llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags,
                                             ASTFrontendAction *Action = 0,
                                             ASTUnit *Unit = 0,
                                             bool Persistent = true,
                                      StringRef ResourceFilesPath = StringRef(),
                                             bool OnlyLocalDecls = false,
                                             bool CaptureDiagnostics = false,
                                             bool PrecompilePreamble = false,
                                       bool CacheCodeCompletionResults = false,
                              bool IncludeBriefCommentsInCodeCompletion = false,
                                       bool UserFilesAreVolatile = false,
                                       llvm::OwningPtr<ASTUnit> *ErrAST = 0);

  /// LoadFromCompilerInvocation - Create an ASTUnit from a source file, via a
  /// CompilerInvocation object.
  ///
  /// \param CI - The compiler invocation to use; it must have exactly one input
  /// source file. The ASTUnit takes ownership of the CompilerInvocation object.
  ///
  /// \param Diags - The diagnostics engine to use for reporting errors; its
  /// lifetime is expected to extend past that of the returned ASTUnit.
  //
  // FIXME: Move OnlyLocalDecls, UseBumpAllocator to setters on the ASTUnit, we
  // shouldn't need to specify them at construction time.
  static ASTUnit *LoadFromCompilerInvocation(CompilerInvocation *CI,
                                             llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags);


};

} // namespace clang

#endif
