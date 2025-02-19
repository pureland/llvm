//===-- lib/CodeGen/GlobalISel/CallLowering.cpp - Call lowering -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements some simple delegations needed for call lowering.
///
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "call-lowering"

using namespace llvm;

void CallLowering::anchor() {}

bool CallLowering::lowerCall(MachineIRBuilder &MIRBuilder, ImmutableCallSite CS,
                             ArrayRef<Register> ResRegs,
                             ArrayRef<ArrayRef<Register>> ArgRegs,
                             Register SwiftErrorVReg,
                             std::function<unsigned()> GetCalleeReg) const {
  CallLoweringInfo Info;
  auto &DL = CS.getParent()->getParent()->getParent()->getDataLayout();

  // First step is to marshall all the function's parameters into the correct
  // physregs and memory locations. Gather the sequence of argument types that
  // we'll pass to the assigner function.
  unsigned i = 0;
  unsigned NumFixedArgs = CS.getFunctionType()->getNumParams();
  for (auto &Arg : CS.args()) {
    ArgInfo OrigArg{ArgRegs[i], Arg->getType(), ISD::ArgFlagsTy{},
                    i < NumFixedArgs};
    setArgFlags(OrigArg, i + AttributeList::FirstArgIndex, DL, CS);
    Info.OrigArgs.push_back(OrigArg);
    ++i;
  }

  if (const Function *F = CS.getCalledFunction())
    Info.Callee = MachineOperand::CreateGA(F, 0);
  else
    Info.Callee = MachineOperand::CreateReg(GetCalleeReg(), false);

  Info.OrigRet = ArgInfo{ResRegs, CS.getType(), ISD::ArgFlagsTy{}};
  if (!Info.OrigRet.Ty->isVoidTy())
    setArgFlags(Info.OrigRet, AttributeList::ReturnIndex, DL, CS);

  Info.KnownCallees =
      CS.getInstruction()->getMetadata(LLVMContext::MD_callees);
  Info.CallConv = CS.getCallingConv();
  Info.SwiftErrorVReg = SwiftErrorVReg;
  Info.IsMustTailCall = CS.isMustTailCall();

  return lowerCall(MIRBuilder, Info);
}

template <typename FuncInfoTy>
void CallLowering::setArgFlags(CallLowering::ArgInfo &Arg, unsigned OpIdx,
                               const DataLayout &DL,
                               const FuncInfoTy &FuncInfo) const {
  auto &Flags = Arg.Flags[0];
  const AttributeList &Attrs = FuncInfo.getAttributes();
  if (Attrs.hasAttribute(OpIdx, Attribute::ZExt))
    Flags.setZExt();
  if (Attrs.hasAttribute(OpIdx, Attribute::SExt))
    Flags.setSExt();
  if (Attrs.hasAttribute(OpIdx, Attribute::InReg))
    Flags.setInReg();
  if (Attrs.hasAttribute(OpIdx, Attribute::StructRet))
    Flags.setSRet();
  if (Attrs.hasAttribute(OpIdx, Attribute::SwiftSelf))
    Flags.setSwiftSelf();
  if (Attrs.hasAttribute(OpIdx, Attribute::SwiftError))
    Flags.setSwiftError();
  if (Attrs.hasAttribute(OpIdx, Attribute::ByVal))
    Flags.setByVal();
  if (Attrs.hasAttribute(OpIdx, Attribute::InAlloca))
    Flags.setInAlloca();

  if (Flags.isByVal() || Flags.isInAlloca()) {
    Type *ElementTy = cast<PointerType>(Arg.Ty)->getElementType();

    auto Ty = Attrs.getAttribute(OpIdx, Attribute::ByVal).getValueAsType();
    Flags.setByValSize(DL.getTypeAllocSize(Ty ? Ty : ElementTy));

    // For ByVal, alignment should be passed from FE.  BE will guess if
    // this info is not there but there are cases it cannot get right.
    unsigned FrameAlign;
    if (FuncInfo.getParamAlignment(OpIdx - 2))
      FrameAlign = FuncInfo.getParamAlignment(OpIdx - 2);
    else
      FrameAlign = getTLI()->getByValTypeAlignment(ElementTy, DL);
    Flags.setByValAlign(FrameAlign);
  }
  if (Attrs.hasAttribute(OpIdx, Attribute::Nest))
    Flags.setNest();
  Flags.setOrigAlign(DL.getABITypeAlignment(Arg.Ty));
}

