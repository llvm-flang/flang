//===-- Decl.h - Declarations -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Classes for Fortran declarations.
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_AST_DECL_H__
#define FORTRAN_AST_DECL_H__

#include "flang/AST/DeclarationName.h"
#include "flang/AST/Type.h"
#include "flang/Basic/IdentifierTable.h"
#include "flang/Basic/SourceLocation.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "flang/Basic/LLVM.h"

namespace llvm {
  class SourceMgr;
}

namespace flang {

class ASTContext;
class DeclSpec;
class IdentifierInfo;
class StoredDeclsMap;
class TypeLoc;

// Decls
class DeclContext;
class TranslationUnitDecl;
class NamedDecl;
class TypeDecl;
class RecordDecl;
class ValueDecl;
class EnumConstantDecl;
class DeclaratorDecl;
class FunctionDecl;
class FieldDecl;
class VarDecl;
class FileScopeAsmDecl;

} // end fortran namespace

namespace llvm {

// DeclContext* is only 4-byte aligned on 32-bit systems.
template<>
  class PointerLikeTypeTraits<flang::DeclContext*> {
  typedef flang::DeclContext* PT;
public:
  static inline void *getAsVoidPointer(PT P) { return P; }
  static inline PT getFromVoidPointer(void *P) {
    return static_cast<PT>(P);
  }
  enum { NumLowBitsAvailable = 2 };
};

} // end llvm namespace

namespace flang {

//===----------------------------------------------------------------------===//
/// Decl - Base class for declarations.
///
class Decl {
public:
  /// \brief Lists the kind of concrete classes of Decl.
  enum Kind {
#define DECL(DERIVED, BASE) DERIVED,
#define ABSTRACT_DECL(DECL)
#define DECL_RANGE(BASE, START, END) \
        first##BASE = START, last##BASE = END,
#define LAST_DECL_RANGE(BASE, START, END) \
        first##BASE = START, last##BASE = END
#include "flang/AST/DeclNodes.inc"
  };

private:
  friend class DeclContext;

  /// NextDeclInContext - The next declaration within the same lexical
  /// DeclContext. These pointers form the linked list that is traversed via
  /// DeclContext's decls_begin()/decls_end().
  Decl *NextDeclInContext;

  /// DeclCtx - The declaration context.
  DeclContext *DeclCtx;

  /// Loc - The location of this decl.
  llvm::SMLoc Loc;

  /// DeclKind - The class of decl this is.
  unsigned DeclKind    : 8;

  /// InvalidDecl - This indicates a semantic error occurred.
  unsigned InvalidDecl : 1;

  /// HasAttrs - This indicates whether the decl has attributes or not.
  unsigned HasAttrs    : 1;

  /// Implicit - Whether this declaration was implicitly generated by
  /// the implementation rather than explicitly written by the user.
  unsigned Implicit    : 1;

protected:

  Decl(Kind DK, DeclContext *DC, llvm::SMLoc L)
    : NextDeclInContext(0), DeclCtx(DC), Loc(L), DeclKind(DK),
      InvalidDecl(false), HasAttrs(false), Implicit(false) {}

  virtual ~Decl();

public:
  /// \brief Source range that this declaration covers.
  virtual SourceRange getSourceRange() const {
    return SourceRange(getLocation(), getLocation());
  }
  llvm::SMLoc getLocStart() const { return getSourceRange().getBegin(); }
  llvm::SMLoc getLocEnd() const { return getSourceRange().getEnd(); }

  llvm::SMLoc getLocation() const { return Loc; }
  void setLocation(llvm::SMLoc L) { Loc = L; }

  Kind getKind() const { return static_cast<Kind>(DeclKind); }

  Decl *getNextDeclInContext() { return NextDeclInContext; }
  const Decl *getNextDeclInContext() const { return NextDeclInContext; }

  DeclContext *getDeclContext() { return DeclCtx; }
  const DeclContext *getDeclContext() const { return DeclCtx; }
  void setDeclContext(DeclContext *DC) { DeclCtx = DC; }

  TranslationUnitDecl *getTranslationUnitDecl();
  const TranslationUnitDecl *getTranslationUnitDecl() const {
    return const_cast<Decl*>(this)->getTranslationUnitDecl();
  }

  ASTContext &getASTContext() const;

