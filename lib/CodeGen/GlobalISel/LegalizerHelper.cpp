//===-- llvm/CodeGen/GlobalISel/LegalizerHelper.cpp -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file This file implements the LegalizerHelper class to legalize
/// individual instructions and the LegalizeMachineIR wrapper pass for the
/// primary legalization.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "legalizer"

using namespace llvm;
using namespace LegalizeActions;

LegalizerHelper::LegalizerHelper(MachineFunction &MF)
    : MRI(MF.getRegInfo()), LI(*MF.getSubtarget().getLegalizerInfo()) {
  MIRBuilder.setMF(MF);
}

LegalizerHelper::LegalizerHelper(MachineFunction &MF, const LegalizerInfo &LI)
    : MRI(MF.getRegInfo()), LI(LI) {
  MIRBuilder.setMF(MF);
}
LegalizerHelper::LegalizeResult
LegalizerHelper::legalizeInstrStep(MachineInstr &MI) {
  LLVM_DEBUG(dbgs() << "Legalizing: "; MI.print(dbgs()));

  auto Step = LI.getAction(MI, MRI);
  switch (Step.Action) {
  case Legal:
    LLVM_DEBUG(dbgs() << ".. Already legal\n");
    return AlreadyLegal;
  case Libcall:
    LLVM_DEBUG(dbgs() << ".. Convert to libcall\n");
    return libcall(MI);
  case NarrowScalar:
    LLVM_DEBUG(dbgs() << ".. Narrow scalar\n");
    return narrowScalar(MI, Step.TypeIdx, Step.NewType);
  case WidenScalar:
    LLVM_DEBUG(dbgs() << ".. Widen scalar\n");
    return widenScalar(MI, Step.TypeIdx, Step.NewType);
  case Lower:
    LLVM_DEBUG(dbgs() << ".. Lower\n");
    return lower(MI, Step.TypeIdx, Step.NewType);
  case FewerElements:
    LLVM_DEBUG(dbgs() << ".. Reduce number of elements\n");
    return fewerElementsVector(MI, Step.TypeIdx, Step.NewType);
  case Custom:
    LLVM_DEBUG(dbgs() << ".. Custom legalization\n");
    return LI.legalizeCustom(MI, MRI, MIRBuilder) ? Legalized
                                                  : UnableToLegalize;
  default:
    LLVM_DEBUG(dbgs() << ".. Unable to legalize\n");
    return UnableToLegalize;
  }
}

void LegalizerHelper::extractParts(unsigned Reg, LLT Ty, int NumParts,
                                   SmallVectorImpl<unsigned> &VRegs) {
  for (int i = 0; i < NumParts; ++i)
    VRegs.push_back(MRI.createGenericVirtualRegister(Ty));
  MIRBuilder.buildUnmerge(VRegs, Reg);
}

static RTLIB::Libcall getRTLibDesc(unsigned Opcode, unsigned Size) {
  switch (Opcode) {
  case TargetOpcode::G_SDIV:
    assert(Size == 32 && "Unsupported size");
    return RTLIB::SDIV_I32;
  case TargetOpcode::G_UDIV:
    assert(Size == 32 && "Unsupported size");
    return RTLIB::UDIV_I32;
  case TargetOpcode::G_SREM:
    assert(Size == 32 && "Unsupported size");
    return RTLIB::SREM_I32;
  case TargetOpcode::G_UREM:
    assert(Size == 32 && "Unsupported size");
    return RTLIB::UREM_I32;
  case TargetOpcode::G_FADD:
    assert((Size == 32 || Size == 64) && "Unsupported size");
    return Size == 64 ? RTLIB::ADD_F64 : RTLIB::ADD_F32;
  case TargetOpcode::G_FSUB:
    assert((Size == 32 || Size == 64) && "Unsupported size");
    return Size == 64 ? RTLIB::SUB_F64 : RTLIB::SUB_F32;
  case TargetOpcode::G_FMUL:
    assert((Size == 32 || Size == 64) && "Unsupported size");
    return Size == 64 ? RTLIB::MUL_F64 : RTLIB::MUL_F32;
  case TargetOpcode::G_FDIV:
    assert((Size == 32 || Size == 64) && "Unsupported size");
    return Size == 64 ? RTLIB::DIV_F64 : RTLIB::DIV_F32;
  case TargetOpcode::G_FREM:
    return Size == 64 ? RTLIB::REM_F64 : RTLIB::REM_F32;
  case TargetOpcode::G_FPOW:
    return Size == 64 ? RTLIB::POW_F64 : RTLIB::POW_F32;
  case TargetOpcode::G_FMA:
    assert((Size == 32 || Size == 64) && "Unsupported size");
    return Size == 64 ? RTLIB::FMA_F64 : RTLIB::FMA_F32;
  }
  llvm_unreachable("Unknown libcall function");
}

LegalizerHelper::LegalizeResult
llvm::createLibcall(MachineIRBuilder &MIRBuilder, RTLIB::Libcall Libcall,
                    const CallLowering::ArgInfo &Result,
                    ArrayRef<CallLowering::ArgInfo> Args) {
  auto &CLI = *MIRBuilder.getMF().getSubtarget().getCallLowering();
  auto &TLI = *MIRBuilder.getMF().getSubtarget().getTargetLowering();
  const char *Name = TLI.getLibcallName(Libcall);

  MIRBuilder.getMF().getFrameInfo().setHasCalls(true);
  if (!CLI.lowerCall(MIRBuilder, TLI.getLibcallCallingConv(Libcall),
                     MachineOperand::CreateES(Name), Result, Args))
    return LegalizerHelper::UnableToLegalize;

  return LegalizerHelper::Legalized;
}

// Useful for libcalls where all operands have the same type.
static LegalizerHelper::LegalizeResult
simpleLibcall(MachineInstr &MI, MachineIRBuilder &MIRBuilder, unsigned Size,
              Type *OpType) {
  auto Libcall = getRTLibDesc(MI.getOpcode(), Size);

  SmallVector<CallLowering::ArgInfo, 3> Args;
  for (unsigned i = 1; i < MI.getNumOperands(); i++)
    Args.push_back({MI.getOperand(i).getReg(), OpType});
  return createLibcall(MIRBuilder, Libcall, {MI.getOperand(0).getReg(), OpType},
                       Args);
}

