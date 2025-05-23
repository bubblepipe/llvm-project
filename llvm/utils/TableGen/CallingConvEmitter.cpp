//===- CallingConvEmitter.cpp - Generate calling conventions --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend is responsible for emitting descriptions of the calling
// conventions supported by this target.
//
//===----------------------------------------------------------------------===//

#include "Common/CodeGenTarget.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TGTimer.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <deque>
#include <set>

using namespace llvm;

namespace {
class CallingConvEmitter {
  const RecordKeeper &Records;
  unsigned Counter = 0u;
  std::string CurrentAction;
  bool SwiftAction = false;

  std::map<std::string, std::set<std::string>> AssignedRegsMap;
  std::map<std::string, std::set<std::string>> AssignedSwiftRegsMap;
  std::map<std::string, std::set<std::string>> DelegateToMap;

public:
  explicit CallingConvEmitter(const RecordKeeper &R) : Records(R) {}

  void run(raw_ostream &O);

private:
  void emitCallingConv(const Record *CC, raw_ostream &O);
  void emitAction(const Record *Action, indent Indent, raw_ostream &O);
  void emitArgRegisterLists(raw_ostream &O);
};
} // End anonymous namespace

void CallingConvEmitter::run(raw_ostream &O) {
  emitSourceFileHeader("Calling Convention Implementation Fragment", O);

  ArrayRef<const Record *> CCs =
      Records.getAllDerivedDefinitions("CallingConv");

  // Emit prototypes for all of the non-custom CC's so that they can forward ref
  // each other.
  Records.getTimer().startTimer("Emit prototypes");
  O << "#ifndef GET_CC_REGISTER_LISTS\n\n";
  for (const Record *CC : CCs) {
    if (!CC->getValueAsBit("Custom")) {
      unsigned Pad = CC->getName().size();
      if (CC->getValueAsBit("Entry")) {
        O << "bool llvm::";
        Pad += 12;
      } else {
        O << "static bool ";
        Pad += 13;
      }
      O << CC->getName() << "(unsigned ValNo, MVT ValVT,\n"
        << std::string(Pad, ' ') << "MVT LocVT, CCValAssign::LocInfo LocInfo,\n"
        << std::string(Pad, ' ')
        << "ISD::ArgFlagsTy ArgFlags, CCState &State);\n";
    }
  }

  // Emit each non-custom calling convention description in full.
  Records.getTimer().startTimer("Emit full descriptions");
  for (const Record *CC : CCs) {
    if (!CC->getValueAsBit("Custom")) {
      emitCallingConv(CC, O);
    }
  }

  emitArgRegisterLists(O);

  O << "\n#endif // CC_REGISTER_LIST\n";
}

void CallingConvEmitter::emitCallingConv(const Record *CC, raw_ostream &O) {
  const ListInit *CCActions = CC->getValueAsListInit("Actions");
  Counter = 0;

  CurrentAction = CC->getName().str();
  // Call upon the creation of a map entry from the void!
  // We want an entry in AssignedRegsMap for every action, even if that
  // entry is empty.
  AssignedRegsMap[CurrentAction] = {};

  O << "\n\n";
  unsigned Pad = CurrentAction.size();
  if (CC->getValueAsBit("Entry")) {
    O << "bool llvm::";
    Pad += 12;
  } else {
    O << "static bool ";
    Pad += 13;
  }
  O << CurrentAction << "(unsigned ValNo, MVT ValVT,\n"
    << std::string(Pad, ' ') << "MVT LocVT, CCValAssign::LocInfo LocInfo,\n"
    << std::string(Pad, ' ') << "ISD::ArgFlagsTy ArgFlags, CCState &State) {\n";
  // Emit all of the actions, in order.
  for (unsigned I = 0, E = CCActions->size(); I != E; ++I) {
    const Record *Action = CCActions->getElementAsRecord(I);
    SwiftAction =
        llvm::any_of(Action->getSuperClasses(), [](const Record *Class) {
          std::string Name = Class->getNameInitAsString();
          return StringRef(Name).starts_with("CCIfSwift");
        });

    O << "\n";
    emitAction(Action, indent(2), O);
  }

  O << "\n  return true; // CC didn't match.\n";
  O << "}\n";
}

