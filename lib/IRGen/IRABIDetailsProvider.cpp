//===--- IRABIDetailsProvider.cpp - Get ABI details for decls ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IRGen/IRABIDetailsProvider.h"
#include "Callee.h"
#include "FixedTypeInfo.h"
#include "GenEnum.h"
#include "GenType.h"
#include "GenericRequirement.h"
#include "IRGen.h"
#include "IRGenModule.h"
#include "NativeConventionSchema.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILFunctionBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/Subsystems.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/CodeGen/SwiftCallingConv.h"
#include "llvm/IR/DerivedTypes.h"

using namespace swift;
using namespace irgen;

static Optional<Type> getPrimitiveTypeFromLLVMType(ASTContext &ctx,
                                                   const llvm::Type *type) {
  if (const auto *intType = dyn_cast<llvm::IntegerType>(type)) {
    switch (intType->getBitWidth()) {
    case 1:
      return ctx.getBoolType();
    case 8:
      return ctx.getUInt8Type();
    case 16:
      return ctx.getUInt16Type();
    case 32:
      return ctx.getUInt32Type();
    case 64:
      return ctx.getUInt64Type();
    default:
      return None;
    }
  } else if (type->isFloatTy()) {
    return ctx.getFloatType();
  } else if (type->isDoubleTy()) {
    return ctx.getDoubleType();
  } else if (type->isPointerTy()) {
    return ctx.getOpaquePointerType();
  }
  // FIXME: Handle vector type.
  return None;
}

namespace swift {

class IRABIDetailsProviderImpl {
public:
  IRABIDetailsProviderImpl(ModuleDecl &mod, const IRGenOptions &opts)
      : typeConverter(mod),
        silMod(SILModule::createEmptyModule(&mod, typeConverter, silOpts)),
        IRGen(opts, *silMod), IGM(IRGen, IRGen.createTargetMachine()) {}

  llvm::Optional<IRABIDetailsProvider::SizeAndAlignment>
  getTypeSizeAlignment(const NominalTypeDecl *TD) {
    auto *TI = &IGM.getTypeInfoForUnlowered(TD->getDeclaredTypeInContext());
    auto *fixedTI = dyn_cast<FixedTypeInfo>(TI);
    if (!fixedTI)
      return None;
    return IRABIDetailsProvider::SizeAndAlignment{
        fixedTI->getFixedSize().getValue(),
        fixedTI->getFixedAlignment().getValue()};
  }

  bool shouldPassIndirectly(Type type) {
    auto *TI = &IGM.getTypeInfoForUnlowered(type);
    NativeConventionSchema schema(IGM, TI, /*isResult=*/false);
    return schema.requiresIndirect();
  }

  bool shouldReturnIndirectly(Type type) {
    if (type->isVoid())
      return false;
    auto *TI = &IGM.getTypeInfoForUnlowered(type);
    NativeConventionSchema schema(IGM, TI, /*isResult=*/true);
    return schema.requiresIndirect();
  }

  bool enumerateDirectPassingRecordMembers(
      Type t, llvm::function_ref<void(clang::CharUnits, clang::CharUnits, Type)>
                  callback) {
    auto *TI = &IGM.getTypeInfoForUnlowered(t);
    NativeConventionSchema schema(IGM, TI, /*isResult=*/false);
    bool hasError = false;
    schema.enumerateComponents(
        [&](clang::CharUnits offset, clang::CharUnits end, llvm::Type *type) {
          auto primitiveType = getPrimitiveTypeFromLLVMType(
              IGM.getSwiftModule()->getASTContext(), type);
          if (!primitiveType) {
            hasError = true;
            return;
          }
          callback(offset, end, *primitiveType);
        });
    return hasError;
  }

  IRABIDetailsProvider::FunctionABISignature
  getTypeMetadataAccessFunctionSignature() {
    auto &ctx = IGM.getSwiftModule()->getASTContext();
    llvm::StructType *responseTy = IGM.getTypeMetadataResponseTy();
    IRABIDetailsProvider::TypeRecordABIRepresentation::MemberVectorTy members;
    for (auto *elementTy : responseTy->elements())
      members.push_back(*getPrimitiveTypeFromLLVMType(ctx, elementTy));
    auto returnTy =
        IRABIDetailsProvider::TypeRecordABIRepresentation(std::move(members));
    auto paramTy = IRABIDetailsProvider::TypeRecordABIRepresentation(
        {*getPrimitiveTypeFromLLVMType(ctx,
                                       IGM.getTypeMetadataRequestParamTy())});
    return {returnTy, {paramTy}};
  }

