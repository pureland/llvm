//===- MipsCallLowering.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the lowering of LLVM calls to machine code calls for
/// GlobalISel.
//
//===----------------------------------------------------------------------===//

#include "MipsCallLowering.h"
#include "MipsCCState.h"
#include "MipsMachineFunction.h"
#include "MipsTargetMachine.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"

using namespace llvm;

MipsCallLowering::MipsCallLowering(const MipsTargetLowering &TLI)
    : CallLowering(&TLI) {}

bool MipsCallLowering::MipsHandler::assign(Register VReg, const CCValAssign &VA,
                                           const EVT &VT) {
  if (VA.isRegLoc()) {
    assignValueToReg(VReg, VA, VT);
  } else if (VA.isMemLoc()) {
    assignValueToAddress(VReg, VA);
  } else {
    return false;
  }
  return true;
}

bool MipsCallLowering::MipsHandler::assignVRegs(ArrayRef<Register> VRegs,
                                                ArrayRef<CCValAssign> ArgLocs,
                                                unsigned ArgLocsStartIndex,
                                                const EVT &VT) {
  for (unsigned i = 0; i < VRegs.size(); ++i)
    if (!assign(VRegs[i], ArgLocs[ArgLocsStartIndex + i], VT))
      return false;
  return true;
}

void MipsCallLowering::MipsHandler::setLeastSignificantFirst(
    SmallVectorImpl<Register> &VRegs) {
  if (!MIRBuilder.getMF().getDataLayout().isLittleEndian())
    std::reverse(VRegs.begin(), VRegs.end());
}

bool MipsCallLowering::MipsHandler::handle(
    ArrayRef<CCValAssign> ArgLocs, ArrayRef<CallLowering::ArgInfo> Args) {
  SmallVector<Register, 4> VRegs;
  unsigned SplitLength;
  const Function &F = MIRBuilder.getMF().getFunction();
  const DataLayout &DL = F.getParent()->getDataLayout();
  const MipsTargetLowering &TLI = *static_cast<const MipsTargetLowering *>(
      MIRBuilder.getMF().getSubtarget().getTargetLowering());

  for (unsigned ArgsIndex = 0, ArgLocsIndex = 0; ArgsIndex < Args.size();
       ++ArgsIndex, ArgLocsIndex += SplitLength) {
    EVT VT = TLI.getValueType(DL, Args[ArgsIndex].Ty);
    SplitLength = TLI.getNumRegistersForCallingConv(F.getContext(),
                                                    F.getCallingConv(), VT);
    assert(Args[ArgsIndex].Regs.size() == 1 && "Can't handle multple regs yet");

    if (SplitLength > 1) {
      VRegs.clear();
      MVT RegisterVT = TLI.getRegisterTypeForCallingConv(
          F.getContext(), F.getCallingConv(), VT);
      for (unsigned i = 0; i < SplitLength; ++i)
        VRegs.push_back(MRI.createGenericVirtualRegister(LLT{RegisterVT}));

      if (!handleSplit(VRegs, ArgLocs, ArgLocsIndex, Args[ArgsIndex].Regs[0],
                       VT))
        return false;
    } else {
      if (!assign(Args[ArgsIndex].Regs[0], ArgLocs[ArgLocsIndex], VT))
        return false;
    }
  }
  return true;
}

namespace {
class IncomingValueHandler : public MipsCallLowering::MipsHandler {
public:
  IncomingValueHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI)
      : MipsHandler(MIRBuilder, MRI) {}

private:
  void assignValueToReg(Register ValVReg, const CCValAssign &VA,
                        const EVT &VT) override;

  Register getStackAddress(const CCValAssign &VA,
                           MachineMemOperand *&MMO) override;

  void assignValueToAddress(Register ValVReg, const CCValAssign &VA) override;

  bool handleSplit(SmallVectorImpl<Register> &VRegs,
                   ArrayRef<CCValAssign> ArgLocs, unsigned ArgLocsStartIndex,
                   Register ArgsReg, const EVT &VT) override;