template void
CallLowering::setArgFlags<Function>(CallLowering::ArgInfo &Arg, unsigned OpIdx,
                                    const DataLayout &DL,
                                    const Function &FuncInfo) const;

template void
CallLowering::setArgFlags<CallInst>(CallLowering::ArgInfo &Arg, unsigned OpIdx,
                                    const DataLayout &DL,
                                    const CallInst &FuncInfo) const;

Register CallLowering::packRegs(ArrayRef<Register> SrcRegs, Type *PackedTy,
                                MachineIRBuilder &MIRBuilder) const {
  assert(SrcRegs.size() > 1 && "Nothing to pack");

  const DataLayout &DL = MIRBuilder.getMF().getDataLayout();
  MachineRegisterInfo *MRI = MIRBuilder.getMRI();

  LLT PackedLLT = getLLTForType(*PackedTy, DL);

  SmallVector<LLT, 8> LLTs;
  SmallVector<uint64_t, 8> Offsets;
  computeValueLLTs(DL, *PackedTy, LLTs, &Offsets);
  assert(LLTs.size() == SrcRegs.size() && "Regs / types mismatch");

  Register Dst = MRI->createGenericVirtualRegister(PackedLLT);
  MIRBuilder.buildUndef(Dst);
  for (unsigned i = 0; i < SrcRegs.size(); ++i) {
    Register NewDst = MRI->createGenericVirtualRegister(PackedLLT);
    MIRBuilder.buildInsert(NewDst, Dst, SrcRegs[i], Offsets[i]);
    Dst = NewDst;
  }

  return Dst;
}

void CallLowering::unpackRegs(ArrayRef<Register> DstRegs, Register SrcReg,
                              Type *PackedTy,
                              MachineIRBuilder &MIRBuilder) const {
  assert(DstRegs.size() > 1 && "Nothing to unpack");

  const DataLayout &DL = MIRBuilder.getMF().getDataLayout();

  SmallVector<LLT, 8> LLTs;
  SmallVector<uint64_t, 8> Offsets;
  computeValueLLTs(DL, *PackedTy, LLTs, &Offsets);
  assert(LLTs.size() == DstRegs.size() && "Regs / types mismatch");

  for (unsigned i = 0; i < DstRegs.size(); ++i)
    MIRBuilder.buildExtract(DstRegs[i], SrcReg, Offsets[i]);
}

bool CallLowering::handleAssignments(MachineIRBuilder &MIRBuilder,
                                     SmallVectorImpl<ArgInfo> &Args,
                                     ValueHandler &Handler) const {
  MachineFunction &MF = MIRBuilder.getMF();
  const Function &F = MF.getFunction();
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(F.getCallingConv(), F.isVarArg(), MF, ArgLocs, F.getContext());
  return handleAssignments(CCInfo, ArgLocs, MIRBuilder, Args, Handler);
}