  SmallVector<GenericRequirement, 2>
  getTypeMetadataAccessFunctionGenericRequirementParameters(
      NominalTypeDecl *nominal) {
    GenericTypeRequirements requirements(IGM, nominal);
    SmallVector<GenericRequirement, 2> result;
    for (const auto &req : requirements.getRequirements())
      result.push_back(req);
    return result;
  }

  llvm::MapVector<EnumElementDecl *, IRABIDetailsProvider::EnumElementInfo>
  getEnumTagMapping(const EnumDecl *ED) {
    llvm::MapVector<EnumElementDecl *, IRABIDetailsProvider::EnumElementInfo>
        elements;
    auto &enumImplStrat =
        getEnumImplStrategy(IGM, ED->getDeclaredType()->getCanonicalType());

    for (auto *element : ED->getAllElements()) {
      auto tagIdx = enumImplStrat.getTagIndex(element);
      auto *global = cast<llvm::GlobalVariable>(
          IGM.getAddrOfEnumCase(element, NotForDefinition).getAddress());
      elements.insert({element, {tagIdx, global->getName()}});
    }

    return elements;
  }

  llvm::Optional<IRABIDetailsProvider::LoweredFunctionSignature>
  getFunctionLoweredSignature(AbstractFunctionDecl *fd) {
    auto function = SILFunction::getFunction(SILDeclRef(fd), *silMod);
    auto silFuncType = function->getLoweredFunctionType();
    // FIXME: Async function support.
    if (silFuncType->isAsync())
      return None;
    if (silFuncType->getLanguage() != SILFunctionLanguage::Swift)
      return None;

    auto funcPointerKind =
        FunctionPointerKind(FunctionPointerKind::BasicKind::Function);

    auto *abiDetails = new (signatureExpansions.Allocate())
        SignatureExpansionABIDetails(Signature::getUncachedABIDetails(
            IGM, silFuncType, funcPointerKind));

    auto result =
        IRABIDetailsProvider::LoweredFunctionSignature(fd, *this, *abiDetails);
    // Save metadata source types to avoid keeping the SIL func around.
    for (const auto &typeSource :
         abiDetails->polymorphicSignatureExpandedTypeSources) {
      typeSource.visit(
          [&](const GenericRequirement &reqt) {},
          [&](const MetadataSource &metadataSource) {
            auto index = metadataSource.getParamIndex();
            auto canType =
                silFuncType->getParameters()[index].getInterfaceType();
            result.metadataSourceTypes.push_back(canType);
          });
    }
    return result;
  }

  llvm::SmallVector<IRABIDetailsProvider::ABIAdditionalParam, 1>
  getFunctionABIAdditionalParams(AbstractFunctionDecl *afd) {
    llvm::SmallVector<IRABIDetailsProvider::ABIAdditionalParam, 1> params;

    auto function = SILFunction::getFunction(SILDeclRef(afd), *silMod);

    auto silFuncType = function->getLoweredFunctionType();
    auto funcPointerKind =
        FunctionPointerKind(FunctionPointerKind::BasicKind::Function);

    auto abiDetails =
        Signature::getUncachedABIDetails(IGM, silFuncType, funcPointerKind);

    using ABIAdditionalParam = IRABIDetailsProvider::ABIAdditionalParam;
    using ParamRole = ABIAdditionalParam::ABIParameterRole;
    // FIXME: remove second signature computation.
    auto signature = Signature::getUncached(IGM, silFuncType, funcPointerKind);
    for (auto attrSet : signature.getAttributes()) {
      if (attrSet.hasAttribute(llvm::Attribute::AttrKind::SwiftSelf))
        params.push_back(
            ABIAdditionalParam(ParamRole::Self, llvm::None, CanType()));
      if (attrSet.hasAttribute(llvm::Attribute::AttrKind::SwiftError))
        params.push_back(
            ABIAdditionalParam(ParamRole::Error, llvm::None, CanType()));
    }
    return params;
  }

  Lowering::TypeConverter typeConverter;
  // Default silOptions are sufficient, as we don't need to generated SIL.
  SILOptions silOpts;
  std::unique_ptr<SILModule> silMod;
  IRGenerator IRGen;
  IRGenModule IGM;
  llvm::SpecificBumpPtrAllocator<SignatureExpansionABIDetails>
      signatureExpansions;
};

} // namespace swift