  /// setInvalidDecl - Indicates the Decl had a semantic error. This
  /// allows for graceful error recovery.
  void setInvalidDecl(bool Invalid = true) { InvalidDecl = Invalid; }
  bool isInvalidDecl() const { return (bool)InvalidDecl; }

  /// isImplicit - Indicates whether the declaration was implicitly
  /// generated by the implementation. If false, this declaration
  /// was written explicitly in the source code.
  bool isImplicit() const { return Implicit; }
  void setImplicit(bool I = true) { Implicit = I; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *) { return true; }
  static DeclContext *castToDeclContext(const Decl *);
  static Decl *castFromDeclContext(const DeclContext *);
};

/// PrettyStackTraceDecl - If a crash occurs, indicate that it happened when
/// doing something to a specific decl.
class PrettyStackTraceDecl : public llvm::PrettyStackTraceEntry {
  const Decl *TheDecl;
  llvm::SMLoc Loc;
  llvm::SourceMgr &SM;
  const char *Message;
public:
  PrettyStackTraceDecl(const Decl *theDecl, llvm::SMLoc L,
                       llvm::SourceMgr &sm, const char *Msg)
    : TheDecl(theDecl), Loc(L), SM(sm), Message(Msg) {}

  virtual void print(raw_ostream &OS) const;
};

class DeclContextLookupResult
  : public std::pair<NamedDecl**,NamedDecl**> {
public:
  DeclContextLookupResult(NamedDecl **I, NamedDecl **E)
    : std::pair<NamedDecl**,NamedDecl**>(I, E) {}
  DeclContextLookupResult()
    : std::pair<NamedDecl**,NamedDecl**>() {}

  using std::pair<NamedDecl**,NamedDecl**>::operator=;
};

class DeclContextLookupConstResult
  : public std::pair<NamedDecl*const*, NamedDecl*const*> {
public:
  DeclContextLookupConstResult(std::pair<NamedDecl**,NamedDecl**> R)
    : std::pair<NamedDecl*const*, NamedDecl*const*>(R) {}
  DeclContextLookupConstResult(NamedDecl * const *I, NamedDecl * const *E)
    : std::pair<NamedDecl*const*, NamedDecl*const*>(I, E) {}
  DeclContextLookupConstResult()
    : std::pair<NamedDecl*const*, NamedDecl*const*>() {}

  using std::pair<NamedDecl*const*,NamedDecl*const*>::operator=;
};

/// DeclContext - This is used only as base class of specific decl types that
/// can act as declaration contexts. These decls are (only the top classes
/// that directly derive from DeclContext are mentioned, not their subclasses):
///
///   TranslationUnitDecl
///   FunctionDecl
///   RecordDecl
///
class DeclContext {
  /// DeclKind - This indicates which class this is.
  unsigned DeclKind : 8;

  /// \brief Pointer to the data structure used to lookup declarations within
  /// this context.
  mutable StoredDeclsMap *LookupPtr;
protected:
  /// FirstDecl - The first declaration stored within this declaration
  /// context.
  mutable Decl *FirstDecl;

  /// LastDecl - The last declaration stored within this declaration
  /// context.
  mutable Decl *LastDecl;

  DeclContext(Decl::Kind K)
    : DeclKind(K), LookupPtr(0), FirstDecl(0), LastDecl(0) {}
public:
  ~DeclContext();

  DeclContext *getParent() {
    return Decl::castFromDeclContext(this)->getDeclContext();
  }
  const DeclContext *getParent() const {
    return const_cast<DeclContext*>(this)->getParent();
  }

  ASTContext &getParentASTContext() const {
    return Decl::castFromDeclContext(this)->getASTContext();
  }

  Decl::Kind getDeclKind() const {
    return static_cast<Decl::Kind>(DeclKind);
  }

  bool isTranslationUnit() const { return DeclKind == Decl::TranslationUnit; }
  bool isRecord() const { return DeclKind == Decl::Record; }

  /// \brief Retrieve the internal representation of the lookup structure.
  StoredDeclsMap *getLookupPtr() const { return LookupPtr; }

  /// decl_iterator - Iterates through the declarations stored within this
  /// context.
  class decl_iterator {
    /// Current - The current declaration.
    Decl *Current;

