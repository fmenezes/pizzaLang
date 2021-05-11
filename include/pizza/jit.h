#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/TargetProcessControl.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/LLVMContext.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

// copied from https://github.com/llvm/llvm-project/blob/release/12.x/llvm/examples/Kaleidoscope/include/KaleidoscopeJIT.h

namespace Pizza
{

    class JIT
    {
    private:
        std::unique_ptr<llvm::orc::TargetProcessControl> TPC;
        std::unique_ptr<llvm::orc::ExecutionSession> ES;

        llvm::DataLayout DL;
        llvm::orc::MangleAndInterner Mangle;

        llvm::orc::RTDyldObjectLinkingLayer ObjectLayer;
        llvm::orc::IRCompileLayer CompileLayer;

        llvm::orc::JITDylib &MainJD;

    public:
        JIT(std::unique_ptr<llvm::orc::TargetProcessControl> TPC,
            std::unique_ptr<llvm::orc::ExecutionSession> ES,
            llvm::orc::JITTargetMachineBuilder JTMB, llvm::DataLayout DL)
            : TPC(std::move(TPC)), ES(std::move(ES)), DL(std::move(DL)),
              Mangle(*this->ES, this->DL),
              ObjectLayer(*this->ES,
                          []()
                          { return std::make_unique<llvm::SectionMemoryManager>(); }),
              CompileLayer(*this->ES, ObjectLayer,
                           std::make_unique<llvm::orc::ConcurrentIRCompiler>(std::move(JTMB))),
              MainJD(this->ES->createBareJITDylib("<main>"))
        {
            MainJD.addGenerator(
                cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                    DL.getGlobalPrefix())));
        }

        ~JIT()
        {
            if (auto Err = ES->endSession())
                ES->reportError(std::move(Err));
        }

        static llvm::Expected<std::unique_ptr<JIT>> Create()
        {
            auto SSP = std::make_shared<llvm::orc::SymbolStringPool>();
            auto TPC = llvm::orc::SelfTargetProcessControl::Create(SSP);
            if (!TPC)
                return TPC.takeError();

            auto ES = std::make_unique<llvm::orc::ExecutionSession>(std::move(SSP));

            llvm::orc::JITTargetMachineBuilder JTMB((*TPC)->getTargetTriple());

            auto DL = JTMB.getDefaultDataLayoutForTarget();
            if (!DL)
                return DL.takeError();

            return std::make_unique<JIT>(std::move(*TPC), std::move(ES),
                                                     std::move(JTMB), std::move(*DL));
        }

        const llvm::DataLayout &getDataLayout() const { return DL; }

        llvm::orc::JITDylib &getMainJITDylib() { return MainJD; }

        llvm::Error addModule(llvm::orc::ThreadSafeModule TSM, llvm::orc::ResourceTrackerSP RT = nullptr)
        {
            if (!RT)
                RT = MainJD.getDefaultResourceTracker();
            return CompileLayer.add(RT, std::move(TSM));
        }

        llvm::Expected<llvm::JITEvaluatedSymbol> lookup(llvm::StringRef Name)
        {
            return ES->lookup({&MainJD}, Mangle(Name.str()));
        }
    };

}