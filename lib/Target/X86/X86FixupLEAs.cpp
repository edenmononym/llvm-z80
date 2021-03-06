//===-- X86FixupLEAs.cpp - use or replace LEA instructions -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the pass which will find  instructions  which
// can be re-written as LEA instructions in order to reduce pipeline
// delays for some models of the Intel Atom family.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "x86-fixup-LEAs"
#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
using namespace llvm;

STATISTIC(NumLEAs, "Number of LEA instructions created");

namespace {
  class FixupLEAPass : public MachineFunctionPass {
    enum RegUsageState { RU_NotUsed, RU_Write, RU_Read };
    static char ID;
    /// \brief Loop over all of the instructions in the basic block
    /// replacing applicable instructions with LEA instructions,
    /// where appropriate.
    bool processBasicBlock(MachineFunction &MF, MachineFunction::iterator MFI);

    virtual const char *getPassName() const { return "X86 Atom LEA Fixup";}

    /// \brief Given a machine register, look for the instruction
    /// which writes it in the current basic block. If found,
    /// try to replace it with an equivalent LEA instruction.
    /// If replacement succeeds, then also process the the newly created
    /// instruction.
    void  seekLEAFixup(MachineOperand& p, MachineBasicBlock::iterator& I,
                      MachineFunction::iterator MFI);

    /// \brief Given a memory access or LEA instruction
    /// whose address mode uses a base and/or index register, look for
    /// an opportunity to replace the instruction which sets the base or index
    /// register with an equivalent LEA instruction.
    void processInstruction(MachineBasicBlock::iterator& I,
                            MachineFunction::iterator MFI);

    /// \brief Determine if an instruction references a machine register
    /// and, if so, whether it reads or writes the register.
    RegUsageState usesRegister(MachineOperand& p,
                               MachineBasicBlock::iterator I);

    /// \brief Step backwards through a basic block, looking
    /// for an instruction which writes a register within 
    /// a maximum of INSTR_DISTANCE_THRESHOLD instruction latency cycles.
    MachineBasicBlock::iterator searchBackwards(MachineOperand& p,
                                                MachineBasicBlock::iterator& I,
                                                MachineFunction::iterator MFI);

    /// \brief if an instruction can be converted to an 
    /// equivalent LEA, insert the new instruction into the basic block
    /// and return a pointer to it. Otherwise, return zero.
    MachineInstr* postRAConvertToLEA(MachineFunction::iterator &MFI,
                                     MachineBasicBlock::iterator &MBBI) const;

  public:
    FixupLEAPass() : MachineFunctionPass(ID) {}

    /// \brief Loop over all of the basic blocks,
    /// replacing instructions by equivalent LEA instructions
    /// if needed and when possible.
    virtual bool runOnMachineFunction(MachineFunction &MF);

  private:
    MachineFunction *MF;
    const TargetMachine *TM;
    const TargetInstrInfo *TII; // Machine instruction info.

  };
  char FixupLEAPass::ID = 0;
}

MachineInstr *
FixupLEAPass::postRAConvertToLEA(MachineFunction::iterator &MFI,
                                 MachineBasicBlock::iterator &MBBI) const {
  MachineInstr* MI = MBBI;
  MachineInstr* NewMI;
  switch (MI->getOpcode()) {
  case X86::MOV32rr: 
  case X86::MOV64rr: {
    const MachineOperand& Src = MI->getOperand(1);
    const MachineOperand& Dest = MI->getOperand(0);
    NewMI = BuildMI(*MF, MI->getDebugLoc(),
      TII->get( MI->getOpcode() == X86::MOV32rr ? X86::LEA32r : X86::LEA64r))
      .addOperand(Dest)
      .addOperand(Src).addImm(1).addReg(0).addImm(0).addReg(0);
    MFI->insert(MBBI, NewMI);   // Insert the new inst
    return NewMI;
  }
  case X86::ADD64ri32:
  case X86::ADD64ri8:
  case X86::ADD64ri32_DB:
  case X86::ADD64ri8_DB:
  case X86::ADD32ri:
  case X86::ADD32ri8:
  case X86::ADD32ri_DB:
  case X86::ADD32ri8_DB:
  case X86::ADD16ri:
  case X86::ADD16ri8:
  case X86::ADD16ri_DB:
  case X86::ADD16ri8_DB:
    if (!MI->getOperand(2).isImm()) {
      // convertToThreeAddress will call getImm()
      // which requires isImm() to be true
      return 0;
    }
    break;
  case X86::ADD16rr:
  case X86::ADD16rr_DB:
    if (MI->getOperand(1).getReg() != MI->getOperand(2).getReg()) {
      // if src1 != src2, then convertToThreeAddress will
      // need to create a Virtual register, which we cannot do
      // after register allocation.
      return 0;
    }
  }
  return TII->convertToThreeAddress(MFI, MBBI, 0);
}