  virtual void markPhysRegUsed(unsigned PhysReg) {
    MIRBuilder.getMRI()->addLiveIn(PhysReg);
    MIRBuilder.getMBB().addLiveIn(PhysReg);
  }

  void buildLoad(Register Val, const CCValAssign &VA) {
    MachineMemOperand *MMO;
    Register Addr = getStackAddress(VA, MMO);
    MIRBuilder.buildLoad(Val, Addr, *MMO);
  }
};

class CallReturnHandler : public IncomingValueHandler {
public:
  CallReturnHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                    MachineInstrBuilder &MIB)
      : IncomingValueHandler(MIRBuilder, MRI), MIB(MIB) {}

private:
  void markPhysRegUsed(unsigned PhysReg) override {
    MIB.addDef(PhysReg, RegState::Implicit);
  }

  MachineInstrBuilder &MIB;
};

} // end anonymous namespace

void IncomingValueHandler::assignValueToReg(Register ValVReg,
                                            const CCValAssign &VA,
                                            const EVT &VT) {
  const MipsSubtarget &STI =
      static_cast<const MipsSubtarget &>(MIRBuilder.getMF().getSubtarget());
  Register PhysReg = VA.getLocReg();
  if (VT == MVT::f64 && PhysReg >= Mips::A0 && PhysReg <= Mips::A3) {
    const MipsSubtarget &STI =
        static_cast<const MipsSubtarget &>(MIRBuilder.getMF().getSubtarget());

    MIRBuilder
        .buildInstr(STI.isFP64bit() ? Mips::BuildPairF64_64
                                    : Mips::BuildPairF64)
        .addDef(ValVReg)
        .addUse(PhysReg + (STI.isLittle() ? 0 : 1))
        .addUse(PhysReg + (STI.isLittle() ? 1 : 0))
        .constrainAllUses(MIRBuilder.getTII(), *STI.getRegisterInfo(),
                          *STI.getRegBankInfo());
    markPhysRegUsed(PhysReg);
    markPhysRegUsed(PhysReg + 1);
  } else if (VT == MVT::f32 && PhysReg >= Mips::A0 && PhysReg <= Mips::A3) {
    MIRBuilder.buildInstr(Mips::MTC1)
        .addDef(ValVReg)
        .addUse(PhysReg)
        .constrainAllUses(MIRBuilder.getTII(), *STI.getRegisterInfo(),
                          *STI.getRegBankInfo());
    markPhysRegUsed(PhysReg);
  } else {
    switch (VA.getLocInfo()) {
    case CCValAssign::LocInfo::SExt:
    case CCValAssign::LocInfo::ZExt:
    case CCValAssign::LocInfo::AExt: {
      auto Copy = MIRBuilder.buildCopy(LLT{VA.getLocVT()}, PhysReg);
      MIRBuilder.buildTrunc(ValVReg, Copy);
      break;
    }
    default:
      MIRBuilder.buildCopy(ValVReg, PhysReg);
      break;
    }
    markPhysRegUsed(PhysReg);
  }
}

Register IncomingValueHandler::getStackAddress(const CCValAssign &VA,
                                               MachineMemOperand *&MMO) {
  MachineFunction &MF = MIRBuilder.getMF();
  unsigned Size = alignTo(VA.getValVT().getSizeInBits(), 8) / 8;
  unsigned Offset = VA.getLocMemOffset();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  int FI = MFI.CreateFixedObject(Size, Offset, true);
  MachinePointerInfo MPO =
      MachinePointerInfo::getFixedStack(MIRBuilder.getMF(), FI);

  const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();
  unsigned Align = MinAlign(TFL->getStackAlignment(), Offset);
  MMO = MF.getMachineMemOperand(MPO, MachineMemOperand::MOLoad, Size, Align);

  Register AddrReg = MRI.createGenericVirtualRegister(LLT::pointer(0, 32));
  MIRBuilder.buildFrameIndex(AddrReg, FI);

  return AddrReg;
}

