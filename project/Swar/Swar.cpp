/* Problems I have now:
** 1) We need to use BasicBlockPass instead of FunctionPass (difference?)
**
** 5) Constants are all changed to i128? (Or changed to i32??)
** 6) Mask 149796 -> -112348??????? two-complement something.
**
** ./bin/opt -load lib/LLVMSwar.dylib -Swar /Users/sally/code/test/add2.ll
** updaet2
** 1) test performance
** 2) bitcast
** 3) negative numbers
**
*/

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include <stdlib.h>

using namespace llvm;

#define HIGHMASK false
#define LOWMASK true

namespace {
    struct Swar : public FunctionPass {
        static char ID;
        Swar() : FunctionPass(ID) {}

        Constant * generateMask(VectorType * vectorType, bool isLow);
        Value * swar_add(Value * a1, Value * a2, VectorType * vectorType, Instruction * addInst);
        Value * bitcast(Value * vector, BinaryOperator * addInst);

        bool runOnFunction(Function &function) override {
            errs() << function.getName() << ":\n";

            bool modified = false;

            for (auto &block : function) {
                std::vector<BinaryOperator *> addInstsToErase;

                for (auto &inst : block) {

                    if (!inst.isBinaryOp() || inst.getOpcode() != Instruction::Add) {
                        continue;
                    }
                    LLVMContext & context = llvm::getGlobalContext();
                    BinaryOperator * addInst = dyn_cast<BinaryOperator>(&inst);
                    errs() << "  found binary instruction\n";

                    Value * vector1 = inst.getOperand(0);
                    Value * vector2 = inst.getOperand(1);

                    auto vectorType = dyn_cast<VectorType>(vector1->getType());
                    auto elemSize = vectorType->getScalarSizeInBits();
                    auto numElems = vectorType->getNumElements();

                    errs() << "    from vector <" << numElems << " x i" << elemSize << ">\n";

                    auto vectorSize = elemSize * numElems;
                    Type * castTy;
                    if (vectorSize > 256) { // larger than 256
                        errs() << "Length larger than 256\n";
                        continue;
                    } else {
                        castTy = llvm::IntegerType::get(context, vectorSize);
                    }
                    modified = true;

                    Value * a1 = bitcast(vector1, addInst);
                    Value * a2 = bitcast(vector2, addInst);

                    Value * result = swar_add(a1, a2, vectorType, addInst);

                    addInstsToErase.push_back(addInst);
                    addInst->replaceAllUsesWith(result);
                }

                for (auto &instr : addInstsToErase) {
                    instr->eraseFromParent();
                }
            }

            return modified;
        }
    };
}

char Swar::ID = 0;

static RegisterPass<Swar> X("Swar", "SWAR", false, false);

/**
 * Swar addition instructions
 */
Value * Swar::swar_add(Value * a1, Value * a2, VectorType * vectorType, Instruction * addInst) {
    Constant * lmask = generateMask(vectorType, LOWMASK);
    Constant * hmask = generateMask(vectorType, HIGHMASK);

    BinaryOperator * m1 = BinaryOperator::Create(Instruction::And, (Value *)a1, (Value *)lmask, "", addInst);
    BinaryOperator * m2 = BinaryOperator::Create(Instruction::And, (Value *)a2, (Value *)lmask, "", addInst);
    BinaryOperator * r1 = BinaryOperator::Create(Instruction::Add, (Value *)m1, (Value *)m2, "", addInst);
    BinaryOperator * r2 = BinaryOperator::Create(Instruction::Xor, (Value *)a1, (Value *)a2, "", addInst);
    BinaryOperator * r3 = BinaryOperator::Create(Instruction::And, (Value *)r2, (Value *)hmask, "", addInst);
    BinaryOperator * r4 = BinaryOperator::Create(Instruction::Xor, (Value *)r1, (Value *)r3, "", addInst);

    LLVMContext & context = llvm::getGlobalContext();
    auto elementType = Type::getInt8Ty(context);
    auto legalType = VectorType::get(elementType, 16);

    ZExtInst * r5 = new ZExtInst((Value *)r4, Type::getInt128Ty(context), "", addInst);
    BitCastInst * result = new llvm::BitCastInst(r5, legalType, "", addInst);

    return (Value *)result;
}

/**
 * Bitcast Legalization
 * Ex: <6 * i3> <i3 3, i3 4, i3 2, i3 1, i3 0, i3 1>
 * To: <i18> Octal: 101243 Decimal: 33443
 */
Value * Swar::bitcast(Value * vector, BinaryOperator * addInst) {
    auto vectorType = cast<VectorType>(vector->getType());
    auto elemSize = vectorType->getScalarSizeInBits();
    auto numElems = vectorType->getNumElements();
    auto totalSize = elemSize * numElems;

    unsigned int legalizedTotalSize = 8;
    while (legalizedTotalSize < totalSize) {
        legalizedTotalSize *= 2;
    }

    IRBuilder<> builder(addInst);

    Value * lastValue;
    for (unsigned int i = 0; i < numElems; ++i) {
        Value * elemValue = builder.CreateExtractElement(vector, i);
        Value * zextElemValue = builder.CreateZExt(elemValue, IntegerType::get(builder.getContext(), legalizedTotalSize));

        if (i == 0) {
            lastValue = zextElemValue;
        }
        else {
            Value * shiftedValue = builder.CreateShl(zextElemValue, i * elemSize);
            lastValue = builder.CreateOr(lastValue, shiftedValue);
        }
    }

    Value * result = builder.CreateTrunc(lastValue, IntegerType::get(builder.getContext(), legalizedTotalSize));
    return result;
}

// Need to OPTIMIZE!!
Constant * Swar::generateMask(VectorType * vectorType, bool isLow = false) {
    auto elemSize = vectorType->getScalarSizeInBits();
    auto numElems = vectorType->getNumElements();
    unsigned int legalizedTotalSize = 8;
    while (legalizedTotalSize < elemSize * numElems) {
        legalizedTotalSize *= 2;
    }
    Type * castTy = llvm::IntegerType::get(llvm::getGlobalContext(), legalizedTotalSize);

    char * ptr;
    char string[128];
    char unit[128];

    unsigned int j;
    for (j = 0; j < elemSize; j ++) {
        if (j == 0) {
            unit[j] = (isLow) ? '0' : '1';
            continue;
        }
        unit[j] = (isLow) ? '1' : '0';
    }
    unit[j] = '\0';

    for (j = 0; j < numElems; j ++) {
        for (unsigned int i = 0; i < elemSize; i ++) {
            string[j * elemSize + i] = unit[i];
        }
    }
    string[j * elemSize] = '\0';

    unsigned long int parsed = strtol(string, &ptr, 2);
    Constant * mask = llvm::ConstantInt::get(castTy, parsed);

    return mask;
}