//===--- CGArray.cpp - Emit LLVM Code for Array operations and Expr -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit Array subscript expressions and operations.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "CGArray.h"
#include "CGValueOperations.h"
#include "flang/AST/ASTContext.h"
#include "flang/AST/ExprVisitor.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"

namespace flang {
namespace CodeGen {

llvm::Type *CodeGenTypes::GetFixedSizeArrayType(const ArrayType *T,
                                                uint64_t Size) {
  return llvm::ArrayType::get(ConvertTypeForMem(T->getElementType()),
                              Size);
}

llvm::Type *CodeGenTypes::ConvertArrayType(const ArrayType *T) {
  return llvm::PointerType::get(ConvertTypeForMem(T->getElementType()), 0);
}

llvm::Value *CodeGenFunction::CreateArrayAlloca(QualType T,
                                                const llvm::Twine &Name,
                                                bool IsTemp) {
  auto ATy = cast<ArrayType>(T.getTypePtr());
  uint64_t ArraySize;
  if(ATy->EvaluateSize(ArraySize, getContext())) {
    auto Ty = getTypes().GetFixedSizeArrayType(ATy, ArraySize);
    if(IsTemp)
      return CreateTempAlloca(Ty, Name);
    else
      return Builder.CreateAlloca(Ty, nullptr, Name);
  }
  // FIXME variable size stack/heap allocation
  return nullptr;
}

void CodeGenFunction::GetArrayDimensionsInfo(QualType T, SmallVectorImpl<ArrayDimensionValueTy> &Dims) {
  auto ATy = cast<ArrayType>(T.getTypePtr());
  auto Dimensions = ATy->getDimensions();

  for(size_t I = 0; I < Dimensions.size(); ++I) {
    llvm::Value *LB = nullptr;
    llvm::Value *UB = nullptr;
    auto LowerBound = Dimensions[I]->getLowerBoundOrNull();
    auto UpperBound = Dimensions[I]->getUpperBoundOrNull();
    if(LowerBound) {
      int64_t ConstantLowerBound;
      if(LowerBound->EvaluateAsInt(ConstantLowerBound, getContext())) {
        LB = llvm::ConstantInt::get(ConvertType(getContext().IntegerTy),
                                                ConstantLowerBound);
      } else LB = EmitScalarExpr(LowerBound);
    }
    if(UpperBound) {
      int64_t ConstantUpperBound;
      if(UpperBound->EvaluateAsInt(ConstantUpperBound, getContext())) {
        UB = llvm::ConstantInt::get(ConvertType(getContext().IntegerTy),
                                                ConstantUpperBound);
      } else UB = EmitScalarExpr(UpperBound);
    }
    Dims.push_back(ArrayDimensionValueTy(LB, UB));
  }
}

llvm::Value *CodeGenFunction::EmitDimSize(const ArrayDimensionValueTy &Dim) {
  // UB - LB + 1
  if(Dim.hasLowerBound()) {
    return Builder.CreateAdd(Builder.CreateSub(Dim.UpperBound,
                                               Dim.LowerBound),
                             llvm::ConstantInt::get(Dim.LowerBound->getType(),
                                                    1));
  }
  // UB - LB + 1 => UB - 1 + 1 => UB
  return Dim.UpperBound;
}

llvm::Value *CodeGenFunction::EmitDimSubscript(llvm::Value *Subscript,
                                               const ArrayDimensionValueTy &Dim) {
  // S - LB
  auto LB = Dim.hasLowerBound()? Dim.LowerBound :
                                 llvm::ConstantInt::get(Subscript->getType(), 1);
  return Builder.CreateSub(Subscript, LB);
}

llvm::Value *CodeGenFunction::EmitNthDimSubscript(llvm::Value *Subscript,
                                                  const ArrayDimensionValueTy &Dim,
                                                  llvm::Value *DimSizeProduct) {
  // (Sn - LBn) * product of sizes of previous dimensions.
  return Builder.CreateMul(EmitDimSubscript(Subscript, Dim),
                           DimSizeProduct);
}

ArraySection CodeGenFunction::EmitDimSection(const ArrayDimensionValueTy &Dim) {
  auto Offset = Dim.hasOffset()? Dim.Offset :
                  llvm::ConstantInt::get(ConvertType(getContext().IntegerTy), 0);
  auto Size = EmitDimSize(Dim);
  return ArraySection(ArrayRangeSection(Offset, Size,
                        Dim.hasStride()? Dim.Stride : nullptr),
                      Size);
}

class ArrayValueExprEmitter
  : public ConstExprVisitor<ArrayValueExprEmitter, void> {
  CodeGenFunction &CGF;
  CGBuilderTy &Builder;
  llvm::LLVMContext &VMContext;

  SmallVector<ArrayDimensionValueTy, 8> Dims;
  llvm::Value *Ptr;
public:

  ArrayValueExprEmitter(CodeGenFunction &cgf);

  void EmitExpr(const Expr *E);
  void VisitVarExpr(const VarExpr *E);

  ArrayRef<ArrayDimensionValueTy> getResultInfo() const {
    return Dims;
  }
  llvm::Value *getResultPtr() const {
    return Ptr;
  }
};

ArrayValueExprEmitter::ArrayValueExprEmitter(CodeGenFunction &cgf)
  : CGF(cgf), Builder(cgf.getBuilder()),
    VMContext(cgf.getLLVMContext()) {
}

void ArrayValueExprEmitter::EmitExpr(const Expr *E) {
  Visit(E);
}

void ArrayValueExprEmitter::VisitVarExpr(const VarExpr *E) {
  auto VD = E->getVarDecl();
  if(CGF.IsInlinedArgument(VD))
    return EmitExpr(CGF.GetInlinedArgumentValue(VD));
  if(VD->isParameter()) {
    return; //FIXME?
  }
  if(VD->isArgument()) {
    CGF.GetArrayDimensionsInfo(VD->getType(), Dims);
    Ptr = CGF.GetVarPtr(VD);
    return;
  }
  CGF.GetArrayDimensionsInfo(VD->getType(), Dims);
  Ptr = Builder.CreateConstInBoundsGEP2_32(CGF.GetVarPtr(VD), 0, 0);
}

class ArraySectionsEmmitter
  : public ConstExprVisitor<ArraySectionsEmmitter, void> {
  CodeGenFunction &CGF;
  CGBuilderTy &Builder;
  llvm::LLVMContext &VMContext;

  SmallVector<ArraySection, 8> Sections;
  llvm::Value *Ptr;
public:

  ArraySectionsEmmitter(CodeGenFunction &cgf);

  void EmitExpr(const Expr *E);
  void VisitVarExpr(const VarExpr *E);

  ArrayRef<ArraySection> getSections() const {
    return Sections;
  }
  llvm::Value *getPointer() const {
    return Ptr;
  }
};

ArraySectionsEmmitter::ArraySectionsEmmitter(CodeGenFunction &cgf)
  : CGF(cgf), Builder(cgf.getBuilder()),
    VMContext(cgf.getLLVMContext()) {
}

void ArraySectionsEmmitter::EmitExpr(const Expr *E) {
  Visit(E);
}

void ArraySectionsEmmitter::VisitVarExpr(const VarExpr *E) {
  auto VD = E->getVarDecl();
  if(CGF.IsInlinedArgument(VD))
    return EmitExpr(CGF.GetInlinedArgumentValue(VD));
  if(VD->isParameter()) {
    return; //FIXME?
  }
  SmallVector<ArrayDimensionValueTy, 8> Dims;
  if(VD->isArgument()) {
    CGF.GetArrayDimensionsInfo(VD->getType(), Dims);
    Ptr = CGF.GetVarPtr(VD);
  } else {
    CGF.GetArrayDimensionsInfo(VD->getType(), Dims);
    Ptr = Builder.CreateConstInBoundsGEP2_32(CGF.GetVarPtr(VD), 0, 0);
  }
  for(auto I : Dims)
    Sections.push_back(CGF.EmitDimSection(I));
}

//
// Array sections emmitter for array operations.
//

ArrayValueTy ArrayOperation::getArrayValue(const Expr *E) {
  auto Arr = Arrays[E];
  return ArrayValueTy(llvm::makeArrayRef(Sections.begin() + Arr.SectionsOffset,
                        E->getType()->asArrayType()->getDimensionCount()),
                      Arr.Ptr);
}

void ArrayOperation::EmitArraySections(CodeGenFunction &CGF, const Expr *E) {
  ArraySectionsEmmitter EV(CGF);
  EV.EmitExpr(E);

  StoredArrayValue ArrayValue;
  ArrayValue.SectionsOffset = Sections.size();
  ArrayValue.Ptr = EV.getPointer();
  Arrays[E] = ArrayValue;

  for(auto S : EV.getSections())
    Sections.push_back(S);
}

class ArraySectionsGatherer : public ConstExprVisitor<ArraySectionsGatherer> {
  CodeGenFunction &CGF;
  ArrayOperation &ArrayOp;
public:

  ArraySectionsGatherer(CodeGenFunction &cgf, ArrayOperation &ArrOp)
    : CGF(cgf), ArrayOp(ArrOp) {}

  void Emit(const Expr *E);
  void VisitVarExpr(const VarExpr *E);
  void VisitImplicitCastExpr(const ImplicitCastExpr *E);
  void VisitUnaryExpr(const UnaryExpr *E);
  void VisitBinaryExpr(const BinaryExpr *E);
};

void ArraySectionsGatherer::Emit(const Expr *E) {
  if(E->getType()->isArrayType())
    Visit(E);
}

void ArraySectionsGatherer::VisitVarExpr(const VarExpr *E) {
  ArrayOp.EmitArraySections(CGF, E);
}

void ArraySectionsGatherer::VisitImplicitCastExpr(const ImplicitCastExpr *E) {
  Emit(E->getExpression());
}

void ArraySectionsGatherer::VisitUnaryExpr(const UnaryExpr *E) {
  Emit(E->getExpression());
}

void ArraySectionsGatherer::VisitBinaryExpr(const BinaryExpr *E) {
  Emit(E->getLHS());
  Emit(E->getRHS());
}

void ArrayOperation::EmitAllArraySections(CodeGenFunction &CGF, const Expr *E) {
  ArraySectionsGatherer EV(CGF, *this);
  EV.Emit(E);
}

//
// Foreach element in given sections loop emmitter for array operations
//

ArrayLoopEmmitter::ArrayLoopEmmitter(CodeGenFunction &cgf,
                                             ArrayRef<ArraySection> LHS)
  : CGF(cgf), Builder(cgf.getBuilder()), Sections(LHS) { }


llvm::Value *ArrayLoopEmmitter::EmitSectionIndex(const ArrayRangeSection &Range,
                                                 int Dimension) {
  // compute dimension index -> index = base + loop_index * stride
  return Builder.CreateAdd(Range.Offset,
                           !Range.hasStride()? Elements[Dimension] :
                             Builder.CreateMul(Elements[Dimension], Range.Stride));
}

llvm::Value *ArrayLoopEmmitter::EmitSectionIndex(const ArraySection &Section,
                                                 int Dimension) {
  if(Section.isRangeSection())
    return EmitSectionIndex(Section.getRangeSection(), Dimension);
  else
    return Section.getElementSection().Index;
}

// FIXME: add support for vector sections.
void ArrayLoopEmmitter::EmitArrayIterationBegin() {
  auto IndexType = CGF.ConvertType(CGF.getContext().IntegerTy);

  Elements.resize(Sections.size());
  Loops.resize(Sections.size());

  // Foreach section from back to front (column major
  // order for efficient memory access).
  for(auto I = Sections.size(); I!=0;) {
    --I;
    if(Sections[I].isRangeSection()) {
      auto Range = Sections[I].getRangeSection();
      auto Var = CGF.CreateTempAlloca(IndexType,"array-dim-loop-counter");
      Builder.CreateStore(llvm::ConstantInt::get(IndexType, 0), Var);
      auto LoopCond = CGF.createBasicBlock("array-dim-loop");
      auto LoopBody = CGF.createBasicBlock("array-dim-loop-body");
      auto LoopEnd = CGF.createBasicBlock("array-dim-loop-end");
      CGF.EmitBlock(LoopCond);
      Builder.CreateCondBr(Builder.CreateICmpULT(Builder.CreateLoad(Var), Range.Size),
                           LoopBody, LoopEnd);
      CGF.EmitBlock(LoopBody);
      Elements[I] = Builder.CreateLoad(Var);

      Loops[I].EndBlock = LoopEnd;
      Loops[I].TestBlock = LoopCond;
      Loops[I].Counter = Var;
    } else {
      Elements[I] = nullptr;
      Loops[I].EndBlock = nullptr;
    }
  }
}

void ArrayLoopEmmitter::EmitArrayIterationEnd() {
  auto IndexType = CGF.ConvertType(CGF.getContext().IntegerTy);

  // foreach loop from front to back.
  for(auto Loop : Loops) {
    if(Loop.EndBlock) {
      Builder.CreateStore(Builder.CreateAdd(Builder.CreateLoad(Loop.Counter),
                                            llvm::ConstantInt::get(IndexType, 1)),
                          Loop.Counter);
      CGF.EmitBranch(Loop.TestBlock);
      CGF.EmitBlock(Loop.EndBlock);
    }
  }
}

llvm::Value *ArrayLoopEmmitter::EmitElementOffset(ArrayRef<ArraySection> Sections) {
  auto Offset = EmitSectionIndex(Sections[0], 0);
  if(Sections.size() > 1) {
    auto SizeProduct = Sections[0].getDimensionSize();
    for(size_t I = 1; I < Sections.size(); ++I) {
      auto Sub = Builder.CreateMul(EmitSectionIndex(Sections[I], I),
                                   SizeProduct);
      Offset = Builder.CreateAdd(Offset, Sub);
      if((I + 1) < Sections.size())
        SizeProduct = Builder.CreateMul(SizeProduct, Sections[I].getDimensionSize());
    }
  }
  return Offset;
}

llvm::Value *ArrayLoopEmmitter::EmitElementPointer(ArrayValueTy Array) {
  return Builder.CreateGEP(Array.Ptr, EmitElementOffset(Array.Sections));
}

//
// Element expression emmitter for array operations
//

/// ArrayOperationEmmitter - Emits the array expression for the current
/// iteration of the multidimensional array loop.
template<typename T>
class ArrayOperationEmmitter : public ConstExprVisitor<ArrayOperationEmmitter<T>, T> {
  CodeGenFunction   &CGF;
  CGBuilderTy       &Builder;
  ArrayOperation    &Operation;
  ArrayLoopEmmitter &Looper;
public:

  ArrayOperationEmmitter(CodeGenFunction &cgf, ArrayOperation &Op,
                         ArrayLoopEmmitter &Loop)
    : CGF(cgf), Builder(cgf.getBuilder()), Operation(Op),
      Looper(Loop) {}

  T VisitVarExpr(const VarExpr *E);
};

template<typename T>
T ArrayOperationEmmitter<T>::VisitVarExpr(const VarExpr *E) {
  if(E->getType()->isArrayType())
    return ValueOperations<T>::EmitLoad(CGF, Looper.EmitElementPointer(Operation.getArrayValue(E)));
  return T(); // FIXME
}

template<typename T>
static void EmitArrayAssignment(CodeGenFunction &CGF, ArrayOperation &Op,
                                ArrayLoopEmmitter &Looper, ArrayValueTy LHS,
                                const Expr *RHS) {
  ArrayOperationEmmitter<T> EV(CGF, Op, Looper);
  auto Val = EV.Visit(RHS);
  ValueOperations<T>::EmitStore(CGF, Val, Looper.EmitElementPointer(LHS));
}

//
//
//

llvm::Value *CodeGenFunction::EmitArrayElementPtr(const Expr *Target,
                                                  const ArrayRef<Expr*> Subscripts) {
  ArrayValueExprEmitter EV(*this);
  EV.EmitExpr(Target);
  auto ResultDims = EV.getResultInfo();

  llvm::Value *Offset = EmitDimSubscript(EmitScalarExpr(Subscripts[0]), ResultDims[0]);
  if(Subscripts.size() > 1) {
    llvm::Value *SizeProduct = EmitDimSize(ResultDims[0]);
    for(size_t I = 1; I < Subscripts.size(); ++I) {
      auto Sub = EmitNthDimSubscript(EmitScalarExpr(Subscripts[I]),
                                     ResultDims[I], SizeProduct);
      Offset = Builder.CreateAdd(Offset, Sub);
      if((I + 1) != Subscripts.size())
        SizeProduct = Builder.CreateMul(SizeProduct, EmitDimSize(ResultDims[I]));
    }
  }
  return Builder.CreateGEP(EV.getResultPtr(), Offset);
}

llvm::Value *CodeGenFunction::EmitArrayPtr(const Expr *E) {
  ArrayValueExprEmitter EV(*this);
  EV.EmitExpr(E);
  // FIXME strided array - allocate memory and pack / unpack
  return EV.getResultPtr();
}

void CodeGenFunction::EmitArrayConstructorToKnownSizeAssignment(const ArrayType *LHSType,
                                                                uint64_t LHSSize,
                                                                llvm::Value *LHSPtr,
                                                                ArrayRef<Expr*> RHS) {
  assert(RHS.size() == LHSSize);
  for(uint64_t I = 0; I < LHSSize; ++I) {
    auto Dest = Builder.CreateConstInBoundsGEP1_64(LHSPtr, I);
    EmitAssignment(LValueTy(Dest, LHSType->getElementType()),
                   EmitRValue(RHS[I]));
  }
}

void CodeGenFunction::EmitArrayAssignment(const Expr *LHS, const Expr *RHS) {  
  auto LHSType = cast<ArrayType>(LHS->getType().getTypePtr());
  auto RHSType = RHS->getType();


  if(auto AC = dyn_cast<ArrayConstructorExpr>(RHS)) {
    uint64_t LHSSize;
    if(LHSType->EvaluateSize(LHSSize, getContext())) {
      ArrayValueExprEmitter EV(*this);
      EV.EmitExpr(LHS);
      auto Ptr = EV.getResultPtr();
      EmitArrayConstructorToKnownSizeAssignment(LHSType, LHSSize,
                                                Ptr, AC->getItems());
      return;
    }
  }

  auto ElementType = LHSType->getElementType();
  RValueTy ScalarRHS;
  if(!RHSType->isArrayType())
    ScalarRHS = EmitRValue(RHS);

  ArrayOperation OP;
  OP.EmitArraySections(*this, LHS);
  OP.EmitAllArraySections(*this, RHS);
  auto LHSArray = OP.getArrayValue(LHS);

  ArrayLoopEmmitter Looper(*this, LHSArray.Sections);
  Looper.EmitArrayIterationBegin();
  if(ScalarRHS.isNothing()) {
    // Array = array
    if(ElementType->isComplexType())
      CodeGen::EmitArrayAssignment<ComplexValueTy>(*this, OP, Looper, LHSArray, RHS);
    else
      CodeGen::EmitArrayAssignment<llvm::Value*>(*this, OP, Looper, LHSArray, RHS);
  } else {
    // Array = scalar
    auto Dest = Looper.EmitElementPointer(LHSArray);
    EmitAssignment(Dest, ScalarRHS);
  }
  Looper.EmitArrayIterationEnd();
}

}
} // end namespace flang