static RTLIB::Libcall getConvRTLibDesc(unsigned Opcode, Type *ToType,
                                       Type *FromType) {
  auto ToMVT = MVT::getVT(ToType);
  auto FromMVT = MVT::getVT(FromType);

  switch (Opcode) {
  case TargetOpcode::G_FPEXT:
    return RTLIB::getFPEXT(FromMVT, ToMVT);
  case TargetOpcode::G_FPTRUNC:
    return RTLIB::getFPROUND(FromMVT, ToMVT);
  case TargetOpcode::G_FPTOSI:
    return RTLIB::getFPTOSINT(FromMVT, ToMVT);
  case TargetOpcode::G_FPTOUI:
    return RTLIB::getFPTOUINT(FromMVT, ToMVT);
  case TargetOpcode::G_SITOFP:
    return RTLIB::getSINTTOFP(FromMVT, ToMVT);
  case TargetOpcode::G_UITOFP:
    return RTLIB::getUINTTOFP(FromMVT, ToMVT);
  }
  llvm_unreachable("Unsupported libcall function");
}

static LegalizerHelper::LegalizeResult
conversionLibcall(MachineInstr &MI, MachineIRBuilder &MIRBuilder, Type *ToType,
                  Type *FromType) {
  RTLIB::Libcall Libcall = getConvRTLibDesc(MI.getOpcode(), ToType, FromType);
  return createLibcall(MIRBuilder, Libcall, {MI.getOperand(0).getReg(), ToType},
                       {{MI.getOperand(1).getReg(), FromType}});
}

LegalizerHelper::LegalizeResult
LegalizerHelper::libcall(MachineInstr &MI) {
  LLT LLTy = MRI.getType(MI.getOperand(0).getReg());
  unsigned Size = LLTy.getSizeInBits();
  auto &Ctx = MIRBuilder.getMF().getFunction().getContext();

  MIRBuilder.setInstr(MI);

  switch (MI.getOpcode()) {
  default:
    return UnableToLegalize;
  case TargetOpcode::G_SDIV:
  case TargetOpcode::G_UDIV:
  case TargetOpcode::G_SREM:
  case TargetOpcode::G_UREM: {
    Type *HLTy = Type::getInt32Ty(Ctx);
    auto Status = simpleLibcall(MI, MIRBuilder, Size, HLTy);
    if (Status != Legalized)
      return Status;
    break;
  }
  case TargetOpcode::G_FADD:
  case TargetOpcode::G_FSUB:
  case TargetOpcode::G_FMUL:
  case TargetOpcode::G_FDIV:
  case TargetOpcode::G_FMA:
  case TargetOpcode::G_FPOW:
  case TargetOpcode::G_FREM: {
    Type *HLTy = Size == 64 ? Type::getDoubleTy(Ctx) : Type::getFloatTy(Ctx);
    auto Status = simpleLibcall(MI, MIRBuilder, Size, HLTy);
    if (Status != Legalized)
      return Status;
    break;
  }
  case TargetOpcode::G_FPEXT: {
    // FIXME: Support other floating point types (half, fp128 etc)
    unsigned FromSize = MRI.getType(MI.getOperand(1).getReg()).getSizeInBits();
    unsigned ToSize = MRI.getType(MI.getOperand(0).getReg()).getSizeInBits();
    if (ToSize != 64 || FromSize != 32)
      return UnableToLegalize;
    LegalizeResult Status = conversionLibcall(
        MI, MIRBuilder, Type::getDoubleTy(Ctx), Type::getFloatTy(Ctx));
    if (Status != Legalized)
      return Status;
    break;
  }
  case TargetOpcode::G_FPTRUNC: {
    // FIXME: Support other floating point types (half, fp128 etc)
    unsigned FromSize = MRI.getType(MI.getOperand(1).getReg()).getSizeInBits();
    unsigned ToSize = MRI.getType(MI.getOperand(0).getReg()).getSizeInBits();
    if (ToSize != 32 || FromSize != 64)
      return UnableToLegalize;
    LegalizeResult Status = conversionLibcall(
        MI, MIRBuilder, Type::getFloatTy(Ctx), Type::getDoubleTy(Ctx));
    if (Status != Legalized)
      return Status;
    break;
  }
  case TargetOpcode::G_FPTOSI:
  case TargetOpcode::G_FPTOUI: {
    // FIXME: Support other types
    unsigned FromSize = MRI.getType(MI.getOperand(1).getReg()).getSizeInBits();
    unsigned ToSize = MRI.getType(MI.getOperand(0).getReg()).getSizeInBits();
    if (ToSize != 32 || (FromSize != 32 && FromSize != 64))
      return UnableToLegalize;
    LegalizeResult Status = conversionLibcall(
        MI, MIRBuilder, Type::getInt32Ty(Ctx),
        FromSize == 64 ? Type::getDoubleTy(Ctx) : Type::getFloatTy(Ctx));
    if (Status != Legalized)
      return Status;
    break;
  }
  case TargetOpcode::G_SITOFP:
  case TargetOpcode::G_UITOFP: {
    // FIXME: Support other types
    unsigned FromSize = MRI.getType(MI.getOperand(1).getReg()).getSizeInBits();
    unsigned ToSize = MRI.getType(MI.getOperand(0).getReg()).getSizeInBits();
    if (FromSize != 32 || (ToSize != 32 && ToSize != 64))
      return UnableToLegalize;
    LegalizeResult Status = conversionLibcall(
        MI, MIRBuilder,
        ToSize == 64 ? Type::getDoubleTy(Ctx) : Type::getFloatTy(Ctx),
        Type::getInt32Ty(Ctx));
    if (Status != Legalized)
      return Status;
    break;
  }
  }

  MI.eraseFromParent();
  return Legalized;
}