  public:
    typedef Decl*                     value_type;
    typedef Decl*                     reference;
    typedef Decl*                     pointer;
    typedef std::forward_iterator_tag iterator_category;
    typedef std::ptrdiff_t            difference_type;

    decl_iterator() : Current(0) { }
    explicit decl_iterator(Decl *C) : Current(C) { }

    reference operator*() const { return Current; }
    pointer operator->() const { return Current; }

    decl_iterator& operator++() {
      Current = Current->getNextDeclInContext();
      return *this;
    }

    decl_iterator operator++(int) {
      decl_iterator tmp(*this);
      ++(*this);
      return tmp;
    }

    friend bool operator==(decl_iterator x, decl_iterator y) {
      return x.Current == y.Current;
    }
    friend bool operator!=(decl_iterator x, decl_iterator y) {
      return x.Current != y.Current;
    }
  };

  /// decls_begin/decls_end - Iterate over the declarations stored in this
  /// context.
  decl_iterator decls_begin() const { return decl_iterator(FirstDecl); }
  decl_iterator decls_end() const   { return decl_iterator(); }
  bool decls_empty() const          { return !FirstDecl; }

  /// @brief Add the declaration D into this context.
  ///
  /// This routine should be invoked when the declaration D has first been
  /// declared, to place D into the context where it was (lexically)
  /// defined. Every declaration must be added to one (and only one!) context,
  /// where it can be visited via [decls_begin(), decls_end()). Once a
  /// declaration has been added to its lexical context, the corresponding
  /// DeclContext owns the declaration.
  ///
  /// If D is also a NamedDecl, it will be made visible within its semantic
  /// context via makeDeclVisibleInContext.
  void addDecl(Decl *D);

  /// @brief Removes a declaration from this context.
  void removeDecl(Decl *D);

  /// lookup_iterator - An iterator that provides access to the results of
  /// looking up a name within this context.
  typedef NamedDecl **lookup_iterator;

  /// lookup_const_iterator - An iterator that provides non-mutable access to
  /// the results of lookup up a name within this context.
  typedef NamedDecl * const *lookup_const_iterator;

  typedef DeclContextLookupResult lookup_result;
  typedef DeclContextLookupConstResult lookup_const_result;

  /// lookup - Find the declarations (if any) with the given Name in this
  /// context. Returns a range of iterators that contains all of the
  /// declarations with this name, with object, function, member, and enumerator
  /// names preceding any tag name. Note that this routine will not look into
  /// parent contexts.
  lookup_result lookup(DeclarationName Name);
  lookup_const_result lookup(DeclarationName Name) const {
    return const_cast<DeclContext*>(this)->lookup(Name);
  }

  /// @brief Makes a declaration visible within this context.
  ///
  /// This routine makes the declaration D visible to name lookup within this
  /// context and, if this is a transparent context, within its parent contexts
  /// up to the first enclosing non-transparent context. Making a declaration
  /// visible within a context does not transfer ownership of a declaration, and
  /// a declaration can be visible in many contexts that aren't its lexical
  /// context.
  void makeDeclVisibleInContext(NamedDecl *D);

  static bool classof(const Decl *D);
  static bool classof(const DeclContext *D) { return true; }
#define DECL(NAME, BASE)
#define DECL_CONTEXT(NAME) \
  static bool classof(const NAME##Decl *D) { return true; }
#include "flang/AST/DeclNodes.inc"

private:
  StoredDeclsMap *CreateStoredDeclsMap(ASTContext &C) const;
  void buildLookup(DeclContext *DCtx);
  void makeDeclVisibleInContextImpl(NamedDecl *D);
};

/// \brief A container of type source information.
///
/// A client can read the relevant info using TypeLoc wrappers, e.g:
/// @code
/// TypeLoc TL = TypeSourceInfo->getTypeLoc();
/// if (PointerLoc *PL = dyn_cast<PointerLoc>(&TL))
///   PL->getStarLoc().print(OS, SrcMgr);
/// @endcode
///
class TypeSourceInfo {
  QualType Ty;
  // Contains a memory block after the class, used for type source information,
  // allocated by ASTContext.
  friend class ASTContext;
  TypeSourceInfo(QualType ty) : Ty(ty) { }
public:
  /// \brief Return the type wrapped by this type source info.
  QualType getType() const { return Ty; }
#if 0
  /// \brief Return the TypeLoc wrapper for the type source info.
  TypeLoc getTypeLoc() const; // implemented in TypeLoc.h
#endif
};

/// TranslationUnitDecl - The top declaration context.
class TranslationUnitDecl : public Decl, public DeclContext {
  ASTContext &Ctx;