FunctionPass *llvm::createX86FixupLEAs() {
  return new FixupLEAPass();
}

bool FixupLEAPass::runOnMachineFunction(MachineFunction &Func) {
  MF = &Func;
  TM = &MF->getTarget();
  TII = TM->getInstrInfo();

  DEBUG(dbgs() << "Start X86FixupLEAs\n";);
  // Process all basic blocks.
  for (MachineFunction::iterator I = Func.begin(), E = Func.end(); I != E; ++I)
    processBasicBlock(Func, I);
  DEBUG(dbgs() << "End X86FixupLEAs\n";);

  return true;
}

FixupLEAPass::RegUsageState FixupLEAPass::usesRegister(MachineOperand& p,
                                MachineBasicBlock::iterator I) {
  RegUsageState RegUsage = RU_NotUsed;
  MachineInstr* MI = I;

  for (unsigned int i = 0; i < MI->getNumOperands(); ++i) {
    MachineOperand& opnd = MI->getOperand(i);
    if (opnd.isReg() && opnd.getReg() == p.getReg()){
      if (opnd.isDef())
        return RU_Write;
      RegUsage = RU_Read;
    }
  }
  return RegUsage;
}

/// getPreviousInstr - Given a reference to an instruction in a basic
/// block, return a reference to the previous instruction in the block,
/// wrapping around to the last instruction of the block if the block
/// branches to itself.
static inline bool getPreviousInstr(MachineBasicBlock::iterator& I,
                                    MachineFunction::iterator MFI) {
  if (I == MFI->begin()) {
    if (MFI->isPredecessor(MFI)) {
      I = --MFI->end();
      return true;
    }
    else
      return false;
  }
  --I;
  return true;
}

MachineBasicBlock::iterator FixupLEAPass::searchBackwards(MachineOperand& p,
                                   MachineBasicBlock::iterator& I,
                                   MachineFunction::iterator MFI) {
  int InstrDistance = 1;
  MachineBasicBlock::iterator CurInst;
  static const int INSTR_DISTANCE_THRESHOLD = 5;

  CurInst = I;
  bool Found;
  Found = getPreviousInstr(CurInst, MFI);
  while( Found && I != CurInst) {
    if (CurInst->isCall() || CurInst->isInlineAsm())
      break;
    if (InstrDistance > INSTR_DISTANCE_THRESHOLD)
      break; // too far back to make a difference
    if (usesRegister(p, CurInst) == RU_Write){
      return CurInst;
    }
    InstrDistance += TII->getInstrLatency(TM->getInstrItineraryData(), CurInst);
    Found = getPreviousInstr(CurInst, MFI);
  }
  return 0;
}

void FixupLEAPass::processInstruction(MachineBasicBlock::iterator& I,
                                      MachineFunction::iterator MFI) {
  // Process a load, store, or LEA instruction.
  MachineInstr *MI = I;
  int opcode = MI->getOpcode();
  const MCInstrDesc& Desc = MI->getDesc();
  int AddrOffset = X86II::getMemoryOperandNo(Desc.TSFlags, opcode);
  if (AddrOffset >= 0) {
    AddrOffset += X86II::getOperandBias(Desc);
    MachineOperand& p = MI->getOperand(AddrOffset + X86::AddrBaseReg);
    if (p.isReg() && p.getReg() != X86::ESP) {
      seekLEAFixup(p, I, MFI);
    }
    MachineOperand& q = MI->getOperand(AddrOffset + X86::AddrIndexReg);
    if (q.isReg() && q.getReg() != X86::ESP) {
      seekLEAFixup(q, I, MFI);
    }
  }
}

void FixupLEAPass::seekLEAFixup(MachineOperand& p,
                                MachineBasicBlock::iterator& I,
                                MachineFunction::iterator MFI) {
  MachineBasicBlock::iterator MBI = searchBackwards(p, I, MFI);
  if (MBI) {
    MachineInstr* NewMI = postRAConvertToLEA(MFI, MBI);
    if (NewMI) {
      ++NumLEAs;
      DEBUG(dbgs() << "Candidate to replace:"; MBI->dump(););
      // now to replace with an equivalent LEA...
      DEBUG(dbgs() << "Replaced by: "; NewMI->dump(););
      MFI->erase(MBI);
      MachineBasicBlock::iterator J =
                             static_cast<MachineBasicBlock::iterator> (NewMI);
      processInstruction(J, MFI);
    }
  }
}

bool FixupLEAPass::processBasicBlock(MachineFunction &MF,
                                     MachineFunction::iterator MFI) {

  for (MachineBasicBlock::iterator I = MFI->begin(); I != MFI->end(); ++I)
    processInstruction(I, MFI);
  return false;
}