LegalizerHelper::LegalizeResult LegalizerHelper::narrowScalar(MachineInstr &MI,
                                                              unsigned TypeIdx,
                                                              LLT NarrowTy) {
  // FIXME: Don't know how to handle secondary types yet.
  if (TypeIdx != 0 && MI.getOpcode() != TargetOpcode::G_EXTRACT)
    return UnableToLegalize;

  MIRBuilder.setInstr(MI);

  uint64_t SizeOp0 = MRI.getType(MI.getOperand(0).getReg()).getSizeInBits();
  uint64_t NarrowSize = NarrowTy.getSizeInBits();

  switch (MI.getOpcode()) {
  default:
    return UnableToLegalize;
  case TargetOpcode::G_IMPLICIT_DEF: {
    // FIXME: add support for when SizeOp0 isn't an exact multiple of
    // NarrowSize.
    if (SizeOp0 % NarrowSize != 0)
      return UnableToLegalize;
    int NumParts = SizeOp0 / NarrowSize;

    SmallVector<unsigned, 2> DstRegs;
    for (int i = 0; i < NumParts; ++i)
      DstRegs.push_back(
          MIRBuilder.buildUndef(NarrowTy)->getOperand(0).getReg());
    MIRBuilder.buildMerge(MI.getOperand(0).getReg(), DstRegs);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_ADD: {
    // FIXME: add support for when SizeOp0 isn't an exact multiple of
    // NarrowSize.
    if (SizeOp0 % NarrowSize != 0)
      return UnableToLegalize;
    // Expand in terms of carry-setting/consuming G_ADDE instructions.
    int NumParts = SizeOp0 / NarrowTy.getSizeInBits();

    SmallVector<unsigned, 2> Src1Regs, Src2Regs, DstRegs;
    extractParts(MI.getOperand(1).getReg(), NarrowTy, NumParts, Src1Regs);
    extractParts(MI.getOperand(2).getReg(), NarrowTy, NumParts, Src2Regs);

    unsigned CarryIn = MRI.createGenericVirtualRegister(LLT::scalar(1));
    MIRBuilder.buildConstant(CarryIn, 0);

    for (int i = 0; i < NumParts; ++i) {
      unsigned DstReg = MRI.createGenericVirtualRegister(NarrowTy);
      unsigned CarryOut = MRI.createGenericVirtualRegister(LLT::scalar(1));

      MIRBuilder.buildUAdde(DstReg, CarryOut, Src1Regs[i],
                            Src2Regs[i], CarryIn);

      DstRegs.push_back(DstReg);
      CarryIn = CarryOut;
    }
    unsigned DstReg = MI.getOperand(0).getReg();
    MIRBuilder.buildMerge(DstReg, DstRegs);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_EXTRACT: {
    if (TypeIdx != 1)
      return UnableToLegalize;

    int64_t SizeOp1 = MRI.getType(MI.getOperand(1).getReg()).getSizeInBits();
    // FIXME: add support for when SizeOp1 isn't an exact multiple of
    // NarrowSize.
    if (SizeOp1 % NarrowSize != 0)
      return UnableToLegalize;
    int NumParts = SizeOp1 / NarrowSize;

    SmallVector<unsigned, 2> SrcRegs, DstRegs;
    SmallVector<uint64_t, 2> Indexes;
    extractParts(MI.getOperand(1).getReg(), NarrowTy, NumParts, SrcRegs);

    unsigned OpReg = MI.getOperand(0).getReg();
    uint64_t OpStart = MI.getOperand(2).getImm();
    uint64_t OpSize = MRI.getType(OpReg).getSizeInBits();
    for (int i = 0; i < NumParts; ++i) {
      unsigned SrcStart = i * NarrowSize;

      if (SrcStart + NarrowSize <= OpStart || SrcStart >= OpStart + OpSize) {
        // No part of the extract uses this subregister, ignore it.
        continue;
      } else if (SrcStart == OpStart && NarrowTy == MRI.getType(OpReg)) {
        // The entire subregister is extracted, forward the value.
        DstRegs.push_back(SrcRegs[i]);
        continue;
      }

      // OpSegStart is where this destination segment would start in OpReg if it
      // extended infinitely in both directions.
      int64_t ExtractOffset;
      uint64_t SegSize;
      if (OpStart < SrcStart) {
        ExtractOffset = 0;
        SegSize = std::min(NarrowSize, OpStart + OpSize - SrcStart);
      } else {
        ExtractOffset = OpStart - SrcStart;
        SegSize = std::min(SrcStart + NarrowSize - OpStart, OpSize);
      }

      unsigned SegReg = SrcRegs[i];
      if (ExtractOffset != 0 || SegSize != NarrowSize) {
        // A genuine extract is needed.
        SegReg = MRI.createGenericVirtualRegister(LLT::scalar(SegSize));
        MIRBuilder.buildExtract(SegReg, SrcRegs[i], ExtractOffset);
      }

      DstRegs.push_back(SegReg);
    }

    MIRBuilder.buildMerge(MI.getOperand(0).getReg(), DstRegs);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_INSERT: {
    // FIXME: add support for when SizeOp0 isn't an exact multiple of
    // NarrowSize.
    if (SizeOp0 % NarrowSize != 0)
      return UnableToLegalize;

    int NumParts = SizeOp0 / NarrowSize;

    SmallVector<unsigned, 2> SrcRegs, DstRegs;
    SmallVector<uint64_t, 2> Indexes;
    extractParts(MI.getOperand(1).getReg(), NarrowTy, NumParts, SrcRegs);

    unsigned OpReg = MI.getOperand(2).getReg();
    uint64_t OpStart = MI.getOperand(3).getImm();
    uint64_t OpSize = MRI.getType(OpReg).getSizeInBits();
    for (int i = 0; i < NumParts; ++i) {
      unsigned DstStart = i * NarrowSize;

      if (DstStart + NarrowSize <= OpStart || DstStart >= OpStart + OpSize) {
        // No part of the insert affects this subregister, forward the original.
        DstRegs.push_back(SrcRegs[i]);
        continue;
      } else if (DstStart == OpStart && NarrowTy == MRI.getType(OpReg)) {
        // The entire subregister is defined by this insert, forward the new
        // value.
        DstRegs.push_back(OpReg);
        continue;
      }

      // OpSegStart is where this destination segment would start in OpReg if it
      // extended infinitely in both directions.
      int64_t ExtractOffset, InsertOffset;
      uint64_t SegSize;
      if (OpStart < DstStart) {
        InsertOffset = 0;
        ExtractOffset = DstStart - OpStart;
        SegSize = std::min(NarrowSize, OpStart + OpSize - DstStart);
      } else {
        InsertOffset = OpStart - DstStart;
        ExtractOffset = 0;
        SegSize =
            std::min(NarrowSize - InsertOffset, OpStart + OpSize - DstStart);
      }

      unsigned SegReg = OpReg;
      if (ExtractOffset != 0 || SegSize != OpSize) {
        // A genuine extract is needed.
        SegReg = MRI.createGenericVirtualRegister(LLT::scalar(SegSize));
        MIRBuilder.buildExtract(SegReg, OpReg, ExtractOffset);
      }

      unsigned DstReg = MRI.createGenericVirtualRegister(NarrowTy);
      MIRBuilder.buildInsert(DstReg, SrcRegs[i], SegReg, InsertOffset);
      DstRegs.push_back(DstReg);
    }

    assert(DstRegs.size() == (unsigned)NumParts && "not all parts covered");
    MIRBuilder.buildMerge(MI.getOperand(0).getReg(), DstRegs);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_LOAD: {
    // FIXME: add support for when SizeOp0 isn't an exact multiple of
    // NarrowSize.
    if (SizeOp0 % NarrowSize != 0)
      return UnableToLegalize;

    const auto &MMO = **MI.memoperands_begin();
    // This implementation doesn't work for atomics. Give up instead of doing
    // something invalid.
    if (MMO.getOrdering() != AtomicOrdering::NotAtomic ||
        MMO.getFailureOrdering() != AtomicOrdering::NotAtomic)
      return UnableToLegalize;

    int NumParts = SizeOp0 / NarrowSize;
    LLT OffsetTy = LLT::scalar(
        MRI.getType(MI.getOperand(1).getReg()).getScalarSizeInBits());

    SmallVector<unsigned, 2> DstRegs;
    for (int i = 0; i < NumParts; ++i) {
      unsigned DstReg = MRI.createGenericVirtualRegister(NarrowTy);
      unsigned SrcReg = 0;
      unsigned Adjustment = i * NarrowSize / 8;

      MachineMemOperand *SplitMMO = MIRBuilder.getMF().getMachineMemOperand(
          MMO.getPointerInfo().getWithOffset(Adjustment), MMO.getFlags(),
          NarrowSize / 8, i == 0 ? MMO.getAlignment() : NarrowSize / 8,
          MMO.getAAInfo(), MMO.getRanges(), MMO.getSyncScopeID(),
          MMO.getOrdering(), MMO.getFailureOrdering());

      MIRBuilder.materializeGEP(SrcReg, MI.getOperand(1).getReg(), OffsetTy,
                                Adjustment);

      MIRBuilder.buildLoad(DstReg, SrcReg, *SplitMMO);

      DstRegs.push_back(DstReg);
    }
    unsigned DstReg = MI.getOperand(0).getReg();
    MIRBuilder.buildMerge(DstReg, DstRegs);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_STORE: {
    // FIXME: add support for when SizeOp0 isn't an exact multiple of
    // NarrowSize.
    if (SizeOp0 % NarrowSize != 0)
      return UnableToLegalize;

    const auto &MMO = **MI.memoperands_begin();
    // This implementation doesn't work for atomics. Give up instead of doing
    // something invalid.
    if (MMO.getOrdering() != AtomicOrdering::NotAtomic ||
        MMO.getFailureOrdering() != AtomicOrdering::NotAtomic)
      return UnableToLegalize;

    int NumParts = SizeOp0 / NarrowSize;
    LLT OffsetTy = LLT::scalar(
        MRI.getType(MI.getOperand(1).getReg()).getScalarSizeInBits());

    SmallVector<unsigned, 2> SrcRegs;
    extractParts(MI.getOperand(0).getReg(), NarrowTy, NumParts, SrcRegs);

    for (int i = 0; i < NumParts; ++i) {
      unsigned DstReg = 0;
      unsigned Adjustment = i * NarrowSize / 8;

      MachineMemOperand *SplitMMO = MIRBuilder.getMF().getMachineMemOperand(
          MMO.getPointerInfo().getWithOffset(Adjustment), MMO.getFlags(),
          NarrowSize / 8, i == 0 ? MMO.getAlignment() : NarrowSize / 8,
          MMO.getAAInfo(), MMO.getRanges(), MMO.getSyncScopeID(),
          MMO.getOrdering(), MMO.getFailureOrdering());

      MIRBuilder.materializeGEP(DstReg, MI.getOperand(1).getReg(), OffsetTy,
                                Adjustment);

      MIRBuilder.buildStore(SrcRegs[i], DstReg, *SplitMMO);
    }
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_CONSTANT: {
    // FIXME: add support for when SizeOp0 isn't an exact multiple of
    // NarrowSize.
    if (SizeOp0 % NarrowSize != 0)
      return UnableToLegalize;
    int NumParts = SizeOp0 / NarrowSize;
    const APInt &Cst = MI.getOperand(1).getCImm()->getValue();
    LLVMContext &Ctx = MIRBuilder.getMF().getFunction().getContext();

    SmallVector<unsigned, 2> DstRegs;
    for (int i = 0; i < NumParts; ++i) {
      unsigned DstReg = MRI.createGenericVirtualRegister(NarrowTy);
      ConstantInt *CI =
          ConstantInt::get(Ctx, Cst.lshr(NarrowSize * i).trunc(NarrowSize));
      MIRBuilder.buildConstant(DstReg, *CI);
      DstRegs.push_back(DstReg);
    }
    unsigned DstReg = MI.getOperand(0).getReg();
    MIRBuilder.buildMerge(DstReg, DstRegs);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_OR: {
    // Legalize bitwise operation:
    // A = BinOp<Ty> B, C
    // into:
    // B1, ..., BN = G_UNMERGE_VALUES B
    // C1, ..., CN = G_UNMERGE_VALUES C
    // A1 = BinOp<Ty/N> B1, C2
    // ...
    // AN = BinOp<Ty/N> BN, CN
    // A = G_MERGE_VALUES A1, ..., AN

    // FIXME: add support for when SizeOp0 isn't an exact multiple of
    // NarrowSize.
    if (SizeOp0 % NarrowSize != 0)
      return UnableToLegalize;
    int NumParts = SizeOp0 / NarrowSize;

    // List the registers where the destination will be scattered.
    SmallVector<unsigned, 2> DstRegs;
    // List the registers where the first argument will be split.
    SmallVector<unsigned, 2> SrcsReg1;
    // List the registers where the second argument will be split.
    SmallVector<unsigned, 2> SrcsReg2;
    // Create all the temporary registers.
    for (int i = 0; i < NumParts; ++i) {
      unsigned DstReg = MRI.createGenericVirtualRegister(NarrowTy);
      unsigned SrcReg1 = MRI.createGenericVirtualRegister(NarrowTy);
      unsigned SrcReg2 = MRI.createGenericVirtualRegister(NarrowTy);

      DstRegs.push_back(DstReg);
      SrcsReg1.push_back(SrcReg1);
      SrcsReg2.push_back(SrcReg2);
    }
    // Explode the big arguments into smaller chunks.
    MIRBuilder.buildUnmerge(SrcsReg1, MI.getOperand(1).getReg());
    MIRBuilder.buildUnmerge(SrcsReg2, MI.getOperand(2).getReg());

    // Do the operation on each small part.
    for (int i = 0; i < NumParts; ++i)
      MIRBuilder.buildOr(DstRegs[i], SrcsReg1[i], SrcsReg2[i]);

    // Gather the destination registers into the final destination.
    unsigned DstReg = MI.getOperand(0).getReg();
    MIRBuilder.buildMerge(DstReg, DstRegs);
    MI.eraseFromParent();
    return Legalized;
  }
  }
}

void LegalizerHelper::widenScalarSrc(MachineInstr &MI, LLT WideTy,
                                     unsigned OpIdx, unsigned ExtOpcode) {
  MachineOperand &MO = MI.getOperand(OpIdx);
  auto ExtB = MIRBuilder.buildInstr(ExtOpcode, WideTy, MO.getReg());
  MO.setReg(ExtB->getOperand(0).getReg());
}

void LegalizerHelper::widenScalarDst(MachineInstr &MI, LLT WideTy,
                                     unsigned OpIdx, unsigned TruncOpcode) {
  MachineOperand &MO = MI.getOperand(OpIdx);
  unsigned DstExt = MRI.createGenericVirtualRegister(WideTy);
  MIRBuilder.setInsertPt(MIRBuilder.getMBB(), ++MIRBuilder.getInsertPt());
  MIRBuilder.buildInstr(TruncOpcode, MO.getReg(), DstExt);
  MO.setReg(DstExt);
}

LegalizerHelper::LegalizeResult
LegalizerHelper::widenScalar(MachineInstr &MI, unsigned TypeIdx, LLT WideTy) {
  MIRBuilder.setInstr(MI);

  switch (MI.getOpcode()) {
  default:
    return UnableToLegalize;

  case TargetOpcode::G_ADD:
  case TargetOpcode::G_AND:
  case TargetOpcode::G_MUL:
  case TargetOpcode::G_OR:
  case TargetOpcode::G_XOR:
  case TargetOpcode::G_SUB:
    // Perform operation at larger width (any extension is fine here, high bits
    // don't affect the result) and then truncate the result back to the
    // original type.
    widenScalarSrc(MI, WideTy, 1, TargetOpcode::G_ANYEXT);
    widenScalarSrc(MI, WideTy, 2, TargetOpcode::G_ANYEXT);
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_SHL:
    widenScalarSrc(MI, WideTy, 1, TargetOpcode::G_ANYEXT);
    // The "number of bits to shift" operand must preserve its value as an
    // unsigned integer:
    widenScalarSrc(MI, WideTy, 2, TargetOpcode::G_ZEXT);
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_SDIV:
  case TargetOpcode::G_SREM:
    widenScalarSrc(MI, WideTy, 1, TargetOpcode::G_SEXT);
    widenScalarSrc(MI, WideTy, 2, TargetOpcode::G_SEXT);
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_ASHR:
    widenScalarSrc(MI, WideTy, 1, TargetOpcode::G_SEXT);
    // The "number of bits to shift" operand must preserve its value as an
    // unsigned integer:
    widenScalarSrc(MI, WideTy, 2, TargetOpcode::G_ZEXT);
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_UDIV:
  case TargetOpcode::G_UREM:
  case TargetOpcode::G_LSHR:
    widenScalarSrc(MI, WideTy, 1, TargetOpcode::G_ZEXT);
    widenScalarSrc(MI, WideTy, 2, TargetOpcode::G_ZEXT);
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_SELECT:
    if (TypeIdx != 0)
      return UnableToLegalize;
    // Perform operation at larger width (any extension is fine here, high bits
    // don't affect the result) and then truncate the result back to the
    // original type.
    widenScalarSrc(MI, WideTy, 2, TargetOpcode::G_ANYEXT);
    widenScalarSrc(MI, WideTy, 3, TargetOpcode::G_ANYEXT);
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_FPTOSI:
  case TargetOpcode::G_FPTOUI:
    if (TypeIdx != 0)
      return UnableToLegalize;
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_SITOFP:
    if (TypeIdx != 1)
      return UnableToLegalize;
    widenScalarSrc(MI, WideTy, 1, TargetOpcode::G_SEXT);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_UITOFP:
    if (TypeIdx != 1)
      return UnableToLegalize;
    widenScalarSrc(MI, WideTy, 1, TargetOpcode::G_ZEXT);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_INSERT:
    if (TypeIdx != 0)
      return UnableToLegalize;
    widenScalarSrc(MI, WideTy, 1, TargetOpcode::G_ANYEXT);
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_LOAD:
    // For some types like i24, we might try to widen to i32. To properly handle
    // this we should be using a dedicated extending load, until then avoid
    // trying to legalize.
    if (alignTo(MRI.getType(MI.getOperand(0).getReg()).getSizeInBits(), 8) !=
        WideTy.getSizeInBits())
      return UnableToLegalize;
    LLVM_FALLTHROUGH;
  case TargetOpcode::G_SEXTLOAD:
  case TargetOpcode::G_ZEXTLOAD:
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_STORE: {
    if (MRI.getType(MI.getOperand(0).getReg()) != LLT::scalar(1) ||
        WideTy != LLT::scalar(8))
      return UnableToLegalize;

    widenScalarSrc(MI, WideTy, 0, TargetOpcode::G_ZEXT);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;
  }
  case TargetOpcode::G_CONSTANT: {
    MachineOperand &SrcMO = MI.getOperand(1);
    LLVMContext &Ctx = MIRBuilder.getMF().getFunction().getContext();
    const APInt &Val = SrcMO.getCImm()->getValue().sext(WideTy.getSizeInBits());
    SrcMO.setCImm(ConstantInt::get(Ctx, Val));

    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;
  }
  case TargetOpcode::G_FCONSTANT: {
    MachineOperand &SrcMO = MI.getOperand(1);
    LLVMContext &Ctx = MIRBuilder.getMF().getFunction().getContext();
    APFloat Val = SrcMO.getFPImm()->getValueAPF();
    bool LosesInfo;
    switch (WideTy.getSizeInBits()) {
    case 32:
      Val.convert(APFloat::IEEEsingle(), APFloat::rmTowardZero, &LosesInfo);
      break;
    case 64:
      Val.convert(APFloat::IEEEdouble(), APFloat::rmTowardZero, &LosesInfo);
      break;
    default:
      llvm_unreachable("Unhandled fp widen type");
    }
    SrcMO.setFPImm(ConstantFP::get(Ctx, Val));

    widenScalarDst(MI, WideTy, 0, TargetOpcode::G_FPTRUNC);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;
  }
  case TargetOpcode::G_BRCOND:
    widenScalarSrc(MI, WideTy, 0, TargetOpcode::G_ANYEXT);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_FCMP:
    if (TypeIdx == 0)
      widenScalarDst(MI, WideTy);
    else {
      widenScalarSrc(MI, WideTy, 2, TargetOpcode::G_FPEXT);
      widenScalarSrc(MI, WideTy, 3, TargetOpcode::G_FPEXT);
    }
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_ICMP:
    if (TypeIdx == 0)
      widenScalarDst(MI, WideTy);
    else {
      unsigned ExtOpcode = CmpInst::isSigned(static_cast<CmpInst::Predicate>(
                               MI.getOperand(1).getPredicate()))
                               ? TargetOpcode::G_SEXT
                               : TargetOpcode::G_ZEXT;
      widenScalarSrc(MI, WideTy, 2, ExtOpcode);
      widenScalarSrc(MI, WideTy, 3, ExtOpcode);
    }
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_GEP:
    assert(TypeIdx == 1 && "unable to legalize pointer of GEP");
    widenScalarSrc(MI, WideTy, 2, TargetOpcode::G_SEXT);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;

  case TargetOpcode::G_PHI: {
    assert(TypeIdx == 0 && "Expecting only Idx 0");

    for (unsigned I = 1; I < MI.getNumOperands(); I += 2) {
      MachineBasicBlock &OpMBB = *MI.getOperand(I + 1).getMBB();
      MIRBuilder.setInsertPt(OpMBB, OpMBB.getFirstTerminator());
      widenScalarSrc(MI, WideTy, I, TargetOpcode::G_ANYEXT);
    }

    MachineBasicBlock &MBB = *MI.getParent();
    MIRBuilder.setInsertPt(MBB, --MBB.getFirstNonPHI());
    widenScalarDst(MI, WideTy);
    MIRBuilder.recordInsertion(&MI);
    return Legalized;
  }
  }
}

LegalizerHelper::LegalizeResult
LegalizerHelper::lower(MachineInstr &MI, unsigned TypeIdx, LLT Ty) {
  using namespace TargetOpcode;
  MIRBuilder.setInstr(MI);

  switch(MI.getOpcode()) {
  default:
    return UnableToLegalize;
  case TargetOpcode::G_SREM:
  case TargetOpcode::G_UREM: {
    unsigned QuotReg = MRI.createGenericVirtualRegister(Ty);
    MIRBuilder.buildInstr(MI.getOpcode() == G_SREM ? G_SDIV : G_UDIV)
        .addDef(QuotReg)
        .addUse(MI.getOperand(1).getReg())
        .addUse(MI.getOperand(2).getReg());

    unsigned ProdReg = MRI.createGenericVirtualRegister(Ty);
    MIRBuilder.buildMul(ProdReg, QuotReg, MI.getOperand(2).getReg());
    MIRBuilder.buildSub(MI.getOperand(0).getReg(), MI.getOperand(1).getReg(),
                        ProdReg);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_SMULO:
  case TargetOpcode::G_UMULO: {
    // Generate G_UMULH/G_SMULH to check for overflow and a normal G_MUL for the
    // result.
    unsigned Res = MI.getOperand(0).getReg();
    unsigned Overflow = MI.getOperand(1).getReg();
    unsigned LHS = MI.getOperand(2).getReg();
    unsigned RHS = MI.getOperand(3).getReg();

    MIRBuilder.buildMul(Res, LHS, RHS);

    unsigned Opcode = MI.getOpcode() == TargetOpcode::G_SMULO
                          ? TargetOpcode::G_SMULH
                          : TargetOpcode::G_UMULH;

    unsigned HiPart = MRI.createGenericVirtualRegister(Ty);
    MIRBuilder.buildInstr(Opcode)
      .addDef(HiPart)
      .addUse(LHS)
      .addUse(RHS);

    unsigned Zero = MRI.createGenericVirtualRegister(Ty);
    MIRBuilder.buildConstant(Zero, 0);

    // For *signed* multiply, overflow is detected by checking:
    // (hi != (lo >> bitwidth-1))
    if (Opcode == TargetOpcode::G_SMULH) {
      unsigned Shifted = MRI.createGenericVirtualRegister(Ty);
      unsigned ShiftAmt = MRI.createGenericVirtualRegister(Ty);
      MIRBuilder.buildConstant(ShiftAmt, Ty.getSizeInBits() - 1);
      MIRBuilder.buildInstr(TargetOpcode::G_ASHR)
        .addDef(Shifted)
        .addUse(Res)
        .addUse(ShiftAmt);
      MIRBuilder.buildICmp(CmpInst::ICMP_NE, Overflow, HiPart, Shifted);
    } else {
      MIRBuilder.buildICmp(CmpInst::ICMP_NE, Overflow, HiPart, Zero);
    }
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_FNEG: {
    // TODO: Handle vector types once we are able to
    // represent them.
    if (Ty.isVector())
      return UnableToLegalize;
    unsigned Res = MI.getOperand(0).getReg();
    Type *ZeroTy;
    LLVMContext &Ctx = MIRBuilder.getMF().getFunction().getContext();
    switch (Ty.getSizeInBits()) {
    case 16:
      ZeroTy = Type::getHalfTy(Ctx);
      break;
    case 32:
      ZeroTy = Type::getFloatTy(Ctx);
      break;
    case 64:
      ZeroTy = Type::getDoubleTy(Ctx);
      break;
    case 128:
      ZeroTy = Type::getFP128Ty(Ctx);
      break;
    default:
      llvm_unreachable("unexpected floating-point type");
    }
    ConstantFP &ZeroForNegation =
        *cast<ConstantFP>(ConstantFP::getZeroValueForNegation(ZeroTy));
    auto Zero = MIRBuilder.buildFConstant(Ty, ZeroForNegation);
    MIRBuilder.buildInstr(TargetOpcode::G_FSUB)
        .addDef(Res)
        .addUse(Zero->getOperand(0).getReg())
        .addUse(MI.getOperand(1).getReg());
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_FSUB: {
    // Lower (G_FSUB LHS, RHS) to (G_FADD LHS, (G_FNEG RHS)).
    // First, check if G_FNEG is marked as Lower. If so, we may
    // end up with an infinite loop as G_FSUB is used to legalize G_FNEG.
    if (LI.getAction({G_FNEG, {Ty}}).Action == Lower)
      return UnableToLegalize;
    unsigned Res = MI.getOperand(0).getReg();
    unsigned LHS = MI.getOperand(1).getReg();
    unsigned RHS = MI.getOperand(2).getReg();
    unsigned Neg = MRI.createGenericVirtualRegister(Ty);
    MIRBuilder.buildInstr(TargetOpcode::G_FNEG).addDef(Neg).addUse(RHS);
    MIRBuilder.buildInstr(TargetOpcode::G_FADD)
        .addDef(Res)
        .addUse(LHS)
        .addUse(Neg);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS: {
    unsigned OldValRes = MI.getOperand(0).getReg();
    unsigned SuccessRes = MI.getOperand(1).getReg();
    unsigned Addr = MI.getOperand(2).getReg();
    unsigned CmpVal = MI.getOperand(3).getReg();
    unsigned NewVal = MI.getOperand(4).getReg();
    MIRBuilder.buildAtomicCmpXchg(OldValRes, Addr, CmpVal, NewVal,
                                  **MI.memoperands_begin());
    MIRBuilder.buildICmp(CmpInst::ICMP_EQ, SuccessRes, OldValRes, CmpVal);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_LOAD:
  case TargetOpcode::G_SEXTLOAD:
  case TargetOpcode::G_ZEXTLOAD: {
    // Lower to a memory-width G_LOAD and a G_SEXT/G_ZEXT/G_ANYEXT
    unsigned DstReg = MI.getOperand(0).getReg();
    unsigned PtrReg = MI.getOperand(1).getReg();
    LLT DstTy = MRI.getType(DstReg);
    auto &MMO = **MI.memoperands_begin();

    if (DstTy.getSizeInBits() == MMO.getSize() /* in bytes */ * 8) {
      // In the case of G_LOAD, this was a non-extending load already and we're
      // about to lower to the same instruction.
      if (MI.getOpcode() == TargetOpcode::G_LOAD)
          return UnableToLegalize;
      MIRBuilder.buildLoad(DstReg, PtrReg, MMO);
      MI.eraseFromParent();
      return Legalized;
    }

    if (DstTy.isScalar()) {
      unsigned TmpReg = MRI.createGenericVirtualRegister(
          LLT::scalar(MMO.getSize() /* in bytes */ * 8));
      MIRBuilder.buildLoad(TmpReg, PtrReg, MMO);
      switch (MI.getOpcode()) {
      default:
        llvm_unreachable("Unexpected opcode");
      case TargetOpcode::G_LOAD:
        MIRBuilder.buildAnyExt(DstReg, TmpReg);
        break;
      case TargetOpcode::G_SEXTLOAD:
        MIRBuilder.buildSExt(DstReg, TmpReg);
        break;
      case TargetOpcode::G_ZEXTLOAD:
        MIRBuilder.buildZExt(DstReg, TmpReg);
        break;
      }
      MI.eraseFromParent();
      return Legalized;
    }

    return UnableToLegalize;
  }
  case TargetOpcode::G_CTLZ_ZERO_UNDEF:
  case TargetOpcode::G_CTTZ_ZERO_UNDEF:
  case TargetOpcode::G_CTLZ:
  case TargetOpcode::G_CTTZ:
  case TargetOpcode::G_CTPOP:
    return lowerBitCount(MI, TypeIdx, Ty);
  }
}

LegalizerHelper::LegalizeResult
LegalizerHelper::fewerElementsVector(MachineInstr &MI, unsigned TypeIdx,
                                     LLT NarrowTy) {
  // FIXME: Don't know how to handle secondary types yet.
  if (TypeIdx != 0)
    return UnableToLegalize;
  switch (MI.getOpcode()) {
  default:
    return UnableToLegalize;
  case TargetOpcode::G_ADD: {
    unsigned NarrowSize = NarrowTy.getSizeInBits();
    unsigned DstReg = MI.getOperand(0).getReg();
    unsigned Size = MRI.getType(DstReg).getSizeInBits();
    int NumParts = Size / NarrowSize;
    // FIXME: Don't know how to handle the situation where the small vectors
    // aren't all the same size yet.
    if (Size % NarrowSize != 0)
      return UnableToLegalize;

    MIRBuilder.setInstr(MI);

    SmallVector<unsigned, 2> Src1Regs, Src2Regs, DstRegs;
    extractParts(MI.getOperand(1).getReg(), NarrowTy, NumParts, Src1Regs);
    extractParts(MI.getOperand(2).getReg(), NarrowTy, NumParts, Src2Regs);

    for (int i = 0; i < NumParts; ++i) {
      unsigned DstReg = MRI.createGenericVirtualRegister(NarrowTy);
      MIRBuilder.buildAdd(DstReg, Src1Regs[i], Src2Regs[i]);
      DstRegs.push_back(DstReg);
    }

    MIRBuilder.buildMerge(DstReg, DstRegs);
    MI.eraseFromParent();
    return Legalized;
  }
  }
}

LegalizerHelper::LegalizeResult
LegalizerHelper::lowerBitCount(MachineInstr &MI, unsigned TypeIdx, LLT Ty) {
  unsigned Opc = MI.getOpcode();
  auto &TII = *MI.getMF()->getSubtarget().getInstrInfo();
  auto isLegalOrCustom = [this](const LegalityQuery &Q) {
    auto QAction = LI.getAction(Q).Action;
    return QAction == Legal || QAction == Custom;
  };
  switch (Opc) {
  default:
    return UnableToLegalize;
  case TargetOpcode::G_CTLZ_ZERO_UNDEF: {
    // This trivially expands to CTLZ.
    MI.setDesc(TII.get(TargetOpcode::G_CTLZ));
    MIRBuilder.recordInsertion(&MI);
    return Legalized;
  }
  case TargetOpcode::G_CTLZ: {
    unsigned SrcReg = MI.getOperand(1).getReg();
    unsigned Len = Ty.getSizeInBits();
    if (isLegalOrCustom({TargetOpcode::G_CTLZ_ZERO_UNDEF, {Ty}})) {
      // If CTLZ_ZERO_UNDEF is legal or custom, emit that and a select with
      // zero.
      auto MIBCtlzZU =
          MIRBuilder.buildInstr(TargetOpcode::G_CTLZ_ZERO_UNDEF, Ty, SrcReg);
      auto MIBZero = MIRBuilder.buildConstant(Ty, 0);
      auto MIBLen = MIRBuilder.buildConstant(Ty, Len);
      auto MIBICmp = MIRBuilder.buildICmp(CmpInst::ICMP_EQ, LLT::scalar(1),
                                          SrcReg, MIBZero);
      MIRBuilder.buildSelect(MI.getOperand(0).getReg(), MIBICmp, MIBLen,
                             MIBCtlzZU);
      MI.eraseFromParent();
      return Legalized;
    }
    // for now, we do this:
    // NewLen = NextPowerOf2(Len);
    // x = x | (x >> 1);
    // x = x | (x >> 2);
    // ...
    // x = x | (x >>16);
    // x = x | (x >>32); // for 64-bit input
    // Upto NewLen/2
    // return Len - popcount(x);
    //
    // Ref: "Hacker's Delight" by Henry Warren
    unsigned Op = SrcReg;
    unsigned NewLen = PowerOf2Ceil(Len);
    for (unsigned i = 0; (1U << i) <= (NewLen / 2); ++i) {
      auto MIBShiftAmt = MIRBuilder.buildConstant(Ty, 1ULL << i);
      auto MIBOp = MIRBuilder.buildInstr(
          TargetOpcode::G_OR, Ty, Op,
          MIRBuilder.buildInstr(TargetOpcode::G_LSHR, Ty, Op, MIBShiftAmt));
      Op = MIBOp->getOperand(0).getReg();
    }
    auto MIBPop = MIRBuilder.buildInstr(TargetOpcode::G_CTPOP, Ty, Op);
    MIRBuilder.buildInstr(TargetOpcode::G_SUB, MI.getOperand(0).getReg(),
                          MIRBuilder.buildConstant(Ty, Len), MIBPop);
    MI.eraseFromParent();
    return Legalized;
  }
  case TargetOpcode::G_CTTZ_ZERO_UNDEF: {
    // This trivially expands to CTTZ.
    MI.setDesc(TII.get(TargetOpcode::G_CTTZ));
    MIRBuilder.recordInsertion(&MI);
    return Legalized;
  }
  case TargetOpcode::G_CTTZ: {
    unsigned SrcReg = MI.getOperand(1).getReg();
    unsigned Len = Ty.getSizeInBits();
    if (isLegalOrCustom({TargetOpcode::G_CTTZ_ZERO_UNDEF, {Ty}})) {
      // If CTTZ_ZERO_UNDEF is legal or custom, emit that and a select with
      // zero.
      auto MIBCttzZU =
          MIRBuilder.buildInstr(TargetOpcode::G_CTTZ_ZERO_UNDEF, Ty, SrcReg);
      auto MIBZero = MIRBuilder.buildConstant(Ty, 0);
      auto MIBLen = MIRBuilder.buildConstant(Ty, Len);
      auto MIBICmp = MIRBuilder.buildICmp(CmpInst::ICMP_EQ, LLT::scalar(1),
                                          SrcReg, MIBZero);
      MIRBuilder.buildSelect(MI.getOperand(0).getReg(), MIBICmp, MIBLen,
                             MIBCttzZU);
      MI.eraseFromParent();
      return Legalized;
    }
    // for now, we use: { return popcount(~x & (x - 1)); }
    // unless the target has ctlz but not ctpop, in which case we use:
    // { return 32 - nlz(~x & (x-1)); }
    // Ref: "Hacker's Delight" by Henry Warren
    auto MIBCstNeg1 = MIRBuilder.buildConstant(Ty, -1);
    auto MIBNot =
        MIRBuilder.buildInstr(TargetOpcode::G_XOR, Ty, SrcReg, MIBCstNeg1);
    auto MIBTmp = MIRBuilder.buildInstr(
        TargetOpcode::G_AND, Ty, MIBNot,
        MIRBuilder.buildInstr(TargetOpcode::G_ADD, Ty, SrcReg, MIBCstNeg1));
    if (!isLegalOrCustom({TargetOpcode::G_CTPOP, {Ty}}) &&
        isLegalOrCustom({TargetOpcode::G_CTLZ, {Ty}})) {
      MIRBuilder.buildInstr(
          TargetOpcode::G_SUB, MI.getOperand(0).getReg(),
          MIRBuilder.buildConstant(Ty, Len),
          MIRBuilder.buildInstr(TargetOpcode::G_CTLZ, Ty, MIBTmp));
      MI.eraseFromParent();
      return Legalized;
    }
    MI.setDesc(TII.get(TargetOpcode::G_CTPOP));
    MI.getOperand(1).setReg(MIBTmp->getOperand(0).getReg());
    return Legalized;
  }
  }
}