bool CallLowering::handleAssignments(CCState &CCInfo,
                                     SmallVectorImpl<CCValAssign> &ArgLocs,
                                     MachineIRBuilder &MIRBuilder,
                                     SmallVectorImpl<ArgInfo> &Args,
                                     ValueHandler &Handler) const {
  MachineFunction &MF = MIRBuilder.getMF();
  const Function &F = MF.getFunction();
  const DataLayout &DL = F.getParent()->getDataLayout();

  unsigned NumArgs = Args.size();
  for (unsigned i = 0; i != NumArgs; ++i) {
    MVT CurVT = MVT::getVT(Args[i].Ty);
    if (Handler.assignArg(i, CurVT, CurVT, CCValAssign::Full, Args[i],
                          Args[i].Flags[0], CCInfo)) {
      if (!CurVT.isValid())
        return false;
      MVT NewVT = TLI->getRegisterTypeForCallingConv(
          F.getContext(), F.getCallingConv(), EVT(CurVT));

      // If we need to split the type over multiple regs, check it's a scenario
      // we currently support.
      unsigned NumParts = TLI->getNumRegistersForCallingConv(
          F.getContext(), F.getCallingConv(), CurVT);
      if (NumParts > 1) {
        if (CurVT.isVector())
          return false;
        // For now only handle exact splits.
        if (NewVT.getSizeInBits() * NumParts != CurVT.getSizeInBits())
          return false;
      }

      // For incoming arguments (return values), we could have values in
      // physregs (or memlocs) which we want to extract and copy to vregs.
      // During this, we might have to deal with the LLT being split across
      // multiple regs, so we have to record this information for later.
      //
      // If we have outgoing args, then we have the opposite case. We have a
      // vreg with an LLT which we want to assign to a physical location, and
      // we might have to record that the value has to be split later.
      if (Handler.isIncomingArgumentHandler()) {
        if (NumParts == 1) {
          // Try to use the register type if we couldn't assign the VT.
          if (Handler.assignArg(i, NewVT, NewVT, CCValAssign::Full, Args[i],
                                Args[i].Flags[0], CCInfo))
            return false;
        } else {
          // We're handling an incoming arg which is split over multiple regs.
          // E.g. returning an s128 on AArch64.
          ISD::ArgFlagsTy OrigFlags = Args[i].Flags[0];
          Args[i].OrigRegs.push_back(Args[i].Regs[0]);
          Args[i].Regs.clear();
          Args[i].Flags.clear();
          LLT NewLLT = getLLTForMVT(NewVT);
          // For each split register, create and assign a vreg that will store
          // the incoming component of the larger value. These will later be
          // merged to form the final vreg.
          for (unsigned Part = 0; Part < NumParts; ++Part) {
            Register Reg =
                MIRBuilder.getMRI()->createGenericVirtualRegister(NewLLT);
            ISD::ArgFlagsTy Flags = OrigFlags;
            if (Part == 0) {
              Flags.setSplit();
            } else {
              Flags.setOrigAlign(1);
              if (Part == NumParts - 1)
                Flags.setSplitEnd();
            }
            Args[i].Regs.push_back(Reg);
            Args[i].Flags.push_back(Flags);
            if (Handler.assignArg(i, NewVT, NewVT, CCValAssign::Full, Args[i],
                                  Args[i].Flags[Part], CCInfo)) {
              // Still couldn't assign this smaller part type for some reason.
              return false;
            }
          }
        }
      } else {
        // Handling an outgoing arg that might need to be split.
        if (NumParts < 2)
          return false; // Don't know how to deal with this type combination.

        // This type is passed via multiple registers in the calling convention.
        // We need to extract the individual parts.
        Register LargeReg = Args[i].Regs[0];
        LLT SmallTy = LLT::scalar(NewVT.getSizeInBits());
        auto Unmerge = MIRBuilder.buildUnmerge(SmallTy, LargeReg);
        assert(Unmerge->getNumOperands() == NumParts + 1);
        ISD::ArgFlagsTy OrigFlags = Args[i].Flags[0];
        // We're going to replace the regs and flags with the split ones.
        Args[i].Regs.clear();
        Args[i].Flags.clear();
        for (unsigned PartIdx = 0; PartIdx < NumParts; ++PartIdx) {
          ISD::ArgFlagsTy Flags = OrigFlags;
          if (PartIdx == 0) {
            Flags.setSplit();
          } else {
            Flags.setOrigAlign(1);
            if (PartIdx == NumParts - 1)
              Flags.setSplitEnd();
          }
          Args[i].Regs.push_back(Unmerge.getReg(PartIdx));
          Args[i].Flags.push_back(Flags);
          if (Handler.assignArg(i, NewVT, NewVT, CCValAssign::Full, Args[i],
                                Args[i].Flags[PartIdx], CCInfo))
            return false;
        }
      }
    }
  }

  for (unsigned i = 0, e = Args.size(), j = 0; i != e; ++i, ++j) {
    assert(j < ArgLocs.size() && "Skipped too many arg locs");

    CCValAssign &VA = ArgLocs[j];
    assert(VA.getValNo() == i && "Location doesn't correspond to current arg");

    if (VA.needsCustom()) {
      j += Handler.assignCustomValue(Args[i], makeArrayRef(ArgLocs).slice(j));
      continue;
    }

    // FIXME: Pack registers if we have more than one.
    Register ArgReg = Args[i].Regs[0];

    if (VA.isRegLoc()) {
      MVT OrigVT = MVT::getVT(Args[i].Ty);
      MVT VAVT = VA.getValVT();
      if (Handler.isIncomingArgumentHandler() && VAVT != OrigVT) {
        if (VAVT.getSizeInBits() < OrigVT.getSizeInBits()) {
          // Expected to be multiple regs for a single incoming arg.
          unsigned NumArgRegs = Args[i].Regs.size();
          if (NumArgRegs < 2)
            return false;

          assert((j + (NumArgRegs - 1)) < ArgLocs.size() &&
                 "Too many regs for number of args");
          for (unsigned Part = 0; Part < NumArgRegs; ++Part) {
            // There should be Regs.size() ArgLocs per argument.
            VA = ArgLocs[j + Part];
            Handler.assignValueToReg(Args[i].Regs[Part], VA.getLocReg(), VA);
          }
          j += NumArgRegs - 1;
          // Merge the split registers into the expected larger result vreg
          // of the original call.
          MIRBuilder.buildMerge(Args[i].OrigRegs[0], Args[i].Regs);
          continue;
        }
        const LLT VATy(VAVT);
        Register NewReg =
            MIRBuilder.getMRI()->createGenericVirtualRegister(VATy);
        Handler.assignValueToReg(NewReg, VA.getLocReg(), VA);
        // If it's a vector type, we either need to truncate the elements
        // or do an unmerge to get the lower block of elements.
        if (VATy.isVector() &&
            VATy.getNumElements() > OrigVT.getVectorNumElements()) {
          const LLT OrigTy(OrigVT);
          // Just handle the case where the VA type is 2 * original type.
          if (VATy.getNumElements() != OrigVT.getVectorNumElements() * 2) {
            LLVM_DEBUG(dbgs()
                       << "Incoming promoted vector arg has too many elts");
            return false;
          }
          auto Unmerge = MIRBuilder.buildUnmerge({OrigTy, OrigTy}, {NewReg});
          MIRBuilder.buildCopy(ArgReg, Unmerge.getReg(0));
        } else {
          MIRBuilder.buildTrunc(ArgReg, {NewReg}).getReg(0);
        }
      } else if (!Handler.isIncomingArgumentHandler()) {
        assert((j + (Args[i].Regs.size() - 1)) < ArgLocs.size() &&
               "Too many regs for number of args");
        // This is an outgoing argument that might have been split.
        for (unsigned Part = 0; Part < Args[i].Regs.size(); ++Part) {
          // There should be Regs.size() ArgLocs per argument.
          VA = ArgLocs[j + Part];
          Handler.assignValueToReg(Args[i].Regs[Part], VA.getLocReg(), VA);
        }
        j += Args[i].Regs.size() - 1;
      } else {
        Handler.assignValueToReg(ArgReg, VA.getLocReg(), VA);
      }
    } else if (VA.isMemLoc()) {
      MVT VT = MVT::getVT(Args[i].Ty);
      unsigned Size = VT == MVT::iPTR ? DL.getPointerSize()
                                      : alignTo(VT.getSizeInBits(), 8) / 8;
      unsigned Offset = VA.getLocMemOffset();
      MachinePointerInfo MPO;
      Register StackAddr = Handler.getStackAddress(Size, Offset, MPO);
      Handler.assignValueToAddress(ArgReg, StackAddr, Size, MPO, VA);
    } else {
      // FIXME: Support byvals and other weirdness
      return false;
    }
  }
  return true;
}

Register CallLowering::ValueHandler::extendRegister(Register ValReg,
                                                    CCValAssign &VA) {
  LLT LocTy{VA.getLocVT()};
  if (LocTy.getSizeInBits() == MRI.getType(ValReg).getSizeInBits())
    return ValReg;
  switch (VA.getLocInfo()) {
  default: break;
  case CCValAssign::Full:
  case CCValAssign::BCvt:
    // FIXME: bitconverting between vector types may or may not be a
    // nop in big-endian situations.
    return ValReg;
  case CCValAssign::AExt: {
    auto MIB = MIRBuilder.buildAnyExt(LocTy, ValReg);
    return MIB->getOperand(0).getReg();
  }
  case CCValAssign::SExt: {
    Register NewReg = MRI.createGenericVirtualRegister(LocTy);
    MIRBuilder.buildSExt(NewReg, ValReg);
    return NewReg;
  }
  case CCValAssign::ZExt: {
    Register NewReg = MRI.createGenericVirtualRegister(LocTy);
    MIRBuilder.buildZExt(NewReg, ValReg);
    return NewReg;
  }
  }
  llvm_unreachable("unable to extend register");
}

void CallLowering::ValueHandler::anchor() {}