void IncomingValueHandler::assignValueToAddress(Register ValVReg,
                                                const CCValAssign &VA) {
  if (VA.getLocInfo() == CCValAssign::SExt ||
      VA.getLocInfo() == CCValAssign::ZExt ||
      VA.getLocInfo() == CCValAssign::AExt) {
    Register LoadReg = MRI.createGenericVirtualRegister(LLT::scalar(32));
    buildLoad(LoadReg, VA);
    MIRBuilder.buildTrunc(ValVReg, LoadReg);
  } else
    buildLoad(ValVReg, VA);
}

bool IncomingValueHandler::handleSplit(SmallVectorImpl<Register> &VRegs,
                                       ArrayRef<CCValAssign> ArgLocs,
                                       unsigned ArgLocsStartIndex,
                                       Register ArgsReg, const EVT &VT) {
  if (!assignVRegs(VRegs, ArgLocs, ArgLocsStartIndex, VT))
    return false;
  setLeastSignificantFirst(VRegs);
  MIRBuilder.buildMerge(ArgsReg, VRegs);
  return true;
}

namespace {
class OutgoingValueHandler : public MipsCallLowering::MipsHandler {
public:
  OutgoingValueHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                       MachineInstrBuilder &MIB)
      : MipsHandler(MIRBuilder, MRI), MIB(MIB) {}

private:
  void assignValueToReg(Register ValVReg, const CCValAssign &VA,
                        const EVT &VT) override;

  Register getStackAddress(const CCValAssign &VA,
                           MachineMemOperand *&MMO) override;

  void assignValueToAddress(Register ValVReg, const CCValAssign &VA) override;

  bool handleSplit(SmallVectorImpl<Register> &VRegs,
                   ArrayRef<CCValAssign> ArgLocs, unsigned ArgLocsStartIndex,
                   Register ArgsReg, const EVT &VT) override;

  Register extendRegister(Register ValReg, const CCValAssign &VA);

  MachineInstrBuilder &MIB;
};
} // end anonymous namespace

void OutgoingValueHandler::assignValueToReg(Register ValVReg,
                                            const CCValAssign &VA,
                                            const EVT &VT) {
  Register PhysReg = VA.getLocReg();
  const MipsSubtarget &STI =
      static_cast<const MipsSubtarget &>(MIRBuilder.getMF().getSubtarget());

  if (VT == MVT::f64 && PhysReg >= Mips::A0 && PhysReg <= Mips::A3) {
    MIRBuilder
        .buildInstr(STI.isFP64bit() ? Mips::ExtractElementF64_64
                                    : Mips::ExtractElementF64)
        .addDef(PhysReg + (STI.isLittle() ? 1 : 0))
        .addUse(ValVReg)
        .addImm(1)
        .constrainAllUses(MIRBuilder.getTII(), *STI.getRegisterInfo(),
                          *STI.getRegBankInfo());
    MIRBuilder
        .buildInstr(STI.isFP64bit() ? Mips::ExtractElementF64_64
                                    : Mips::ExtractElementF64)
        .addDef(PhysReg + (STI.isLittle() ? 0 : 1))
        .addUse(ValVReg)
        .addImm(0)
        .constrainAllUses(MIRBuilder.getTII(), *STI.getRegisterInfo(),
                          *STI.getRegBankInfo());
  } else if (VT == MVT::f32 && PhysReg >= Mips::A0 && PhysReg <= Mips::A3) {
    MIRBuilder.buildInstr(Mips::MFC1)
        .addDef(PhysReg)
        .addUse(ValVReg)
        .constrainAllUses(MIRBuilder.getTII(), *STI.getRegisterInfo(),
                          *STI.getRegBankInfo());
  } else {
    Register ExtReg = extendRegister(ValVReg, VA);
    MIRBuilder.buildCopy(PhysReg, ExtReg);
    MIB.addUse(PhysReg, RegState::Implicit);
  }
}