  explicit TranslationUnitDecl(ASTContext &ctx)
    : Decl(TranslationUnit, 0, llvm::SMLoc()),
      DeclContext(TranslationUnit), Ctx(ctx) {}
public:
  ASTContext &getASTContext() const { return Ctx; }

  static TranslationUnitDecl *Create(ASTContext &C);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const TranslationUnitDecl *D) { return true; }
  static bool classofKind(Kind K) { return K == TranslationUnit; }
  static DeclContext *castToDeclContext(const TranslationUnitDecl *D) {
    return static_cast<DeclContext*>(const_cast<TranslationUnitDecl*>(D));
  }
  static TranslationUnitDecl *castFromDeclContext(const DeclContext *DC) {
    return static_cast<TranslationUnitDecl*>(const_cast<DeclContext*>(DC));
  }
};

/// NamedDecl - This represents a decl with a name.
class NamedDecl : public Decl {
  /// Name - The name of this declaration, which is typically a normal
  /// identifier.
  DeclarationName Name;
  friend class ASTContext;
  friend class DeclContext;
protected:
  NamedDecl(Kind DK, DeclContext *DC, llvm::SMLoc L, DeclarationName N)
    : Decl(DK, DC, L), Name(N) {}
public:
  /// getIdentifier - Get the identifier that names this declaration, if there
  /// is one.
  IdentifierInfo *getIdentifier() const { return Name.getAsIdentifierInfo(); }

  /// getName - Get the name of identifier for this declaration as a StringRef.
  /// This requires that the declaration have a name and that it be a simple
  /// identifier.
  StringRef getName() const {
    assert(Name.isIdentifier() && "Name is not a simple identifier");
    return getIdentifier() ? getIdentifier()->getName() : "";
  }

  /// getDeclName - Get the actual, stored name of the declaration.
  DeclarationName getDeclName() const { return Name; }

  /// \brief Set the name of this declaration.
  void setDeclName(DeclarationName N) { Name = N; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const NamedDecl *D) { return true; }
  static bool classofKind(Kind K) { return K >= firstNamed && K <= lastNamed; }
};

/// TypeDecl - Represents a declaration of a type.
class TypeDecl : public NamedDecl {
  /// TypeForDecl - This indicates the Type object that represents this
  /// TypeDecl. It is a cache maintained by ASTContext::getTagDeclType.
  mutable const Type *TypeForDecl;

  /// LocStart - The start of the source range for this declaration.
  llvm::SMLoc LocStart;

  friend class ASTContext;
  friend class DeclContext;
  friend class RecordDecl;
  friend class TagType;
protected:
  TypeDecl(Kind DK, DeclContext *DC, llvm::SMLoc L, IdentifierInfo *Id,
           llvm::SMLoc StartL = llvm::SMLoc())
    : NamedDecl(DK, DC, L, Id), TypeForDecl(0), LocStart(StartL) {}

public:
  // Low-level accessor
  const Type *getTypeForDecl() const { return TypeForDecl; }
  void setTypeForDecl(const Type *TD) { TypeForDecl = TD; }

  llvm::SMLoc getLocStart() const { return LocStart; }
  void setLocStart(llvm::SMLoc L) { LocStart = L; }

  virtual SourceRange getSourceRange() const {
    if (LocStart.isValid())
      return SourceRange(LocStart, getLocation());
    else
      return SourceRange(getLocation());
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const TypeDecl *D) { return true; }
  static bool classofKind(Kind K) { return K >= firstType && K <= lastType; }
};

/// RecordDecl - Represents a structure. This decl will be marked invalid if
/// *any* members are invalid.
class RecordDecl : public TypeDecl, public DeclContext {
  /// IsDefinition - True if this is a definition ("struct foo {};"), false if
  /// it is a declaration ("struct foo;").
  bool IsDefinition : 1;

  /// IsBeingDefined - True if this is currently being defined.
  bool IsBeingDefined : 1;