void CallingConvEmitter::emitAction(const Record *Action, indent Indent,
                                    raw_ostream &O) {
  if (Action->isSubClassOf("CCPredicateAction")) {
    O << Indent << "if (";

    if (Action->isSubClassOf("CCIfType")) {
      const ListInit *VTs = Action->getValueAsListInit("VTs");
      for (unsigned I = 0, E = VTs->size(); I != E; ++I) {
        const Record *VT = VTs->getElementAsRecord(I);
        if (I != 0)
          O << " ||\n    " << Indent;
        O << "LocVT == " << getEnumName(getValueType(VT));
      }

    } else if (Action->isSubClassOf("CCIf")) {
      O << Action->getValueAsString("Predicate");
    } else {
      errs() << *Action;
      PrintFatalError(Action->getLoc(), "Unknown CCPredicateAction!");
    }

    O << ") {\n";
    emitAction(Action->getValueAsDef("SubAction"), Indent + 2, O);
    O << Indent << "}\n";
  } else {
    if (Action->isSubClassOf("CCDelegateTo")) {
      const Record *CC = Action->getValueAsDef("CC");
      O << Indent << "if (!" << CC->getName()
        << "(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))\n"
        << Indent + 2 << "return false;\n";
      DelegateToMap[CurrentAction].insert(CC->getName().str());
    } else if (Action->isSubClassOf("CCAssignToReg") ||
               Action->isSubClassOf("CCAssignToRegAndStack")) {
      const ListInit *RegList = Action->getValueAsListInit("RegList");
      if (RegList->size() == 1) {
        std::string Name = getQualifiedName(RegList->getElementAsRecord(0));
        O << Indent << "if (MCRegister Reg = State.AllocateReg(" << Name
          << ")) {\n";
        if (SwiftAction)
          AssignedSwiftRegsMap[CurrentAction].insert(std::move(Name));
        else
          AssignedRegsMap[CurrentAction].insert(std::move(Name));
      } else {
        O << Indent << "static const MCPhysReg RegList" << ++Counter
          << "[] = {\n";
        O << Indent << "  ";
        ListSeparator LS;
        for (unsigned I = 0, E = RegList->size(); I != E; ++I) {
          std::string Name = getQualifiedName(RegList->getElementAsRecord(I));
          if (SwiftAction)
            AssignedSwiftRegsMap[CurrentAction].insert(Name);
          else
            AssignedRegsMap[CurrentAction].insert(Name);
          O << LS << Name;
        }
        O << "\n" << Indent << "};\n";
        O << Indent << "if (MCRegister Reg = State.AllocateReg(RegList"
          << Counter << ")) {\n";
      }
      O << Indent << "  State.addLoc(CCValAssign::getReg(ValNo, ValVT, "
        << "Reg, LocVT, LocInfo));\n";
      if (Action->isSubClassOf("CCAssignToRegAndStack")) {
        int Size = Action->getValueAsInt("Size");
        int Align = Action->getValueAsInt("Align");
        O << Indent << "  (void)State.AllocateStack(";
        if (Size)
          O << Size << ", ";
        else
          O << "\n"
            << Indent
            << "  State.getMachineFunction().getDataLayout()."
               "getTypeAllocSize(EVT(LocVT).getTypeForEVT(State.getContext())),"
               " ";
        if (Align)
          O << "Align(" << Align << ")";
        else
          O << "\n"
            << Indent
            << "  State.getMachineFunction().getDataLayout()."
               "getABITypeAlign(EVT(LocVT).getTypeForEVT(State.getContext()"
               "))";
        O << ");\n";
      }
      O << Indent << "  return false;\n";
      O << Indent << "}\n";
    } else if (Action->isSubClassOf("CCAssignToRegWithShadow")) {
      const ListInit *RegList = Action->getValueAsListInit("RegList");
      const ListInit *ShadowRegList =
          Action->getValueAsListInit("ShadowRegList");
      if (!ShadowRegList->empty() && ShadowRegList->size() != RegList->size())
        PrintFatalError(Action->getLoc(),
                        "Invalid length of list of shadowed registers");

      if (RegList->size() == 1) {
        O << Indent << "if (MCRegister Reg = State.AllocateReg(";
        O << getQualifiedName(RegList->getElementAsRecord(0));
        O << ", " << getQualifiedName(ShadowRegList->getElementAsRecord(0));
        O << ")) {\n";
      } else {
        unsigned RegListNumber = ++Counter;
        unsigned ShadowRegListNumber = ++Counter;

        O << Indent << "static const MCPhysReg RegList" << RegListNumber
          << "[] = {\n";
        O << Indent << "  ";
        ListSeparator LS;
        for (unsigned I = 0, E = RegList->size(); I != E; ++I)
          O << LS << getQualifiedName(RegList->getElementAsRecord(I));
        O << "\n" << Indent << "};\n";

        O << Indent << "static const MCPhysReg RegList" << ShadowRegListNumber
          << "[] = {\n";
        O << Indent << "  ";
        ListSeparator LSS;
        for (unsigned I = 0, E = ShadowRegList->size(); I != E; ++I)
          O << LSS << getQualifiedName(ShadowRegList->getElementAsRecord(I));
        O << "\n" << Indent << "};\n";

        O << Indent << "if (MCRegister Reg = State.AllocateReg(RegList"
          << RegListNumber << ", "
          << "RegList" << ShadowRegListNumber << ")) {\n";
      }
      O << Indent << "  State.addLoc(CCValAssign::getReg(ValNo, ValVT, "
        << "Reg, LocVT, LocInfo));\n";
      O << Indent << "  return false;\n";
      O << Indent << "}\n";
    } else if (Action->isSubClassOf("CCAssignToStack")) {
      int Size = Action->getValueAsInt("Size");
      int Align = Action->getValueAsInt("Align");

      O << Indent << "int64_t Offset" << ++Counter << " = State.AllocateStack(";
      if (Size)
        O << Size << ", ";
      else
        O << "\n"
          << Indent
          << "  State.getMachineFunction().getDataLayout()."
             "getTypeAllocSize(EVT(LocVT).getTypeForEVT(State.getContext())),"
             " ";
      if (Align)
        O << "Align(" << Align << ")";
      else
        O << "\n"
          << Indent
          << "  State.getMachineFunction().getDataLayout()."
             "getABITypeAlign(EVT(LocVT).getTypeForEVT(State.getContext()"
             "))";
      O << ");\n"
        << Indent << "State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset"
        << Counter << ", LocVT, LocInfo));\n";
      O << Indent << "return false;\n";
    } else if (Action->isSubClassOf("CCAssignToStackWithShadow")) {
      int Size = Action->getValueAsInt("Size");
      int Align = Action->getValueAsInt("Align");
      const ListInit *ShadowRegList =
          Action->getValueAsListInit("ShadowRegList");

      unsigned ShadowRegListNumber = ++Counter;

      O << Indent << "static const MCPhysReg ShadowRegList"
        << ShadowRegListNumber << "[] = {\n";
      O << Indent << "  ";
      ListSeparator LS;
      for (unsigned I = 0, E = ShadowRegList->size(); I != E; ++I)
        O << LS << getQualifiedName(ShadowRegList->getElementAsRecord(I));
      O << "\n" << Indent << "};\n";

      O << Indent << "int64_t Offset" << ++Counter << " = State.AllocateStack("
        << Size << ", Align(" << Align << "), "
        << "ShadowRegList" << ShadowRegListNumber << ");\n";
      O << Indent << "State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset"
        << Counter << ", LocVT, LocInfo));\n";
      O << Indent << "return false;\n";
    } else if (Action->isSubClassOf("CCPromoteToType")) {
      const Record *DestTy = Action->getValueAsDef("DestTy");
      MVT::SimpleValueType DestVT = getValueType(DestTy);
      O << Indent << "LocVT = " << getEnumName(DestVT) << ";\n";
      if (MVT(DestVT).isFloatingPoint()) {
        O << Indent << "LocInfo = CCValAssign::FPExt;\n";
      } else {
        O << Indent << "if (ArgFlags.isSExt())\n"
          << Indent << "  LocInfo = CCValAssign::SExt;\n"
          << Indent << "else if (ArgFlags.isZExt())\n"
          << Indent << "  LocInfo = CCValAssign::ZExt;\n"
          << Indent << "else\n"
          << Indent << "  LocInfo = CCValAssign::AExt;\n";
      }
    } else if (Action->isSubClassOf("CCPromoteToUpperBitsInType")) {
      const Record *DestTy = Action->getValueAsDef("DestTy");
      MVT::SimpleValueType DestVT = getValueType(DestTy);
      O << Indent << "LocVT = " << getEnumName(DestVT) << ";\n";
      if (MVT(DestVT).isFloatingPoint()) {
        PrintFatalError(Action->getLoc(),
                        "CCPromoteToUpperBitsInType does not handle floating "
                        "point");
      } else {
        O << Indent << "if (ArgFlags.isSExt())\n"
          << Indent << "  LocInfo = CCValAssign::SExtUpper;\n"
          << Indent << "else if (ArgFlags.isZExt())\n"
          << Indent << "  LocInfo = CCValAssign::ZExtUpper;\n"
          << Indent << "else\n"
          << Indent << "  LocInfo = CCValAssign::AExtUpper;\n";
      }
    } else if (Action->isSubClassOf("CCBitConvertToType")) {
      const Record *DestTy = Action->getValueAsDef("DestTy");
      O << Indent << "LocVT = " << getEnumName(getValueType(DestTy)) << ";\n";
      O << Indent << "LocInfo = CCValAssign::BCvt;\n";
    } else if (Action->isSubClassOf("CCTruncToType")) {
      const Record *DestTy = Action->getValueAsDef("DestTy");
      O << Indent << "LocVT = " << getEnumName(getValueType(DestTy)) << ";\n";
      O << Indent << "LocInfo = CCValAssign::Trunc;\n";
    } else if (Action->isSubClassOf("CCPassIndirect")) {
      const Record *DestTy = Action->getValueAsDef("DestTy");
      O << Indent << "LocVT = " << getEnumName(getValueType(DestTy)) << ";\n";
      O << Indent << "LocInfo = CCValAssign::Indirect;\n";
    } else if (Action->isSubClassOf("CCPassByVal")) {
      int Size = Action->getValueAsInt("Size");
      int Align = Action->getValueAsInt("Align");
      O << Indent << "State.HandleByVal(ValNo, ValVT, LocVT, LocInfo, " << Size
        << ", Align(" << Align << "), ArgFlags);\n";
      O << Indent << "return false;\n";
    } else if (Action->isSubClassOf("CCCustom")) {
      O << Indent << "if (" << Action->getValueAsString("FuncName")
        << "(ValNo, ValVT, "
        << "LocVT, LocInfo, ArgFlags, State))\n";
      O << Indent << "  return false;\n";
    } else {
      errs() << *Action;
      PrintFatalError(Action->getLoc(), "Unknown CCAction!");
    }
  }
}

