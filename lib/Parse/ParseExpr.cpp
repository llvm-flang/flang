//===-- ParserExpr.cpp - Fortran Expression Parser ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Fortran expression parsing.
//
//===----------------------------------------------------------------------===//

#include "flang/Parse/Parser.h"
#include "flang/Parse/ParseDiagnostic.h"
#include "flang/Sema/SemaDiagnostic.h"
#include "flang/AST/Decl.h"
#include "flang/AST/Expr.h"
#include "flang/Sema/Ownership.h"
#include "flang/Sema/Sema.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"

namespace flang {

// ParseExpression - Expressions are level-5 expresisons optionally involving
// defined binary operators.
//
//   R722:
//     expr :=
//         [ expr defined-binary-op ] level-5-expr
//
//   R723:
//     defined-binary-op :=
//         . letter [ letter ] ... .
Parser::ExprResult Parser::ParseExpression() {
  ExprResult LHS = ParseLevel5Expr();
  if (LHS.isInvalid()) return LHS;

  if (Tok.isNot(tok::defined_operator))
    return LHS;

  llvm::SMLoc OpLoc = Tok.getLocation();
  IdentifierInfo *II = Tok.getIdentifierInfo();
  Lex();

  ExprResult RHS = ParseLevel5Expr();
  if (RHS.isInvalid()) return RHS;

  return DefinedOperatorBinaryExpr::Create(Context, OpLoc, LHS, RHS, II);
}

/// \brief Looks at the next token to see if it's an expression
/// and calls ParseExpression if it is, or reports an expected expression
/// error.
ExprResult Parser::ParseExpectedFollowupExpression(const char *DiagAfter) {
  if(Tok.isAtStartOfStatement()) {
    Diag.Report(Tok.getLocation(), diag::err_expected_expression_after)
      << DiagAfter;
    return ExprError();
  }
  return ParseExpression();
}

// ParseLevel5Expr - Level-5 expressions are level-4 expressions optionally
// involving the logical operators.
//
//   R717:
//     level-5-expr :=
//         [ level-5-expr equiv-op ] equiv-operand
//   R716:
//     equiv-operand :=
//         [ equiv-operand or-op ] or-operand
//   R715:
//     or-operand :=
//         [ or-operand and-op ] and-operand
//   R714:
//     and-operand :=
//         [ not-op ] level-4-expr
//         
//   R718:
//     not-op :=
//         .NOT.
//   R719:
//     and-op :=
//         .AND.
//   R720:
//     or-op :=
//         .OR.
//   R721:
//     equiv-op :=
//         .EQV.
//      or .NEQV.
Parser::ExprResult Parser::ParseAndOperand() {
  llvm::SMLoc NotLoc = Tok.getLocation();
  bool Negate = EatIfPresent(tok::kw_NOT);

  ExprResult E = ParseLevel4Expr();
  if (E.isInvalid()) return E;

  if (Negate)
    E = Actions.ActOnUnaryExpr(Context, NotLoc, UnaryExpr::Not, E);
  return E;
}
Parser::ExprResult Parser::ParseOrOperand() {
  ExprResult E = ParseAndOperand();
  if (E.isInvalid()) return E;

  while (Tok.getKind() == tok::kw_AND) {
    llvm::SMLoc OpLoc = Tok.getLocation();
    Lex();
    ExprResult AndOp = ParseAndOperand();
    if (AndOp.isInvalid()) return AndOp;
    E = Actions.ActOnBinaryExpr(Context, OpLoc, BinaryExpr::And, E, AndOp);
  }

  return E;
}
Parser::ExprResult Parser::ParseEquivOperand() {
  ExprResult E = ParseOrOperand();
  if (E.isInvalid()) return E;

  while (Tok.getKind() == tok::kw_OR) {
    llvm::SMLoc OpLoc = Tok.getLocation();
    Lex();
    ExprResult OrOp = ParseOrOperand();
    if (OrOp.isInvalid()) return OrOp;
    E = Actions.ActOnBinaryExpr(Context, OpLoc, BinaryExpr::Or, E, OrOp);
  }

  return E;
}
Parser::ExprResult Parser::ParseLevel5Expr() {
  ExprResult E = ParseEquivOperand();
  if (E.isInvalid()) return E;

  while (true) {
    llvm::SMLoc OpLoc = Tok.getLocation();
    switch (Tok.getKind()) {
    default:
      return E;
    case tok::kw_EQV:
      Lex();
      E = Actions.ActOnBinaryExpr(Context, OpLoc, BinaryExpr::Eqv, E,
                               ParseEquivOperand());
      break;
    case tok::kw_NEQV:
      Lex();
      E = Actions.ActOnBinaryExpr(Context, OpLoc, BinaryExpr::Neqv, E,
                               ParseEquivOperand());
      break;
    }
  }
  return E;
}

// ParseLevel4Expr - Level-4 expressions are level-3 expressions optionally
// involving the relational operators.
//
//   R712:
//     level-4-expr :=
//         [ level-3-expr rel-op ] level-3-expr
//   R713:
//     rel-op :=
//         .EQ.
//      or .NE.
//      or .LT.
//      or .LE.
//      or .GT.
//      or .GE.
//      or ==
//      or /=
//      or <
//      or <=
//      or >
//      or >=
Parser::ExprResult Parser::ParseLevel4Expr() {
  ExprResult E = ParseLevel3Expr();
  if (E.isInvalid()) return E;

  while (true) {
    llvm::SMLoc OpLoc = Tok.getLocation();
    BinaryExpr::Operator Op = BinaryExpr::None;
    switch (Tok.getKind()) {
    default:
      return E;
    case tok::kw_EQ: case tok::equalequal:
      Op = BinaryExpr::Equal;
      break;
    case tok::kw_NE: case tok::slashequal:
      Op = BinaryExpr::NotEqual;
      break;
    case tok::kw_LT: case tok::less:
      Op = BinaryExpr::LessThan;
      break;
    case tok::kw_LE: case tok::lessequal:
      Op = BinaryExpr::LessThanEqual;
      break;
    case tok::kw_GT: case tok::greater:
      Op = BinaryExpr::GreaterThan;
      break;
    case tok::kw_GE: case tok::greaterequal:
      Op = BinaryExpr::GreaterThanEqual;
      break;
    }

    Lex();
    ExprResult Lvl3Expr = ParseLevel3Expr();
    if (Lvl3Expr.isInvalid()) return Lvl3Expr;
    E = Actions.ActOnBinaryExpr(Context, OpLoc, Op, E, Lvl3Expr);
  }
  return E;
}

// ParseLevel3Expr - Level-3 expressions are level-2 expressions optionally
// involving the character operator concat-op.
//
//   R710:
//     level-3-expr :=
//         [ level-3-expr concat-op ] level-2-expr
//   R711:
//     concat-op :=
//         //
Parser::ExprResult Parser::ParseLevel3Expr() {
  ExprResult E = ParseLevel2Expr();
  if (E.isInvalid()) return E;

  while (Tok.getKind() == tok::slashslash) {
    llvm::SMLoc OpLoc = Tok.getLocation();
    Lex();
    ExprResult Lvl2Expr = ParseLevel2Expr();
    if (Lvl2Expr.isInvalid()) return Lvl2Expr;
    E = Actions.ActOnBinaryExpr(Context, OpLoc, BinaryExpr::Concat, E, Lvl2Expr);
  }
  
  return E;
}

// ParseLevel2Expr - Level-2 expressions are level-1 expressions optionally
// involving the numeric operators power-op, mult-op, and add-op.
//
//   R706:
//     level-2-expr :=
//         [ [ level-2-expr ] add-op ] add-operand
//   R705:
//     add-operand :=
//         [ add-operand mult-op ] mult-operand
//   R704:
//     mult-operand :=
//         level-1-expr [ power-op mult-operand ]
//   R707:
//     power-op :=
//         **
//   R708:
//     mult-op :=
//         *
//      or /
//   R709:
//     add-op :=
//         +
//      or -
Parser::ExprResult Parser::ParseMultOperand() {
  ExprResult E = ParseLevel1Expr();
  if (E.isInvalid()) return E;

  if (Tok.getKind() == tok::starstar) {
    llvm::SMLoc OpLoc = Tok.getLocation();
    Lex();
    ExprResult MulOp = ParseMultOperand();
    if (MulOp.isInvalid()) return MulOp;
    E = Actions.ActOnBinaryExpr(Context, OpLoc, BinaryExpr::Power, E, MulOp);
  }

  return E;
}
Parser::ExprResult Parser::ParseAddOperand() {
  ExprResult E = ParseMultOperand();
  if (E.isInvalid()) return E;

  while (true) {
    llvm::SMLoc OpLoc = Tok.getLocation();
    BinaryExpr::Operator Op = BinaryExpr::None;
    switch (Tok.getKind()) {
    default:
      return E;
    case tok::star:
      Op = BinaryExpr::Multiply;
      break;
    case tok::slash:
      Op = BinaryExpr::Divide;
      break;
    }

    Lex();
    ExprResult MulOp = ParseMultOperand();
    if (MulOp.isInvalid()) return MulOp;
    E = Actions.ActOnBinaryExpr(Context, OpLoc, Op, E, MulOp);
  }
  return E;
}
Parser::ExprResult Parser::ParseLevel2Expr() {
  ExprResult E;
  llvm::SMLoc OpLoc = Tok.getLocation();
  tok::TokenKind Kind = Tok.getKind();

  if (Kind == tok::plus || Kind == tok::minus) {
    Lex(); // Eat operand.

    E = ParseAddOperand();
    if (E.isInvalid()) return E;

    if (Kind == tok::minus)
      E = Actions.ActOnUnaryExpr(Context, OpLoc, UnaryExpr::Minus, E);
    else
      E = Actions.ActOnUnaryExpr(Context, OpLoc, UnaryExpr::Plus, E);
  } else {
    E = ParseAddOperand();
    if (E.isInvalid()) return E;
  }

  while (true) {
    OpLoc = Tok.getLocation();
    BinaryExpr::Operator Op = BinaryExpr::None;
    switch (Tok.getKind()) {
    default:
      return E;
    case tok::plus:
      Op = BinaryExpr::Plus;
      break;
    case tok::minus:
      Op = BinaryExpr::Minus;
      break;
    }

    Lex();
    ExprResult AddOp = ParseAddOperand();
    if (AddOp.isInvalid()) return AddOp;
    E = Actions.ActOnBinaryExpr(Context, OpLoc, Op, E, AddOp);
  }
  return E;
}

// ParseLevel1Expr - Level-1 expressions are primaries optionally operated on by
// defined unary operators.
//
//   R702:
//     level-1-expr :=
//         [ defined-unary-op ] primary
//   R703:
//     defined-unary-op :=
//         . letter [ letter ] ... .
Parser::ExprResult Parser::ParseLevel1Expr() {
  llvm::SMLoc OpLoc = Tok.getLocation();
  IdentifierInfo *II = 0;
  if (Tok.is(tok::defined_operator)) {
    II = Tok.getIdentifierInfo();
    Lex();
  }

  ExprResult E = ParsePrimaryExpr();
  if (E.isInvalid()) return E;

  if (II)
    E = DefinedOperatorUnaryExpr::Create(Context, OpLoc, E, II);

  return E;
}

/// SetKindSelector - Set the constant expression's kind selector (if present).
void Parser::SetKindSelector(ConstantExpr *E, StringRef Kind) {
  if (Kind.empty()) return;

  SMLoc Loc; // FIXME: Need to figure out the correct kind position.
  Expr *KindExpr = 0;

  if (::isdigit(Kind[0])) {
    KindExpr = IntegerConstantExpr::Create(Context, Loc,
                                           Loc,
                                           Kind);
  } else {
    std::string KindStr(Kind);
    const IdentifierInfo *IDInfo = getIdentifierInfo(KindStr);
    VarDecl *VD = Actions.ActOnKindSelector(Context, Loc, IDInfo);
    KindExpr = VarExpr::Create(Context, Loc, VD);
  }

  E->setKindSelector(KindExpr);
}

static llvm::APFloat GetNumberConstant(ExprResult E) {
  Expr *Expr = E.get();
  if(RealConstantExpr::classof(Expr))
    return static_cast<RealConstantExpr*>(Expr)->getValue();
  else {
    assert(IntegerConstantExpr::classof(Expr));
    llvm::APInt Int = static_cast<IntegerConstantExpr*>(Expr)->getValue();
    llvm::APFloat result(llvm::APFloat::IEEEsingle);
    result.convertFromAPInt(Int,true,llvm::APFloat::rmNearestTiesToEven);
    return result;
  }
}

// Parses a complex constant
//   := (X,X)
//     X := integer-constant | real-constant
Parser::ExprResult Parser::ParseComplexConstant(llvm::SMLoc Loc) {
  ExprResult X,Y;
  X = ParsePrimaryExpr();
  Expect(tok::comma,"Expected ',' after the real part");
  Y = ParsePrimaryExpr();
  APFloat Re = GetNumberConstant(X), Im = GetNumberConstant(Y);
  return ComplexConstantExpr::Create(Context, Loc,
                                     getMaxLocationOfCurrentToken(), Re, Im);
}

// ParsePrimaryExpr - Parse a primary expression.
//
//   [R701]:
//     primary :=
//         constant
//      or designator
//      or array-constructor
//      or structure-constructor
//      or function-reference
//      or type-param-inquiry
//      or type-param-name
//      or ( expr )
Parser::ExprResult Parser::ParsePrimaryExpr(bool IsLvalue) {
  ExprResult E;
  llvm::SMLoc Loc = Tok.getLocation();

  // FIXME: Add rest of the primary expressions.
  switch (Tok.getKind()) {
  default:
    if (isaKeyword(Tok.getIdentifierInfo()->getName()))
      goto possible_keyword_as_ident;
    Diag.ReportError(Loc, "unknown unary expression");
    break;
  case tok::error:
    Lex();
    return ExprError();
  case tok::l_paren: {
    Lex();

    // Check for a complex constant
    if((Tok.is(tok::int_literal_constant) ||
       Tok.is(tok::real_literal_constant)) &&
       PeekAhead().is(tok::comma)) {
      //complex constant
      E = ParseComplexConstant(Loc);
    } else E = ParseExpression();

    if (Tok.isNot(tok::r_paren)) {
      Diag.Report(Tok.getLocation(),diag::err_expected_rparen);
      return ExprError();
    }
    Lex();
    break;
  }
  case tok::logical_literal_constant: {
    std::string NumStr;
    CleanLiteral(Tok, NumStr);

    StringRef Data(NumStr);
    std::pair<StringRef, StringRef> StrPair = Data.split('_');
    E = LogicalConstantExpr::Create(Context, Loc,
                                    getMaxLocationOfCurrentToken(),
                                    StrPair.first);
    SetKindSelector(cast<ConstantExpr>(E.get()), StrPair.second);

    Lex();
    break;
  }
  case tok::binary_boz_constant:
  case tok::octal_boz_constant:
  case tok::hex_boz_constant: {
    std::string NumStr;
    CleanLiteral(Tok, NumStr);

    StringRef Data(NumStr);
    std::pair<StringRef, StringRef> StrPair = Data.split('_');
    E = BOZConstantExpr::Create(Context, Loc,
                                getMaxLocationOfCurrentToken(),
                                StrPair.first);
    SetKindSelector(cast<ConstantExpr>(E.get()), StrPair.second);

    Lex();
    break;
  }
  case tok::char_literal_constant: {
    const Token &NextTok = PeekAhead();
    if (NextTok.is(tok::l_paren))
      // Possible substring.
      goto parse_designator;
    std::string NumStr;
    CleanLiteral(Tok, NumStr);
    E = CharacterConstantExpr::Create(Context, Loc,
                                      getMaxLocationOfCurrentToken(),
                                      StringRef(NumStr));
    Lex();
    break;
  }
  case tok::int_literal_constant: {
    std::string NumStr;
    CleanLiteral(Tok, NumStr);

    StringRef Data(NumStr);
    std::pair<StringRef, StringRef> StrPair = Data.split('_');
    E = IntegerConstantExpr::Create(Context, Loc,
                                    getMaxLocationOfCurrentToken(),
                                    StrPair.first);
    SetKindSelector(cast<ConstantExpr>(E.get()), StrPair.second);

    Lex();
    break;
  }
  case tok::real_literal_constant: {
    std::string NumStr;
    CleanLiteral(Tok, NumStr);

    StringRef Data(NumStr);
    std::pair<StringRef, StringRef> StrPair = Data.split('_');
    E = RealConstantExpr::Create(Context, Loc,
                                 getMaxLocationOfCurrentToken(),
                                 NumStr);
    SetKindSelector(cast<ConstantExpr>(E.get()), StrPair.second);

    Lex();
    break;
  }
  case tok::double_precision_literal_constant: {
    std::string NumStr;
    CleanLiteral(Tok, NumStr);
    // Replace the d/D exponent into e exponent
    for(size_t I = 0, Len = NumStr.length(); I < Len; ++I) {
      if(NumStr[I] == 'd' || NumStr[I] == 'D') {
        NumStr[I] = 'e';
        break;
      } else if(NumStr[I] == '_') break;
    }

    StringRef Data(NumStr);
    std::pair<StringRef, StringRef> StrPair = Data.split('_');
    E = DoublePrecisionConstantExpr::Create(Context, Loc,
                                            getMaxLocationOfCurrentToken(),
                                            NumStr);
    SetKindSelector(cast<ConstantExpr>(E.get()), StrPair.second);

    Lex();
    break;
  }
  case tok::identifier:
    possible_keyword_as_ident:
    parse_designator:
    E = Parser::ParseDesignator(IsLvalue);
    if (E.isInvalid()) return E;
    break;
  case tok::minus:
    Lex();
    E = Parser::ParsePrimaryExpr();
    if (E.isInvalid()) return E;
    E = Actions.ActOnUnaryExpr(Context, Loc, UnaryExpr::Minus, E);
    break;
  case tok::plus:
    Lex();
    E = Parser::ParsePrimaryExpr();
    if (E.isInvalid()) return E;
    E = Actions.ActOnUnaryExpr(Context, Loc, UnaryExpr::Plus, E);
    break;
  }

  return E;
}

/// ParseDesignator - Parse a designator. Return null if current token is not a
/// designator.
///
///   [R601]:
///     designator :=
///         object-name
///      or array-element
///      or array-section
///      or coindexed-named-object
///      or complex-part-designator
///      or structure-component
///      or substring
ExprResult Parser::ParseDesignator(bool IsLvalue) {
  if (Tok.is(tok::char_literal_constant)) {
    std::string NumStr;
    CleanLiteral(Tok, NumStr);
    ExprResult E = CharacterConstantExpr::Create(Context,
                                                 Tok.getLocation(),
                                                 getMaxLocationOfCurrentToken(),
                                                 StringRef(NumStr));
    Lex();
    // Possibly something like: '0123456789'(N:N)
    return ParseSubstring(E);
  }

  // [R504]:
  //   object-name :=
  //       name
  const IdentifierInfo *IDInfo = Tok.getIdentifierInfo();
  if (!IDInfo) return ExprError();
  VarDecl *VD = IDInfo->getFETokenInfo<VarDecl>();
  if (!VD) {
    /// Declare implicit declarations only if the expression is lvalue
    if(!IsLvalue) {
      Diag.Report(Tok.getLocation(), diag::err_undeclared_var_use)
        << IDInfo;
      Lex();
      return ExprError();
    }
    // This variable hasn't been specified before. We need to apply any IMPLICIT
    // rules to it.
    Decl *D = Actions.ActOnImplicitEntityDecl(Context, Tok.getLocation(),
                                              IDInfo);
    if (!D) {
      Lex();
      return ExprError();
    }
    VD = cast<VarDecl>(D);
  }

  ExprResult E = VarExpr::Create(Context, Tok.getLocation(), VD);
  Lex();

  if(Tok.is(tok::l_paren)){
    if(VD->getType()->isArrayType()) {
      E = ParseF77Subscript(E);
      if(Tok.is(tok::l_paren)) {
        if(VD->getType()->isArrayOfCharacterType())
          return ParseSubstring(E);
        else {
          Diag.Report(Tok.getLocation(),diag::err_unexpected_lparen);
          return ExprError();
        }
      }
      return E;
    } else if(VD->getType()->isCharacterType()) {
      return ParseSubstring(E);
    } else {
      Diag.Report(Tok.getLocation(),diag::err_unexpected_lparen);
      return ExprError();
    }
  }

  return E;
}

/// ParseArrayElement - Parse an array element.
/// 
///   R617:
///     array-element :=
///         data-ref
ExprResult Parser::ParseArrayElement() {
  ExprResult E;
  return E;
}

/// ParseArraySection - Parse a array section.
///
///   R618:
///     array-section :=
///         data-ref [ ( substring-range ) ]
///      or complex-part-designator
///   R610:
///     substring-range :=
///         [ scalar-int-expr ] : [ scalar-int-expr ]
ExprResult Parser::ParseArraySection() {
  ExprResult E;
  return E;
}

/// ParseCoindexedNamedObject - Parse a coindexed named object.
///
///   R614:
///     coindexed-named-object :=
///         data-ref
///   C620:
///     The data-ref shall contain exactly one part-re. The part-ref shall
///     contain an image-selector. The part-name shall be the name of a scalar
///     coarray.
ExprResult Parser::ParseCoindexedNamedObject() {
  ExprResult E;
  return E;
}

/// ParseComplexPartDesignator - Parse a complex part designator.
///
///   R615:
///     complex-part-designator :=
///         designator % RE
///      or designator % IM
///   C621:
///     The designator shall be of complex type.
ExprResult Parser::ParseComplexPartDesignator() {
  ExprResult E;
  return E;
}

/// ParseStructureComponent - Parse a structure component.
///
///   R613:
///     structure-component :=
///         data-ref
ExprResult Parser::ParseStructureComponent() {
  ExprResult E;
  return E;
}

/// ParseSubstring - Parse a substring.
///
///   R608:
///     substring :=
///         parent-string ( substring-range )
///   R609:
///     parent-string :=
///         scalar-variable-name
///      or array-element
///      or coindexed-named-object
///      or scalar-structure-component
///      or scalar-constant
///   R610:
///     substring-range :=
///         [ scalar-int-expr ] : [ scalar-int-expr ]
ExprResult Parser::ParseSubstring(ExprResult Target) {
  ExprResult StartingPoint, EndPoint;
  Lex();
  if(!Tok.is(tok::colon)) {
    StartingPoint = ParseExpression();
    if(!Tok.is(tok::colon)) {
      Diag.Report(Tok.getLocation(),diag::err_expected_colon);
      return ExprError();
    }
  }
  llvm::SMLoc Loc = Tok.getLocation();
  Lex();
  if(!Tok.is(tok::r_paren)) {
    EndPoint = ParseExpression();
    if(!Tok.is(tok::r_paren)) {
      Diag.Report(Tok.getLocation(),diag::err_expected_rparen);
      return ExprError();
    }
  }
  Lex();
  return Actions.ActOnSubstringExpr(Context, Loc, Target, StartingPoint, EndPoint);
}

/// ParseF77Subscript - Parse a Fortran 77 Array Subscript Expression
///
ExprResult Parser::ParseF77Subscript(ExprResult Target) {
  std::vector<ExprResult> Exprs;
  llvm::SMLoc Loc = Tok.getLocation();
  Lex();

  do {
    ExprResult E = ParseExpression();
    if(E.isInvalid()) return E;
    Exprs.push_back(E);
  } while(EatIfPresent(tok::comma));

  if(!Tok.is(tok::r_paren)) {
    Diag.Report(Tok.getLocation(),diag::err_expected_rparen);
    return ExprError();
  }
  Lex();
  return Actions.ActOnSubscriptExpr(Context, Loc, Target, Exprs);
}

/// ParseDataReference - Parse a data reference.
///
///   R611:
///     data-ref :=
///         part-ref [ % part-ref ] ...
ExprResult Parser::ParseDataReference() {
  std::vector<ExprResult> Exprs;

  do {
    ExprResult E = ParsePartReference();
    if (E.isInvalid()) return E;
    Exprs.push_back(E);
  } while (EatIfPresent(tok::percent));

  return Actions.ActOnDataReference(Exprs);
}

/// ParsePartReference - Parse the part reference.
///
///   R612:
///     part-ref :=
///         part-name [ ( section-subscript-list ) ] [ image-selector ]
///   R620:
///     section-subscript :=
///         subscript
///      or subscript-triplet
///      or vector-subscript
///   R619:
///     subscript :=
///         scalar-int-expr
///   R621:
///     subscript-triplet :=
///         [ subscript ] : [ subscript ] [ : stride ]
///   R622:
///     stride :=
///         scalar-int-expr
///   R623:
///     vector-subscript :=
///         int-expr
///   R624:
///     image-selector :=
///         lbracket cosubscript-list rbracket
///   R625:
///     cosubscript :=
///         scalar-int-expr
ExprResult Parser::ParsePartReference() {
  ExprResult E;
  return E;
}

/// Parser a variable reference
VarExpr *Parser::ParseVariableReference() {
  if(!Tok.is(tok::identifier))
    return nullptr;
  const IdentifierInfo *IDInfo = Tok.getIdentifierInfo();
  if (!IDInfo) return nullptr;
  VarDecl *VD = IDInfo->getFETokenInfo<VarDecl>();
  if (!VD) {
    // This variable hasn't been specified before. We need to apply any IMPLICIT
    // rules to it.
    Decl *D = Actions.ActOnImplicitEntityDecl(Context, Tok.getLocation(),
                                              IDInfo);
    if (!D) return nullptr;
    VD = cast<VarDecl>(D);
  }

  auto E = VarExpr::Create(Context, Tok.getLocation(), VD);
  Lex();
  return E;
}

/// Parses an integer variable reference
VarExpr *Parser::ParseIntegerVariableReference() {
  if(!Tok.is(tok::identifier))
    return nullptr;
  const IdentifierInfo *IDInfo = Tok.getIdentifierInfo();
  if (!IDInfo) return nullptr;
  VarDecl *VD = IDInfo->getFETokenInfo<VarDecl>();
  if(VD && VD->getType()->isIntegerType()) {
    Lex();
    return VarExpr::Create(Context, Tok.getLocation(), VD);
  }
  return nullptr;
}

} //namespace flang
