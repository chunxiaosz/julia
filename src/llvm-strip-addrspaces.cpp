// This file is a part of Julia. License is MIT: https://julialang.org/license

#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "llvm-version.h"
#include "codegen_shared.h"
#include "julia.h"

#define DEBUG_TYPE "strip_julia_addrspaces"

using namespace llvm;

struct StripJuliaAddrspaces : public ModulePass {
    static char ID;
    StripJuliaAddrspaces() : ModulePass(ID) {};

public:
    bool runOnModule(Module &M) override;
};

class AddrspaceStripper: public ValueMapTypeRemapper
{
public:
    Type *remapType(Type *SrcTy) override
    {
        // TODO: handle all descendants of SequentialType/CompositeType
        // TODO: also handle FunctionType here?
        if (auto PtrT = dyn_cast<PointerType>(SrcTy))
            return remapType(PtrT);
        else
            return SrcTy;
    }

    PointerType* remapType(PointerType* SrcTy) {
        Type* ElT = remapType(SrcTy->getElementType());
        if (isSpecialAS(SrcTy->getAddressSpace()))
            return PointerType::get(ElT, AddressSpace::Generic);
        else
            return PointerType::get(ElT, SrcTy->getAddressSpace());        
    }

    FunctionType* remapType(FunctionType *SrcTy) {
        auto *NewRT = remapType(SrcTy->getReturnType());

        auto Params = SrcTy->getNumParams();
        SmallVector<Type*,4> NewParams(Params);
        for (unsigned i = 0; i < Params; ++i)
            NewParams[i] = remapType(SrcTy->getParamType(i));

        return FunctionType::get(NewRT, NewParams, SrcTy->isVarArg());
    }
};

bool StripJuliaAddrspaces::runOnModule(Module &M) {
    SmallVector<std::pair<Function*,Function*>,4> Replacements;

    for (auto &F: M) {
        AddrspaceStripper TypeMapper;
        auto *FTy = F.getFunctionType();
        auto *NewFTy = TypeMapper.remapType(FTy);

        if (FTy != NewFTy) {            
            ValueToValueMapTy VMap;

            // Create the new function...
            auto *NewF = Function::Create(NewFTy, F.getLinkage(), F.getName(), F.getParent());

            // Loop over the arguments, copying the names of the mapped arguments over...
            Function::arg_iterator DestI = NewF->arg_begin();
            for (Function::const_arg_iterator I = F.arg_begin(), E = F.arg_end(); I != E; ++I) {
                DestI->setName(I->getName());   // Copy the name over...
                VMap[&*I] = &*(DestI++);        // Add mapping to VMap
            }

            SmallVector<ReturnInst*, 8> Returns;  // Ignore returns cloned.
            CloneFunctionInto(NewF, &F, VMap, /*ModuleLevelChanges=*/false, Returns,
                              "", NULL, &TypeMapper);

            // TODO: rewrite IR

            Replacements.push_back({&F, NewF});
        }
    }

    for (auto &Fs: Replacements) {
        Function *F = Fs.first;
        Function *NewF = Fs.second;

        std::string NewFName = F->getName();
        F->setName("");
        NewF->setName(NewFName);
        F->replaceAllUsesWith(NewF);
        F->eraseFromParent();
    }

    return Replacements.size() > 0;
}

char StripJuliaAddrspaces::ID = 0;
static RegisterPass<StripJuliaAddrspaces> X("StripJuliaAddrspaces", "Strip (non-)rootedness information", false, false);

Pass *createStripJuliaAddrspaces() {
    return new StripJuliaAddrspaces();
}