IRABIDetailsProvider::LoweredFunctionSignature::LoweredFunctionSignature(
    const AbstractFunctionDecl *FD, IRABIDetailsProviderImpl &owner,
    const irgen::SignatureExpansionABIDetails &abiDetails)
    : FD(FD), owner(owner), abiDetails(abiDetails) {}

IRABIDetailsProvider::LoweredFunctionSignature::DirectResultType::
    DirectResultType(IRABIDetailsProviderImpl &owner,
                     const irgen::TypeInfo &typeDetails)
    : owner(owner), typeDetails(typeDetails) {}

bool IRABIDetailsProvider::LoweredFunctionSignature::DirectResultType::
    enumerateRecordMembers(
        llvm::function_ref<void(clang::CharUnits, clang::CharUnits, Type)>
            callback) const {
  auto &schema = typeDetails.nativeReturnValueSchema(owner.IGM);
  assert(!schema.requiresIndirect());
  bool hasError = false;
  schema.enumerateComponents(
      [&](clang::CharUnits offset, clang::CharUnits end, llvm::Type *type) {
        auto primitiveType = getPrimitiveTypeFromLLVMType(
            owner.IGM.getSwiftModule()->getASTContext(), type);
        if (!primitiveType) {
          hasError = true;
          return;
        }
        callback(offset, end, *primitiveType);
      });
  return hasError;
}

IRABIDetailsProvider::LoweredFunctionSignature::DirectParameter::
    DirectParameter(IRABIDetailsProviderImpl &owner,
                    const irgen::TypeInfo &typeDetails,
                    const ParamDecl &paramDecl)
    : owner(owner), typeDetails(typeDetails), paramDecl(paramDecl) {}

IRABIDetailsProvider::LoweredFunctionSignature::IndirectParameter::
    IndirectParameter(const ParamDecl &paramDecl)
    : paramDecl(paramDecl) {}

bool IRABIDetailsProvider::LoweredFunctionSignature::DirectParameter::
    enumerateRecordMembers(
        llvm::function_ref<void(clang::CharUnits, clang::CharUnits, Type)>
            callback) const {
  auto &schema = typeDetails.nativeParameterValueSchema(owner.IGM);
  assert(!schema.requiresIndirect());
  bool hasError = false;
  schema.enumerateComponents(
      [&](clang::CharUnits offset, clang::CharUnits end, llvm::Type *type) {
        auto primitiveType = getPrimitiveTypeFromLLVMType(
            owner.IGM.getSwiftModule()->getASTContext(), type);
        if (!primitiveType) {
          hasError = true;
          return;
        }
        callback(offset, end, *primitiveType);
      });
  return hasError;
}

IRABIDetailsProvider::LoweredFunctionSignature::GenericRequirementParameter::
    GenericRequirementParameter(const GenericRequirement &requirement)
    : requirement(requirement) {}

IRABIDetailsProvider::LoweredFunctionSignature::MetadataSourceParameter::
    MetadataSourceParameter(const CanType &type)
    : type(type) {}

llvm::Optional<IRABIDetailsProvider::LoweredFunctionSignature::DirectResultType>
IRABIDetailsProvider::LoweredFunctionSignature::getDirectResultType() const {
  if (!abiDetails.directResult)
    return None;
  return DirectResultType(owner, abiDetails.directResult->typeInfo);
}

size_t
IRABIDetailsProvider::LoweredFunctionSignature::getNumIndirectResultValues()
    const {
  return abiDetails.indirectResults.size();
}