  friend class DeclContext;
protected:
  RecordDecl(Kind DK, DeclContext *DC, llvm::SMLoc StartLoc, llvm::SMLoc IdLoc,
             IdentifierInfo *Id, RecordDecl *PrevDecl)
    : TypeDecl(DK, DC, IdLoc, Id), DeclContext(Record) {
    IsDefinition = false;
    IsBeingDefined = false;
  }
public:
  static RecordDecl *Create(const ASTContext &C, DeclContext *DC,
                            llvm::SMLoc StartLoc, llvm::SMLoc IdLoc,
                            IdentifierInfo *Id, RecordDecl *PrevDecl = 0);

  /// getOuterLocStart - Return SMLoc representing start of source range taking
  /// into account any outer template declarations.
  virtual SourceRange getSourceRange() const {
    if (LocStart.isValid())
      return SourceRange(LocStart, getLocation());
    else
      return SourceRange(getLocation());
  }

  // FIXME: Could be more than just 'this'.
  virtual RecordDecl *getCanonicalDecl() { return this; }
  const RecordDecl *getCanonicalDecl() const {
    return const_cast<RecordDecl*>(this)->getCanonicalDecl();
  }

  /// completeDefinition - Notes that the definition of this type is now
  /// complete.
  virtual void completeDefinition() {
    assert(!isDefinition() && "Cannot redefine record!");
    IsDefinition = true;
    IsBeingDefined = false;
  }

  /// isDefinition - Return true if this decl has its body specified.
  bool isDefinition() const { return IsDefinition; }

  /// isBeingDefined - Return true if this decl is currently being defined.
  bool isBeingDefined() const { return IsBeingDefined; }

  /// @brief Starts the definition of this struct declaration.
  ///
  /// This method should be invoked at the beginning of the definition of this
  /// struct declaration. It will set the struct type into a state where it is
  /// in the process of being defined.
  void startDefinition() { IsBeingDefined = true; }

  /// getDefinition - Returns the RecordDecl that actually defines this struct.
  /// When determining whether or not a struct is completely defined, one should
  /// use this method as opposed to 'isDefinition'. 'isDefinition' indicates
  /// whether or not a specific RecordDecl is defining declaration, not whether
  /// or not the struct type is defined. This method returns NULL if there is
  /// no RecordDecl that defines the struct.
  RecordDecl *getDefinition() const {
    return isDefinition() ? const_cast<RecordDecl*>(this) : 0;
  }

  void setDefinition(bool V) { IsDefinition = V; }

  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const RecordDecl *D) { return true; }
  static bool classofKind(Kind K) { return K == Record; }

  static DeclContext *castToDeclContext(const RecordDecl *D) {
    return static_cast<DeclContext*>(const_cast<RecordDecl*>(D));
  }
  static RecordDecl *castFromDeclContext(const DeclContext *DC) {
    return static_cast<RecordDecl*>(const_cast<DeclContext*>(DC));
  }
};

/// ValueDecl - Represent the declaration of a variable (in which case it is an
/// lvalue), a function (in which case it is a function designator), or an enum
/// constant.
class ValueDecl : public NamedDecl {
  QualType DeclType;
protected:
  ValueDecl(Kind DK, DeclContext *DC, llvm::SMLoc L,
            DeclarationName N, QualType T)
    : NamedDecl(DK, DC, L, N), DeclType(T) {}
public:
  QualType getType() const { return DeclType; }
  void setType(QualType newType) { DeclType = newType; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const ValueDecl *D) { return true; }
  static bool classofKind(Kind K) { return K >= firstValue && K <= lastValue; }
};

/// EnumConstantDecl - An instance of this object exists for each enum constant
/// that is defined.
class EnumConstantDecl : public ValueDecl {
  Expr *Init;                   // An integer constant expression.
  llvm::APSInt Val;             // The value.
protected:
  EnumConstantDecl(DeclContext *DC, llvm::SMLoc L,
                   IdentifierInfo *Id, QualType T, Expr *E,
                   const llvm::APSInt &V)
    : ValueDecl(EnumConstant, DC, L, Id, T), Init(E), Val(V) {}
public:
  static EnumConstantDecl *Create(ASTContext &C, DeclContext *DC,
                                  llvm::SMLoc L, IdentifierInfo *Id,
                                  QualType T, Expr *E,
                                  const llvm::APSInt &V);

  const Expr *getInitExpr() const { return Init; }
  Expr *getInitExpr() { return Init; }
  const llvm::APSInt &getInitVal() const { return Val; }