Register OutgoingValueHandler::getStackAddress(const CCValAssign &VA,
                                               MachineMemOperand *&MMO) {
  MachineFunction &MF = MIRBuilder.getMF();
  const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();

  LLT p0 = LLT::pointer(0, 32);
  LLT s32 = LLT::scalar(32);
  Register SPReg = MRI.createGenericVirtualRegister(p0);
  MIRBuilder.buildCopy(SPReg, Register(Mips::SP));

  Register OffsetReg = MRI.createGenericVirtualRegister(s32);
  unsigned Offset = VA.getLocMemOffset();
  MIRBuilder.buildConstant(OffsetReg, Offset);

  Register AddrReg = MRI.createGenericVirtualRegister(p0);
  MIRBuilder.buildGEP(AddrReg, SPReg, OffsetReg);

  MachinePointerInfo MPO =
      MachinePointerInfo::getStack(MIRBuilder.getMF(), Offset);
  unsigned Size = alignTo(VA.getValVT().getSizeInBits(), 8) / 8;
  unsigned Align = MinAlign(TFL->getStackAlignment(), Offset);
  MMO = MF.getMachineMemOperand(MPO, MachineMemOperand::MOStore, Size, Align);

  return AddrReg;
}

void OutgoingValueHandler::assignValueToAddress(Register ValVReg,
                                                const CCValAssign &VA) {
  MachineMemOperand *MMO;
  Register Addr = getStackAddress(VA, MMO);
  Register ExtReg = extendRegister(ValVReg, VA);
  MIRBuilder.buildStore(ExtReg, Addr, *MMO);
}

Register OutgoingValueHandler::extendRegister(Register ValReg,
                                              const CCValAssign &VA) {
  LLT LocTy{VA.getLocVT()};
  switch (VA.getLocInfo()) {
  case CCValAssign::SExt: {
    Register ExtReg = MRI.createGenericVirtualRegister(LocTy);
    MIRBuilder.buildSExt(ExtReg, ValReg);
    return ExtReg;
  }
  case CCValAssign::ZExt: {
    Register ExtReg = MRI.createGenericVirtualRegister(LocTy);
    MIRBuilder.buildZExt(ExtReg, ValReg);
    return ExtReg;
  }
  case CCValAssign::AExt: {
    Register ExtReg = MRI.createGenericVirtualRegister(LocTy);
    MIRBuilder.buildAnyExt(ExtReg, ValReg);
    return ExtReg;
  }
  // TODO : handle upper extends
  case CCValAssign::Full:
    return ValReg;
  default:
    break;
  }
  llvm_unreachable("unable to extend register");
}

bool OutgoingValueHandler::handleSplit(SmallVectorImpl<Register> &VRegs,
                                       ArrayRef<CCValAssign> ArgLocs,
                                       unsigned ArgLocsStartIndex,
                                       Register ArgsReg, const EVT &VT) {
  MIRBuilder.buildUnmerge(VRegs, ArgsReg);
  setLeastSignificantFirst(VRegs);
  if (!assignVRegs(VRegs, ArgLocs, ArgLocsStartIndex, VT))
    return false;

  return true;
}

static bool isSupportedType(Type *T) {
  if (T->isIntegerTy())
    return true;
  if (T->isPointerTy())
    return true;
  if (T->isFloatingPointTy())
    return true;
  return false;
}

static CCValAssign::LocInfo determineLocInfo(const MVT RegisterVT, const EVT VT,
                                             const ISD::ArgFlagsTy &Flags) {
  // > does not mean loss of information as type RegisterVT can't hold type VT,
  // it means that type VT is split into multiple registers of type RegisterVT
  if (VT.getSizeInBits() >= RegisterVT.getSizeInBits())
    return CCValAssign::LocInfo::Full;
  if (Flags.isSExt())
    return CCValAssign::LocInfo::SExt;
  if (Flags.isZExt())
    return CCValAssign::LocInfo::ZExt;
  return CCValAssign::LocInfo::AExt;
}

template <typename T>
static void setLocInfo(SmallVectorImpl<CCValAssign> &ArgLocs,
                       const SmallVectorImpl<T> &Arguments) {
  for (unsigned i = 0; i < ArgLocs.size(); ++i) {
    const CCValAssign &VA = ArgLocs[i];
    CCValAssign::LocInfo LocInfo = determineLocInfo(
        Arguments[i].VT, Arguments[i].ArgVT, Arguments[i].Flags);
    if (VA.isMemLoc())
      ArgLocs[i] =
          CCValAssign::getMem(VA.getValNo(), VA.getValVT(),
                              VA.getLocMemOffset(), VA.getLocVT(), LocInfo);
    else
      ArgLocs[i] = CCValAssign::getReg(VA.getValNo(), VA.getValVT(),
                                       VA.getLocReg(), VA.getLocVT(), LocInfo);
  }
}