void CallingConvEmitter::emitArgRegisterLists(raw_ostream &O) {
  // Transitively merge all delegated CCs into AssignedRegsMap.
  using EntryTy = std::pair<std::string, std::set<std::string>>;
  bool Redo;
  do {
    Redo = false;
    std::deque<EntryTy> Worklist(DelegateToMap.begin(), DelegateToMap.end());

    while (!Worklist.empty()) {
      EntryTy Entry = Worklist.front();
      Worklist.pop_front();

      const std::string &CCName = Entry.first;
      std::set<std::string> &Registers = Entry.second;
      if (!Registers.empty())
        continue;

      for (auto &InnerEntry : Worklist) {
        const std::string &InnerCCName = InnerEntry.first;
        std::set<std::string> &InnerRegisters = InnerEntry.second;

        auto It = InnerRegisters.find(CCName);
        if (It != InnerRegisters.end()) {
          const auto &Src = AssignedRegsMap[CCName];
          AssignedRegsMap[InnerCCName].insert(Src.begin(), Src.end());
          InnerRegisters.erase(It);
        }
      }

      DelegateToMap.erase(CCName);
      Redo = true;
    }
  } while (Redo);

  if (AssignedRegsMap.empty())
    return;

  O << "\n#else\n\n";

  for (auto &Entry : AssignedRegsMap) {
    const std::string &RegName = Entry.first;
    std::set<std::string> &Registers = Entry.second;

    if (RegName.empty())
      continue;

    O << "const MCRegister " << Entry.first << "_ArgRegs[] = { ";

    if (Registers.empty()) {
      O << "0";
    } else {
      ListSeparator LS;
      for (const std::string &Reg : Registers)
        O << LS << Reg;
    }

    O << " };\n";
  }

  if (AssignedSwiftRegsMap.empty())
    return;

  O << "\n// Registers used by Swift.\n";
  for (auto &Entry : AssignedSwiftRegsMap) {
    const std::string &RegName = Entry.first;
    std::set<std::string> &Registers = Entry.second;

    O << "const MCRegister " << RegName << "_Swift_ArgRegs[] = { ";

    ListSeparator LS;
    for (const std::string &Reg : Registers)
      O << LS << Reg;

    O << " };\n";
  }
}

static TableGen::Emitter::OptClass<CallingConvEmitter>
    X("gen-callingconv", "Generate calling convention descriptions");