  void setInitExpr(Expr *E) { Init = E; }
  void setInitVal(const llvm::APSInt &V) { Val = V; }

  SourceRange getSourceRange() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const EnumConstantDecl *D) { return true; }
  static bool classofKind(Kind K) { return K == EnumConstant; }
};

/// \brief Represents a ValueDecl that came out of a declarator.
class DeclaratorDecl : public ValueDecl {
protected:
  DeclaratorDecl(Kind DK, DeclContext *DC, llvm::SMLoc L,
                 DeclarationName N, QualType T)
    : ValueDecl(DK, DC, L, N, T) {
  }
public:
  virtual SourceRange getSourceRange() const {
    // TODO
    return SourceRange();
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const DeclaratorDecl *D) { return true; }
  static bool classofKind(Kind K) {
    return K >= firstDeclarator && K <= lastDeclarator;
  }
};

class FunctionDecl : public DeclaratorDecl, public DeclContext {
protected:
  FunctionDecl(Kind DK, DeclContext *DC, const DeclarationNameInfo &NameInfo,
               QualType T)
    : DeclaratorDecl(DK, DC, NameInfo.getLoc(), NameInfo.getName(), T),
      DeclContext(DK) {
  }
public:

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const FunctionDecl *D) { return true; }
  static bool classofKind(Kind K) { return K == Function; }
  static DeclContext *castToDeclContext(const FunctionDecl *D) {
    return static_cast<DeclContext *>(const_cast<FunctionDecl*>(D));
  }
  static FunctionDecl *castFromDeclContext(const DeclContext *DC) {
    return static_cast<FunctionDecl *>(const_cast<DeclContext*>(DC));
  }
};

/// FieldDecl - An instance of this class is created by Sema::ActOnField to
/// represent a member of a struct.
class FieldDecl : public DeclaratorDecl {
protected:
  FieldDecl(Kind DK, DeclContext *DC, llvm::SMLoc IdLoc, IdentifierInfo *Id,
            QualType T)
    : DeclaratorDecl(DK, DC, IdLoc, Id, T) {}
public:
  static FieldDecl *Create(const ASTContext &C, DeclContext *DC,
                           llvm::SMLoc IdLoc, IdentifierInfo *Id, QualType T);

  /// getParent - Returns the parent of this field declaration, which is the
  /// struct in which this method is defined.
  const RecordDecl *getParent() const {
    return RecordDecl::castFromDeclContext(getDeclContext());
  }
  RecordDecl *getParent() {
    return RecordDecl::castFromDeclContext(getDeclContext());
  }

  SourceRange getSourceRange() const {
    return DeclaratorDecl::getSourceRange();
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const FieldDecl *D) { return true; }
  static bool classofKind(Kind K) { return K == Field; }
};

/// VarDecl - An instance of this class is created to represent a variable
/// declaration or definition.
#if 0
// FIXME:
class VarDecl : public DeclaratorDecl {
#endif
class VarDecl : public DeclaratorDecl, public llvm::FoldingSetNode {
  /// \brief The initializer for this variable.
  mutable Expr *Init; // FIXME: This should be a different type?

  const DeclSpec *DS;

  friend class ASTContext;  // ASTContext creates these.

protected:
  VarDecl(Kind DK, DeclContext *DC, llvm::SMLoc IdLoc, IdentifierInfo *Id,
          QualType T)
    : DeclaratorDecl(DK, DC, IdLoc, Id, T), Init() {}

public:
  static VarDecl *Create(ASTContext &C, DeclContext *DC,
                         llvm::SMLoc IdLoc, IdentifierInfo *Id, QualType T);

  VarDecl(const IdentifierInfo *Info)
    // FIXME:
    : DeclaratorDecl(Var, 0, llvm::SMLoc(), Info, QualType()),
      DS(0)
  {}
  VarDecl(llvm::SMLoc L, const DeclSpec *dts, const IdentifierInfo *Info)
    // FIXME:
    : DeclaratorDecl(Var, 0, L, Info, QualType()),
      DS(dts)
  {}

  const DeclSpec *getDeclSpec() const { return DS; }
  void setDeclSpec(const DeclSpec *Val) { DS = Val; }