bool MipsCallLowering::lowerReturn(MachineIRBuilder &MIRBuilder,
                                   const Value *Val,
                                   ArrayRef<Register> VRegs) const {

  MachineInstrBuilder Ret = MIRBuilder.buildInstrNoInsert(Mips::RetRA);

  if (Val != nullptr && !isSupportedType(Val->getType()))
    return false;

  if (!VRegs.empty()) {
    MachineFunction &MF = MIRBuilder.getMF();
    const Function &F = MF.getFunction();
    const DataLayout &DL = MF.getDataLayout();
    const MipsTargetLowering &TLI = *getTLI<MipsTargetLowering>();
    LLVMContext &Ctx = Val->getType()->getContext();

    SmallVector<EVT, 4> SplitEVTs;
    ComputeValueVTs(TLI, DL, Val->getType(), SplitEVTs);
    assert(VRegs.size() == SplitEVTs.size() &&
           "For each split Type there should be exactly one VReg.");

    SmallVector<ArgInfo, 8> RetInfos;
    SmallVector<unsigned, 8> OrigArgIndices;

    for (unsigned i = 0; i < SplitEVTs.size(); ++i) {
      ArgInfo CurArgInfo = ArgInfo{VRegs[i], SplitEVTs[i].getTypeForEVT(Ctx)};
      setArgFlags(CurArgInfo, AttributeList::ReturnIndex, DL, F);
      splitToValueTypes(CurArgInfo, 0, RetInfos, OrigArgIndices);
    }

    SmallVector<ISD::OutputArg, 8> Outs;
    subTargetRegTypeForCallingConv(F, RetInfos, OrigArgIndices, Outs);

    SmallVector<CCValAssign, 16> ArgLocs;
    MipsCCState CCInfo(F.getCallingConv(), F.isVarArg(), MF, ArgLocs,
                       F.getContext());
    CCInfo.AnalyzeReturn(Outs, TLI.CCAssignFnForReturn());
    setLocInfo(ArgLocs, Outs);

    OutgoingValueHandler RetHandler(MIRBuilder, MF.getRegInfo(), Ret);
    if (!RetHandler.handle(ArgLocs, RetInfos)) {
      return false;
    }
  }
  MIRBuilder.insertInstr(Ret);
  return true;
}

bool MipsCallLowering::lowerFormalArguments(
    MachineIRBuilder &MIRBuilder, const Function &F,
    ArrayRef<ArrayRef<Register>> VRegs) const {

  // Quick exit if there aren't any args.
  if (F.arg_empty())
    return true;

  if (F.isVarArg()) {
    return false;
  }

  for (auto &Arg : F.args()) {
    if (!isSupportedType(Arg.getType()))
      return false;
  }

  MachineFunction &MF = MIRBuilder.getMF();
  const DataLayout &DL = MF.getDataLayout();
  const MipsTargetLowering &TLI = *getTLI<MipsTargetLowering>();

  SmallVector<ArgInfo, 8> ArgInfos;
  SmallVector<unsigned, 8> OrigArgIndices;
  unsigned i = 0;
  for (auto &Arg : F.args()) {
    ArgInfo AInfo(VRegs[i], Arg.getType());
    setArgFlags(AInfo, i + AttributeList::FirstArgIndex, DL, F);
    splitToValueTypes(AInfo, i, ArgInfos, OrigArgIndices);
    ++i;
  }

  SmallVector<ISD::InputArg, 8> Ins;
  subTargetRegTypeForCallingConv(F, ArgInfos, OrigArgIndices, Ins);

  SmallVector<CCValAssign, 16> ArgLocs;
  MipsCCState CCInfo(F.getCallingConv(), F.isVarArg(), MF, ArgLocs,
                     F.getContext());

  const MipsTargetMachine &TM =
      static_cast<const MipsTargetMachine &>(MF.getTarget());
  const MipsABIInfo &ABI = TM.getABI();
  CCInfo.AllocateStack(ABI.GetCalleeAllocdArgSizeInBytes(F.getCallingConv()),
                       1);
  CCInfo.AnalyzeFormalArguments(Ins, TLI.CCAssignFnForCall());
  setLocInfo(ArgLocs, Ins);

  IncomingValueHandler Handler(MIRBuilder, MF.getRegInfo());
  if (!Handler.handle(ArgLocs, ArgInfos))
    return false;

  return true;
}