void IRABIDetailsProvider::LoweredFunctionSignature::visitParameterList(
    llvm::function_ref<void(const IndirectResultValue &)> indirectResultVisitor,
    llvm::function_ref<void(const DirectParameter &)> directParamVisitor,
    llvm::function_ref<void(const IndirectParameter &)> indirectParamVisitor,
    llvm::function_ref<void(const GenericRequirementParameter &)>
        genericRequirementVisitor,
    llvm::function_ref<void(const MetadataSourceParameter &)>
        metadataSourceVisitor) {
  // Indirect result values come before parameters.
  llvm::SmallVector<IndirectResultValue, 1> result;
  for (const auto &r : abiDetails.indirectResults)
    indirectResultVisitor(IndirectResultValue(r.hasSRet));

  // Traverse ABI parameters, mapping them back to the AST parameters.
  llvm::SmallVector<const ParamDecl *, 8> silParamMapping;
  for (auto param : *FD->getParameters()) {
    // FIXME: tuples map to more than one sil param (but they're not yet
    // representable by the consumer).
    silParamMapping.push_back(param);
  }
  size_t currentSilParam = 0;
  for (const auto &abiParam : abiDetails.parameters) {
    bool isIndirect = true;
    if (!isIndirectFormalParameter(abiParam.convention)) {
      const auto &schema =
          abiParam.typeInfo.get().nativeParameterValueSchema(owner.IGM);
      if (!schema.requiresIndirect()) {
        // Skip ABI parameters with empty native representation, as they're not
        // emitted in the LLVM IR signature.
        if (schema.empty())
          continue;
        isIndirect = false;
      }
    }

    const ParamDecl *paramDecl = abiParam.isSelf
                                     ? FD->getImplicitSelfDecl()
                                     : silParamMapping[currentSilParam];
    ++currentSilParam;
    if (!isIndirect) {
      DirectParameter param(owner, abiParam.typeInfo, *paramDecl);
      directParamVisitor(param);
    } else {
      IndirectParameter param(*paramDecl);
      indirectParamVisitor(param);
    }
  }
  // FIXME: Use one assert for indirect self too.
  if (FD->getImplicitSelfDecl())
    assert(currentSilParam == (silParamMapping.size() + 1) ||
           currentSilParam == silParamMapping.size());
  else
    assert(currentSilParam == silParamMapping.size());

  // Generic requirements come next.
  size_t metadataSourceIndex = 0;
  for (const auto &typeSource :
       abiDetails.polymorphicSignatureExpandedTypeSources) {
    typeSource.visit(
        [&](const GenericRequirement &reqt) {
          genericRequirementVisitor(GenericRequirementParameter(reqt));
        },
        [&](const MetadataSource &metadataSource) {
          metadataSourceVisitor(MetadataSourceParameter(
              metadataSourceTypes[metadataSourceIndex]));
          ++metadataSourceIndex;
        });
  }

  // FIXME: Traverse other additional params.
}

IRABIDetailsProvider::IRABIDetailsProvider(ModuleDecl &mod,
                                           const IRGenOptions &opts)
    : impl(std::make_unique<IRABIDetailsProviderImpl>(mod, opts)) {}

IRABIDetailsProvider::~IRABIDetailsProvider() {}

llvm::Optional<IRABIDetailsProvider::SizeAndAlignment>
IRABIDetailsProvider::getTypeSizeAlignment(const NominalTypeDecl *TD) {
  return impl->getTypeSizeAlignment(TD);
}

llvm::Optional<IRABIDetailsProvider::LoweredFunctionSignature>
IRABIDetailsProvider::getFunctionLoweredSignature(AbstractFunctionDecl *fd) {
  return impl->getFunctionLoweredSignature(fd);
}

llvm::SmallVector<IRABIDetailsProvider::ABIAdditionalParam, 1>
IRABIDetailsProvider::getFunctionABIAdditionalParams(
    AbstractFunctionDecl *afd) {
  return impl->getFunctionABIAdditionalParams(afd);
}

bool IRABIDetailsProvider::shouldPassIndirectly(Type t) {
  return impl->shouldPassIndirectly(t);
}

bool IRABIDetailsProvider::shouldReturnIndirectly(Type t) {
  return impl->shouldReturnIndirectly(t);
}

bool IRABIDetailsProvider::enumerateDirectPassingRecordMembers(
    Type t, llvm::function_ref<void(clang::CharUnits, clang::CharUnits, Type)>
                callback) {
  return impl->enumerateDirectPassingRecordMembers(t, callback);
}

IRABIDetailsProvider::FunctionABISignature
IRABIDetailsProvider::getTypeMetadataAccessFunctionSignature() {
  return impl->getTypeMetadataAccessFunctionSignature();
}

SmallVector<GenericRequirement, 2>
IRABIDetailsProvider::getTypeMetadataAccessFunctionGenericRequirementParameters(
    NominalTypeDecl *nominal) {
  return impl->getTypeMetadataAccessFunctionGenericRequirementParameters(
      nominal);
}

llvm::MapVector<EnumElementDecl *, IRABIDetailsProvider::EnumElementInfo>
IRABIDetailsProvider::getEnumTagMapping(const EnumDecl *ED) {
  return impl->getEnumTagMapping(ED);
}