  /// isImplicitlyDefined - A variable which isn't defined and there isn't an
  /// "implicit none" statement has a default type of REAL.
  bool isImplicitlyDefined() const { return DS == 0; }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getIdentifier());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, const IdentifierInfo *Info) {
    ID.AddPointer(Info);
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const VarDecl *D) { return true; }
  static bool classofKind(Kind K) { return K == Var; }
};

class FileScopeAsmDecl : public Decl {
  FileScopeAsmDecl(DeclContext *DC, llvm::SMLoc StartL, llvm::SMLoc EndL)
    : Decl(FileScopeAsm, DC, StartL) {}
public:
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classof(const FileScopeAsmDecl *D) { return true; }
  static bool classofKind(Kind K) { return K == FileScopeAsm; }
};

static inline llvm::raw_ostream &operator<<(llvm::raw_ostream &O,
                                            const VarDecl &V) {
  return O << V.getIdentifier()->getName();
}

// Specialization selected when ToTy is not a known subclass of DeclContext.
template <class ToTy,
          bool IsKnownSubtype = ::llvm::is_base_of< DeclContext, ToTy>::value>
struct cast_convert_decl_context {
  static const ToTy *doit(const DeclContext *Val) {
    return static_cast<const ToTy*>(Decl::castFromDeclContext(Val));
  }

  static ToTy *doit(DeclContext *Val) {
    return static_cast<ToTy*>(Decl::castFromDeclContext(Val));
  }
};

// Specialization selected when ToTy is a known subclass of DeclContext.
template <class ToTy>
struct cast_convert_decl_context<ToTy, true> {
  static const ToTy *doit(const DeclContext *Val) {
    return static_cast<const ToTy*>(Val);
  }

  static ToTy *doit(DeclContext *Val) {
    return static_cast<ToTy*>(Val);
  }
};

} // end flang namespace

namespace llvm {

/// isa<T>(DeclContext*)
template <typename To>
struct isa_impl<To, ::flang::DeclContext> {
  static bool doit(const ::flang::DeclContext &Val) {
    return To::classofKind(Val.getDeclKind());
  }
};

/// cast<T>(DeclContext*)
template<class ToTy>
struct cast_convert_val<ToTy,
                        const ::flang::DeclContext,const ::flang::DeclContext> {
  static const ToTy &doit(const ::flang::DeclContext &Val) {
    return *::flang::cast_convert_decl_context<ToTy>::doit(&Val);
  }
};
template<class ToTy>
struct cast_convert_val<ToTy, ::flang::DeclContext, ::flang::DeclContext> {
  static ToTy &doit(::flang::DeclContext &Val) {
    return *::flang::cast_convert_decl_context<ToTy>::doit(&Val);
  }
};
template<class ToTy>
struct cast_convert_val<ToTy,
                     const ::flang::DeclContext*, const ::flang::DeclContext*> {
  static const ToTy *doit(const ::flang::DeclContext *Val) {
    return ::flang::cast_convert_decl_context<ToTy>::doit(Val);
  }
};
template<class ToTy>
struct cast_convert_val<ToTy, ::flang::DeclContext*, ::flang::DeclContext*> {
  static ToTy *doit(::flang::DeclContext *Val) {
    return ::flang::cast_convert_decl_context<ToTy>::doit(Val);
  }
};

/// Implement cast_convert_val for Decl -> DeclContext conversions.
template<class FromTy>
struct cast_convert_val< ::flang::DeclContext, FromTy, FromTy> {
  static ::flang::DeclContext &doit(const FromTy &Val) {
    return *FromTy::castToDeclContext(&Val);
  }
};

template<class FromTy>
struct cast_convert_val< ::flang::DeclContext, FromTy*, FromTy*> {
  static ::flang::DeclContext *doit(const FromTy *Val) {
    return FromTy::castToDeclContext(Val);
  }
};

template<class FromTy>
struct cast_convert_val< const ::flang::DeclContext, FromTy, FromTy> {
  static const ::flang::DeclContext &doit(const FromTy &Val) {
    return *FromTy::castToDeclContext(&Val);
  }
};

template<class FromTy>
struct cast_convert_val< const ::flang::DeclContext, FromTy*, FromTy*> {
  static const ::flang::DeclContext *doit(const FromTy *Val) {
    return FromTy::castToDeclContext(Val);
  }
};

} // end namespace llvm

#endif // FLANG_AST_DECL_H__