bool MipsCallLowering::lowerCall(MachineIRBuilder &MIRBuilder,
                                 CallLoweringInfo &Info) const {

  if (Info.CallConv != CallingConv::C)
    return false;

  for (auto &Arg : Info.OrigArgs) {
    if (!isSupportedType(Arg.Ty))
      return false;
    if (Arg.Flags[0].isByVal() || Arg.Flags[0].isSRet())
      return false;
  }

  if (!Info.OrigRet.Ty->isVoidTy() && !isSupportedType(Info.OrigRet.Ty))
    return false;

  MachineFunction &MF = MIRBuilder.getMF();
  const Function &F = MF.getFunction();
  const MipsTargetLowering &TLI = *getTLI<MipsTargetLowering>();
  const MipsTargetMachine &TM =
      static_cast<const MipsTargetMachine &>(MF.getTarget());
  const MipsABIInfo &ABI = TM.getABI();

  MachineInstrBuilder CallSeqStart =
      MIRBuilder.buildInstr(Mips::ADJCALLSTACKDOWN);

  const bool IsCalleeGlobalPIC =
      Info.Callee.isGlobal() && TM.isPositionIndependent();

  MachineInstrBuilder MIB = MIRBuilder.buildInstrNoInsert(
      Info.Callee.isReg() || IsCalleeGlobalPIC ? Mips::JALRPseudo : Mips::JAL);
  MIB.addDef(Mips::SP, RegState::Implicit);
  if (IsCalleeGlobalPIC) {
    Register CalleeReg =
        MF.getRegInfo().createGenericVirtualRegister(LLT::pointer(0, 32));
    MachineInstr *CalleeGlobalValue =
        MIRBuilder.buildGlobalValue(CalleeReg, Info.Callee.getGlobal());
    if (!Info.Callee.getGlobal()->hasLocalLinkage())
      CalleeGlobalValue->getOperand(1).setTargetFlags(MipsII::MO_GOT_CALL);
    MIB.addUse(CalleeReg);
  } else
    MIB.add(Info.Callee);
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  MIB.addRegMask(TRI->getCallPreservedMask(MF, F.getCallingConv()));

  TargetLowering::ArgListTy FuncOrigArgs;
  FuncOrigArgs.reserve(Info.OrigArgs.size());

  SmallVector<ArgInfo, 8> ArgInfos;
  SmallVector<unsigned, 8> OrigArgIndices;
  unsigned i = 0;
  for (auto &Arg : Info.OrigArgs) {

    TargetLowering::ArgListEntry Entry;
    Entry.Ty = Arg.Ty;
    FuncOrigArgs.push_back(Entry);

    splitToValueTypes(Arg, i, ArgInfos, OrigArgIndices);
    ++i;
  }

  SmallVector<ISD::OutputArg, 8> Outs;
  subTargetRegTypeForCallingConv(F, ArgInfos, OrigArgIndices, Outs);

  SmallVector<CCValAssign, 8> ArgLocs;
  MipsCCState CCInfo(F.getCallingConv(), F.isVarArg(), MF, ArgLocs,
                     F.getContext());

  CCInfo.AllocateStack(ABI.GetCalleeAllocdArgSizeInBytes(Info.CallConv), 1);
  const char *Call =
      Info.Callee.isSymbol() ? Info.Callee.getSymbolName() : nullptr;
  CCInfo.AnalyzeCallOperands(Outs, TLI.CCAssignFnForCall(), FuncOrigArgs, Call);
  setLocInfo(ArgLocs, Outs);

  OutgoingValueHandler RetHandler(MIRBuilder, MF.getRegInfo(), MIB);
  if (!RetHandler.handle(ArgLocs, ArgInfos)) {
    return false;
  }

  unsigned NextStackOffset = CCInfo.getNextStackOffset();
  const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();
  unsigned StackAlignment = TFL->getStackAlignment();
  NextStackOffset = alignTo(NextStackOffset, StackAlignment);
  CallSeqStart.addImm(NextStackOffset).addImm(0);

  if (IsCalleeGlobalPIC) {
    MIRBuilder.buildCopy(
      Register(Mips::GP),
      MF.getInfo<MipsFunctionInfo>()->getGlobalBaseRegForGlobalISel());
    MIB.addDef(Mips::GP, RegState::Implicit);
  }
  MIRBuilder.insertInstr(MIB);
  if (MIB->getOpcode() == Mips::JALRPseudo) {
    const MipsSubtarget &STI =
        static_cast<const MipsSubtarget &>(MIRBuilder.getMF().getSubtarget());
    MIB.constrainAllUses(MIRBuilder.getTII(), *STI.getRegisterInfo(),
                         *STI.getRegBankInfo());
  }

  if (!Info.OrigRet.Ty->isVoidTy()) {
    ArgInfos.clear();
    SmallVector<unsigned, 8> OrigRetIndices;

    splitToValueTypes(Info.OrigRet, 0, ArgInfos, OrigRetIndices);

    SmallVector<ISD::InputArg, 8> Ins;
    subTargetRegTypeForCallingConv(F, ArgInfos, OrigRetIndices, Ins);

    SmallVector<CCValAssign, 8> ArgLocs;
    MipsCCState CCInfo(F.getCallingConv(), F.isVarArg(), MF, ArgLocs,
                       F.getContext());

    CCInfo.AnalyzeCallResult(Ins, TLI.CCAssignFnForReturn(), Info.OrigRet.Ty, Call);
    setLocInfo(ArgLocs, Ins);

    CallReturnHandler Handler(MIRBuilder, MF.getRegInfo(), MIB);
    if (!Handler.handle(ArgLocs, ArgInfos))
      return false;
  }

  MIRBuilder.buildInstr(Mips::ADJCALLSTACKUP).addImm(NextStackOffset).addImm(0);

  return true;
}

template <typename T>
void MipsCallLowering::subTargetRegTypeForCallingConv(
    const Function &F, ArrayRef<ArgInfo> Args,
    ArrayRef<unsigned> OrigArgIndices, SmallVectorImpl<T> &ISDArgs) const {
  const DataLayout &DL = F.getParent()->getDataLayout();
  const MipsTargetLowering &TLI = *getTLI<MipsTargetLowering>();

  unsigned ArgNo = 0;
  for (auto &Arg : Args) {

    EVT VT = TLI.getValueType(DL, Arg.Ty);
    MVT RegisterVT = TLI.getRegisterTypeForCallingConv(F.getContext(),
                                                       F.getCallingConv(), VT);
    unsigned NumRegs = TLI.getNumRegistersForCallingConv(
        F.getContext(), F.getCallingConv(), VT);

    for (unsigned i = 0; i < NumRegs; ++i) {
      ISD::ArgFlagsTy Flags = Arg.Flags[0];

      if (i == 0)
        Flags.setOrigAlign(TLI.getABIAlignmentForCallingConv(Arg.Ty, DL));
      else
        Flags.setOrigAlign(1);

      ISDArgs.emplace_back(Flags, RegisterVT, VT, true, OrigArgIndices[ArgNo],
                           0);
    }
    ++ArgNo;
  }
}

void MipsCallLowering::splitToValueTypes(
    const ArgInfo &OrigArg, unsigned OriginalIndex,
    SmallVectorImpl<ArgInfo> &SplitArgs,
    SmallVectorImpl<unsigned> &SplitArgsOrigIndices) const {

  // TODO : perform structure and array split. For now we only deal with
  // types that pass isSupportedType check.
  SplitArgs.push_back(OrigArg);
  SplitArgsOrigIndices.push_back(OriginalIndex);
}
