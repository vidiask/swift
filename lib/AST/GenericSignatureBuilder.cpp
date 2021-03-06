//===--- GenericSignatureBuilder.cpp - Generic Requirement Builder --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Support for collecting a set of generic requirements, both explicitly stated
// and inferred, and computing the archetypes and required witness tables from
// those requirements.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Basic/Defer.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace swift;
using llvm::DenseMap;

namespace {
  typedef GenericSignatureBuilder::RequirementSource RequirementSource;
  typedef GenericSignatureBuilder::FloatingRequirementSource
    FloatingRequirementSource;
  typedef GenericSignatureBuilder::ConstraintResult ConstraintResult;
  typedef GenericSignatureBuilder::PotentialArchetype PotentialArchetype;
  typedef GenericSignatureBuilder::ConcreteConstraint ConcreteConstraint;
  template<typename T> using Constraint =
    GenericSignatureBuilder::Constraint<T>;
  typedef GenericSignatureBuilder::EquivalenceClass EquivalenceClass;
  typedef EquivalenceClass::DerivedSameTypeComponent DerivedSameTypeComponent;

} // end anonymous namespace

struct GenericSignatureBuilder::Implementation {
  /// Function used to look up conformances.
  std::function<GenericFunction> LookupConformance;

  /// The generic parameters that this generic signature builder is working
  /// with.
  SmallVector<GenericTypeParamType *, 4> GenericParams;

  /// The potential archetypes for the generic parameters in \c GenericParams.
  SmallVector<PotentialArchetype *, 4> PotentialArchetypes;

  /// The number of nested types that haven't yet been resolved to archetypes.
  /// Once all requirements have been added, this will be zero in well-formed
  /// code.
  unsigned NumUnresolvedNestedTypes = 0;

  /// The nested types that have been renamed.
  SmallVector<PotentialArchetype *, 4> RenamedNestedTypes;

  /// The requirement sources used in this generic signature builder.
  llvm::FoldingSet<RequirementSource> RequirementSources;

  /// The set of requirements that have been delayed for some reason.
  SmallVector<DelayedRequirement, 4> DelayedRequirements;

#ifndef NDEBUG
  /// Whether we've already finalized the builder.
  bool finalized = false;
#endif
};

#pragma mark Requirement sources

#ifndef NDEBUG
bool RequirementSource::isAcceptableStorageKind(Kind kind,
                                                StorageKind storageKind) {
  switch (kind) {
  case Explicit:
  case Inferred:
  case RequirementSignatureSelf:
  case NestedTypeNameMatch:
    switch (storageKind) {
    case StorageKind::RootArchetype:
      return true;

    case StorageKind::StoredType:
    case StorageKind::ProtocolConformance:
    case StorageKind::AssociatedTypeDecl:
      return false;
    }

  case Parent:
    switch (storageKind) {
    case StorageKind::AssociatedTypeDecl:
      return true;

    case StorageKind::RootArchetype:
    case StorageKind::StoredType:
    case StorageKind::ProtocolConformance:
      return false;
    }

  case ProtocolRequirement:
  case InferredProtocolRequirement:
    switch (storageKind) {
    case StorageKind::StoredType:
      return true;

    case StorageKind::RootArchetype:
    case StorageKind::ProtocolConformance:
    case StorageKind::AssociatedTypeDecl:
      return false;
    }

  case Superclass:
  case Concrete:
    switch (storageKind) {
    case StorageKind::ProtocolConformance:
      return true;

    case StorageKind::RootArchetype:
    case StorageKind::StoredType:
    case StorageKind::AssociatedTypeDecl:
      return false;
    }
  }

  llvm_unreachable("Unhandled RequirementSourceKind in switch.");
}
#endif

const void *RequirementSource::getOpaqueStorage1() const {
  switch (storageKind) {
  case StorageKind::RootArchetype:
    return storage.rootArchetype;

  case StorageKind::ProtocolConformance:
    return storage.conformance;

  case StorageKind::StoredType:
    return storage.type;

  case StorageKind::AssociatedTypeDecl:
    return storage.assocType;
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

const void *RequirementSource::getOpaqueStorage2() const {
  if (numTrailingObjects(OverloadToken<ProtocolDecl *>()) == 1)
    return getTrailingObjects<ProtocolDecl *>()[0];
  if (numTrailingObjects(OverloadToken<WrittenRequirementLoc>()) == 1)
    return getTrailingObjects<WrittenRequirementLoc>()[0].getOpaqueValue();

  return nullptr;
}

const void *RequirementSource::getOpaqueStorage3() const {
  if (numTrailingObjects(OverloadToken<ProtocolDecl *>()) == 1 &&
      numTrailingObjects(OverloadToken<WrittenRequirementLoc>()) == 1)
    return getTrailingObjects<WrittenRequirementLoc>()[0].getOpaqueValue();

  return nullptr;
}

bool RequirementSource::isInferredRequirement() const {
  for (auto source = this; source; source = source->parent) {
    switch (source->kind) {
    case Inferred:
    case InferredProtocolRequirement:
      return true;

    case Concrete:
    case Explicit:
    case NestedTypeNameMatch:
    case Parent:
    case ProtocolRequirement:
    case RequirementSignatureSelf:
    case Superclass:
      break;
    }
  }

  return false;
}

unsigned RequirementSource::classifyDiagKind() const {
  if (isInferredRequirement()) return 2;
  if (isDerivedRequirement()) return 1;
  return 0;
}

bool RequirementSource::isDerivedRequirement() const {
  switch (kind) {
  case Explicit:
  case Inferred:
    return false;

  case NestedTypeNameMatch:
  case Parent:
  case Superclass:
  case Concrete:
  case RequirementSignatureSelf:
    return true;

  case ProtocolRequirement:
  case InferredProtocolRequirement:
    // Requirements based on protocol requirements are derived unless they are
    // direct children of the requirement-signature source, in which case we
    // need to keep them for the requirement signature.
    return parent->kind != RequirementSignatureSelf;
  }

  llvm_unreachable("Unhandled RequirementSourceKind in switch.");
}

bool RequirementSource::isSelfDerivedSource(PotentialArchetype *pa,
                                            bool &derivedViaConcrete) const {
  derivedViaConcrete = false;

  // If it's not a derived requirement, it's not self-derived.
  if (!isDerivedRequirement()) return false;

  return visitPotentialArchetypesAlongPath(
           [&](PotentialArchetype *currentPA, const RequirementSource *source) {
    switch (source->kind) {
    case RequirementSource::Explicit:
    case RequirementSource::Inferred:
    case RequirementSource::RequirementSignatureSelf:
      for (auto parent = currentPA->getParent(); parent;
           parent = parent->getParent()) {
        if (parent->isInSameEquivalenceClassAs(pa))
          return true;
      }

      return false;

    case RequirementSource::Parent:
      return currentPA->isInSameEquivalenceClassAs(pa);

    case RequirementSource::ProtocolRequirement:
    case RequirementSource::InferredProtocolRequirement:
      // Note whether we saw derivation through a concrete type.
      if (currentPA->isConcreteType())
        derivedViaConcrete = true;
      return false;

    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::Concrete:
    case RequirementSource::Superclass:
      return false;
    }
  }) == nullptr;
}

/// Replace 'Self' in the given dependent type (\c depTy) with the given
/// potential archetype, producing a new potential archetype that refers to
/// the nested type. This limited operation makes sure that it does not
/// create any new potential archetypes along the way, so it should only be
/// used in cases where we're reconstructing something that we know exists.
static PotentialArchetype *replaceSelfWithPotentialArchetype(
                             PotentialArchetype *selfPA, Type depTy) {
  if (auto depMemTy = depTy->getAs<DependentMemberType>()) {
    // Recurse to produce the potential archetype for the base.
    auto basePA = replaceSelfWithPotentialArchetype(selfPA,
                                                    depMemTy->getBase());

    PotentialArchetype *nestedPAByName = nullptr;

    auto assocType = depMemTy->getAssocType();
    auto name = depMemTy->getName();
    auto findNested = [&](PotentialArchetype *pa) -> PotentialArchetype * {
      const auto &nested = pa->getNestedTypes();
      auto found = nested.find(name);

      if (found == nested.end()) return nullptr;
      if (found->second.empty()) return nullptr;

      // Note that we've found a nested PA by name.
      if (!nestedPAByName) {
        nestedPAByName = found->second.front();
      }

      // If we don't have an associated type to look for, we're done.
      if (!assocType) return nestedPAByName;

      // Look for a nested PA matching the associated type.
      for (auto nestedPA : found->second) {
        if (nestedPA->getResolvedAssociatedType() == assocType)
          return nestedPA;
      }

      return nullptr;
    };

    // First, look in the base potential archetype for the member we want.
    if (auto result = findNested(basePA))
      return result;

    // Otherwise, look elsewhere in the equivalence class of the base potential
    // archetype.
    for (auto otherBasePA : basePA->getEquivalenceClassMembers()) {
      if (otherBasePA == basePA) continue;

      if (auto result = findNested(otherBasePA))
        return result;
    }

    assert(nestedPAByName && "Didn't find the associated type we wanted");
    return nestedPAByName;
  }

  assert(depTy->is<GenericTypeParamType>() && "missing Self?");
  return selfPA;
}

bool RequirementSource::isSelfDerivedConformance(
                                             PotentialArchetype *currentPA,
                                             ProtocolDecl *proto,
                                             bool &derivedViaConcrete) const {
  /// Keep track of all of the requirements we've seen along the way. If
  /// we see the same requirement twice, it's a self-derived conformance.
  llvm::DenseSet<std::pair<PotentialArchetype *, ProtocolDecl *>>
    constraintsSeen;

  // Note that we've now seen a new constraint, returning true if we've seen
  // it before.
  auto addConstraint = [&](PotentialArchetype *pa, ProtocolDecl *proto) {
    return !constraintsSeen.insert({pa->getRepresentative(), proto}).second;
  };

  // Insert our end state.
  constraintsSeen.insert({currentPA->getRepresentative(), proto});

  derivedViaConcrete = false;
  bool sawProtocolRequirement = false;

  PotentialArchetype *rootPA = nullptr;
  auto resultPA = visitPotentialArchetypesAlongPath(
                    [&](PotentialArchetype *parentPA,
                        const RequirementSource *source) {
    switch (source->kind) {
    case ProtocolRequirement:
    case InferredProtocolRequirement: {
      // Note that we've seen a protocol requirement.
      sawProtocolRequirement = true;

      // If the base has been made concrete, note it.
      if (parentPA->isConcreteType())
        derivedViaConcrete = true;

      // The parent potential archetype must conform to the protocol in which
      // this requirement resides.
      return addConstraint(parentPA, source->getProtocolDecl());
    }

    case Concrete:
    case Superclass:
    case Parent:
      return false;
    case Explicit:
    case Inferred:
    case NestedTypeNameMatch:
    case RequirementSignatureSelf:
      rootPA = parentPA;
      return false;
    }
  });

  // If we saw a constraint twice, it's self-derived.
  if (!resultPA) return true;

  // If we haven't seen a protocol requirement, we're done.
  if (!sawProtocolRequirement) return false;

  // The root archetype might be a nested type, which implies constraints
  // for each of the protocols of the associated types referenced (if any).
  for (auto pa = rootPA; pa->getParent(); pa = pa->getParent()) {
    if (auto assocType = pa->getResolvedAssociatedType()) {
      if (addConstraint(pa->getParent(), assocType->getProtocol()))
        return true;
    }
  }

  return false;
}

#define REQUIREMENT_SOURCE_FACTORY_BODY(ProfileArgs, ConstructorArgs,      \
                                        NumProtocolDecls, WrittenReq)      \
  llvm::FoldingSetNodeID nodeID;                                           \
  Profile ProfileArgs;                                                     \
                                                                           \
  void *insertPos = nullptr;                                               \
  if (auto known =                                                         \
        builder.Impl->RequirementSources.FindNodeOrInsertPos(nodeID,       \
                                                             insertPos))   \
    return known;                                                          \
                                                                           \
  unsigned size =                                                          \
    totalSizeToAlloc<ProtocolDecl *, WrittenRequirementLoc>(               \
                                           NumProtocolDecls,               \
                                           WrittenReq.isNull()? 0 : 1);    \
  void *mem = ::operator new(size);                                        \
  auto result = new (mem) RequirementSource ConstructorArgs;               \
  builder.Impl->RequirementSources.InsertNode(result, insertPos);          \
  return result

const RequirementSource *RequirementSource::forAbstract(
                                                    PotentialArchetype *root) {
  auto &builder = *root->getBuilder();
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Explicit, nullptr, root, nullptr, nullptr),
                        (Explicit, root, nullptr, WrittenRequirementLoc()),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::forExplicit(
    PotentialArchetype *root,
    GenericSignatureBuilder::WrittenRequirementLoc writtenLoc) {
  auto &builder = *root->getBuilder();
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Explicit, nullptr, root,
                         writtenLoc.getOpaqueValue(), nullptr),
                        (Explicit, root, nullptr, writtenLoc),
                        0, writtenLoc);
}

const RequirementSource *RequirementSource::forInferred(
                                              PotentialArchetype *root,
                                              const TypeRepr *typeRepr) {
  WrittenRequirementLoc writtenLoc = typeRepr;
  auto &builder = *root->getBuilder();
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Inferred, nullptr, root,
                         writtenLoc.getOpaqueValue(), nullptr),
                        (Inferred, root, nullptr, writtenLoc),
                        0, writtenLoc);
}

const RequirementSource *RequirementSource::forRequirementSignature(
                                              PotentialArchetype *root,
                                              ProtocolDecl *protocol) {
  auto &builder = *root->getBuilder();
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, RequirementSignatureSelf, nullptr, root,
                         protocol, nullptr),
                        (RequirementSignatureSelf, root, protocol,
                         WrittenRequirementLoc()),
                        1, WrittenRequirementLoc());

}

const RequirementSource *RequirementSource::forNestedTypeNameMatch(
                                             PotentialArchetype *root) {
  auto &builder = *root->getBuilder();
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, NestedTypeNameMatch, nullptr, root,
                         nullptr, nullptr),
                        (NestedTypeNameMatch, root, nullptr,
                         WrittenRequirementLoc()),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::viaProtocolRequirement(
            GenericSignatureBuilder &builder, Type dependentType,
            ProtocolDecl *protocol,
            bool inferred,
            GenericSignatureBuilder::WrittenRequirementLoc writtenLoc) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID,
                         inferred ? InferredProtocolRequirement
                                  : ProtocolRequirement,
                         this,
                         dependentType.getPointer(), protocol,
                         writtenLoc.getOpaqueValue()),
                        (inferred ? InferredProtocolRequirement
                                  : ProtocolRequirement,
                         this, dependentType,
                         protocol, writtenLoc),
                        1, writtenLoc);
}

const RequirementSource *RequirementSource::viaSuperclass(
                                      GenericSignatureBuilder &builder,
                                      ProtocolConformance *conformance) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Superclass, this, conformance,
                         nullptr, nullptr),
                        (Superclass, this, conformance),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::viaConcrete(
                                      GenericSignatureBuilder &builder,
                                      ProtocolConformance *conformance) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Concrete, this, conformance, nullptr, nullptr),
                        (Concrete, this, conformance),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::viaParent(
                                      GenericSignatureBuilder &builder,
                                      AssociatedTypeDecl *assocType) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Parent, this, assocType, nullptr, nullptr),
                        (Parent, this, assocType),
                        0, WrittenRequirementLoc());
}

#undef REQUIREMENT_SOURCE_FACTORY_BODY

const RequirementSource *RequirementSource::getRoot() const {
  auto root = this;
  while (auto parent = root->parent)
    root = parent;
  return root;
}

PotentialArchetype *RequirementSource::getRootPotentialArchetype() const {
  /// Find the root.
  auto root = getRoot();

  // We're at the root, so it's in the inline storage.
  assert(root->storageKind == StorageKind::RootArchetype);
  return root->storage.rootArchetype;
}

PotentialArchetype *RequirementSource::getAffectedPotentialArchetype() const {
  return visitPotentialArchetypesAlongPath(
                         [](PotentialArchetype *, const RequirementSource *) {
                           return false;
                         });
}

PotentialArchetype *
RequirementSource::visitPotentialArchetypesAlongPath(
         llvm::function_ref<bool(PotentialArchetype *,
                                 const RequirementSource *)> visitor) const {
  switch (kind) {
  case RequirementSource::Parent: {
    auto parentPA = parent->visitPotentialArchetypesAlongPath(visitor);
    if (!parentPA) return nullptr;

    if (visitor(parentPA, this)) return nullptr;

    return replaceSelfWithPotentialArchetype(
                             parentPA,
                             getAssociatedType()->getDeclaredInterfaceType());
  }

  case RequirementSource::NestedTypeNameMatch:
  case RequirementSource::Explicit:
  case RequirementSource::Inferred:
  case RequirementSource::RequirementSignatureSelf: {
    auto rootPA = getRootPotentialArchetype();
    if (visitor(rootPA, this)) return nullptr;

    return rootPA;
  }

  case RequirementSource::Concrete:
  case RequirementSource::Superclass:
    return parent->visitPotentialArchetypesAlongPath(visitor);

  case RequirementSource::ProtocolRequirement:
  case RequirementSource::InferredProtocolRequirement: {
    auto parentPA = parent->visitPotentialArchetypesAlongPath(visitor);
    if (!parentPA) return nullptr;

    if (visitor(parentPA, this)) return nullptr;

    return replaceSelfWithPotentialArchetype(parentPA, getStoredType());
  }
  }
}

Type RequirementSource::getStoredType() const {
  switch (storageKind) {
  case StorageKind::RootArchetype:
  case StorageKind::ProtocolConformance:
  case StorageKind::AssociatedTypeDecl:
    return Type();

  case StorageKind::StoredType:
    return storage.type;
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

ProtocolDecl *RequirementSource::getProtocolDecl() const {
  switch (storageKind) {
  case StorageKind::RootArchetype:
    if (kind == RequirementSignatureSelf)
      return getTrailingObjects<ProtocolDecl *>()[0];
    return nullptr;

  case StorageKind::StoredType:
    if (isProtocolRequirement())
      return getTrailingObjects<ProtocolDecl *>()[0];
    return nullptr;

  case StorageKind::ProtocolConformance:
    if (storage.conformance)
      return storage.conformance->getProtocol();

    return nullptr;

  case StorageKind::AssociatedTypeDecl:
    return storage.assocType->getProtocol();
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

SourceLoc RequirementSource::getLoc() const {
  // Don't produce locations for protocol requirements unless the parent is
  // the protocol self.
  // FIXME: We should have a better notion of when to emit diagnostics
  // for a particular requirement, rather than turning on/off location info.
  // Locations that fall into this category should be advisory, emitted via
  // notes rather than as the normal location.
  if (isProtocolRequirement() && parent &&
      parent->kind != RequirementSignatureSelf)
    return parent->getLoc();

  if (auto typeRepr = getTypeRepr())
    return typeRepr->getStartLoc();

  if (auto requirementRepr = getRequirementRepr()) {
    switch (requirementRepr->getKind()) {
    case RequirementReprKind::LayoutConstraint:
    case RequirementReprKind::TypeConstraint:
      return requirementRepr->getColonLoc();

    case RequirementReprKind::SameType:
      return requirementRepr->getEqualLoc();
    }
  }
  if (parent)
    return parent->getLoc();

  if (kind == RequirementSignatureSelf)
    return getProtocolDecl()->getLoc();

  return SourceLoc();
}

/// Compute the path length of a requirement source, counting only the number
/// of \c ProtocolRequirement elements.
static unsigned sourcePathLength(const RequirementSource *source) {
  unsigned count = 0;
  for (; source; source = source->parent) {
    if (source->isProtocolRequirement())
      ++count;
  }
  return count;
}

int RequirementSource::compare(const RequirementSource *other) const {
  // Prefer the derived option, if there is one.
  bool thisIsDerived = this->isDerivedRequirement();
  bool otherIsDerived = other->isDerivedRequirement();
  if (thisIsDerived != otherIsDerived)
    return thisIsDerived ? -1 : +1;

  // Prefer the shorter path.
  unsigned thisLength = sourcePathLength(this);
  unsigned otherLength = sourcePathLength(other);
  if (thisLength != otherLength)
    return thisLength < otherLength ? -1 : +1;

  // FIXME: Arbitrary hack to allow later requirement sources to stomp on
  // earlier ones. We need a proper ordering here.
  return +1;
}

void RequirementSource::dump() const {
  dump(llvm::errs(), nullptr, 0);
  llvm::errs() << "\n";
}

/// Dump the constraint source.
void RequirementSource::dump(llvm::raw_ostream &out, SourceManager *srcMgr,
                             unsigned indent) const {
  // FIXME: Implement for real, so we actually dump the structure.
  out.indent(indent);
  print(out, srcMgr);
}

void RequirementSource::print() const {
  print(llvm::errs(), nullptr);
}

void RequirementSource::print(llvm::raw_ostream &out,
                              SourceManager *srcMgr) const {
  if (parent) {
    parent->print(out, srcMgr);
    out << " -> ";
  } else {
    auto pa = getRootPotentialArchetype();
    out << pa->getDebugName() << ": ";
  }

  switch (kind) {
  case Concrete:
    out << "Concrete";
    break;

  case Explicit:
    out << "Explicit";
    break;

  case Inferred:
    out << "Inferred";
    break;

  case NestedTypeNameMatch:
    out << "Nested type match";
    break;

  case Parent:
    out << "Parent";
    break;

  case ProtocolRequirement:
    out << "Protocol requirement";
    break;

  case InferredProtocolRequirement:
    out << "Inferred protocol requirement";
    break;

  case RequirementSignatureSelf:
    out << "Requirement signature self";
    break;

  case Superclass:
    out << "Superclass";
    break;
  }

  // Local function to dump a source location, if we can.
  auto dumpSourceLoc = [&](SourceLoc loc) {
    if (!srcMgr) return;
    if (loc.isInvalid()) return;

    unsigned bufferID = srcMgr->findBufferContainingLoc(loc);

    auto lineAndCol = srcMgr->getLineAndColumn(loc, bufferID);
    out << " @ " << lineAndCol.first << ':' << lineAndCol.second;
  };

  switch (storageKind) {
  case StorageKind::RootArchetype:
    break;

  case StorageKind::StoredType:
    if (auto proto = getProtocolDecl()) {
      out << " (via " << storage.type->getString() << " in " << proto->getName()
          << ")";
    }
    break;

  case StorageKind::ProtocolConformance:
    if (storage.conformance) {
      out << " (" << storage.conformance->getType()->getString() << ": "
          << storage.conformance->getProtocol()->getName() << ")";
    }
    break;

  case StorageKind::AssociatedTypeDecl:
    out << " (" << storage.assocType->getProtocol()->getName()
        << "::" << storage.assocType->getName() << ")";
    break;
  }

  if (getTypeRepr() || getRequirementRepr()) {
    dumpSourceLoc(getLoc());
  }
}

/// Form the dependent type such that the given protocol's \c Self can be
/// replaced by \c basePA to reach \c pa.
static Type formProtocolRelativeType(ProtocolDecl *proto,
                                     PotentialArchetype *basePA,
                                     PotentialArchetype *pa) {
  // Basis case: we've hit the base potential archetype.
  if (basePA == pa)
    return proto->getSelfInterfaceType();

  // Recursive case: form a dependent member type.
  auto baseType = formProtocolRelativeType(proto, basePA, pa->getParent());
  if (auto assocType = pa->getResolvedAssociatedType())
    return DependentMemberType::get(baseType, assocType);

  return DependentMemberType::get(baseType, pa->getNestedName());
}

const RequirementSource *FloatingRequirementSource::getSource(
                                                PotentialArchetype *pa) const {
  switch (kind) {
  case Resolved:
      return storage.get<const RequirementSource *>();

  case Explicit:
    if (auto requirementRepr = storage.dyn_cast<const RequirementRepr *>())
      return RequirementSource::forExplicit(pa, requirementRepr);
    if (auto typeRepr = storage.dyn_cast<const TypeRepr *>())
      return RequirementSource::forExplicit(pa, typeRepr);
    return RequirementSource::forAbstract(pa);

  case Inferred:
    return RequirementSource::forInferred(pa, storage.get<const TypeRepr *>());

  case AbstractProtocol: {
    // Derive the dependent type on which this requirement was written. It is
    // the path from the requirement source on which this requirement is based
    // to the potential archetype on which the requirement is being placed.
    auto baseSource = storage.get<const RequirementSource *>();
    auto baseSourcePA =
      baseSource->getAffectedPotentialArchetype();

    auto dependentType =
      formProtocolRelativeType(protocolReq.protocol, baseSourcePA, pa);

    return storage.get<const RequirementSource *>()
      ->viaProtocolRequirement(*pa->getBuilder(), dependentType,
                               protocolReq.protocol, protocolReq.inferred,
                               protocolReq.written);
  }

  case NestedTypeNameMatch:
    return RequirementSource::forNestedTypeNameMatch(pa);
  }

  llvm_unreachable("Unhandled FloatingPointRequirementSourceKind in switch.");
}

SourceLoc FloatingRequirementSource::getLoc() const {
  if (auto source = storage.dyn_cast<const RequirementSource *>())
    return source->getLoc();

  if (auto typeRepr = storage.dyn_cast<const TypeRepr *>())
    return typeRepr->getLoc();

  if (auto requirementRepr = storage.dyn_cast<const RequirementRepr *>()) {
    switch (requirementRepr->getKind()) {
    case RequirementReprKind::LayoutConstraint:
    case RequirementReprKind::TypeConstraint:
      return requirementRepr->getColonLoc();

    case RequirementReprKind::SameType:
      return requirementRepr->getEqualLoc();
    }
  }

  return SourceLoc();
}

bool FloatingRequirementSource::isExplicit() const {
  switch (kind) {
  case Explicit:
    return true;

  case Inferred:
  case NestedTypeNameMatch:
    return false;

  case AbstractProtocol:
    switch (storage.get<const RequirementSource *>()->kind) {
    case RequirementSource::RequirementSignatureSelf:
      return true;

    case RequirementSource::Concrete:
    case RequirementSource::Explicit:
    case RequirementSource::Inferred:
    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::Parent:
    case RequirementSource::ProtocolRequirement:
    case RequirementSource::InferredProtocolRequirement:
    case RequirementSource::Superclass:
      return false;
    }

  case Resolved:
    switch (storage.get<const RequirementSource *>()->kind) {
    case RequirementSource::Explicit:
      return true;

    case RequirementSource::ProtocolRequirement:
      return storage.get<const RequirementSource *>()->parent->kind
        == RequirementSource::RequirementSignatureSelf;

    case RequirementSource::Inferred:
    case RequirementSource::InferredProtocolRequirement:
    case RequirementSource::RequirementSignatureSelf:
    case RequirementSource::Concrete:
    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::Parent:
    case RequirementSource::Superclass:
      return false;
    }
  }
}


FloatingRequirementSource FloatingRequirementSource::asInferred(
                                              const TypeRepr *typeRepr) const {
  switch (kind) {
  case Explicit:
    return forInferred(typeRepr);

  case Inferred:
  case Resolved:
  case NestedTypeNameMatch:
    return *this;

  case AbstractProtocol:
    return viaProtocolRequirement(storage.get<const RequirementSource *>(),
                                  protocolReq.protocol, typeRepr,
                                  /*inferred=*/true);
  }
}

bool FloatingRequirementSource::isRecursive(
                                    Type rootType,
                                    GenericSignatureBuilder &builder) const {
  llvm::SmallSet<std::pair<CanType, ProtocolDecl *>, 4> visitedAssocReqs;
  for (auto storedSource = storage.dyn_cast<const RequirementSource *>();
       storedSource; storedSource = storedSource->parent) {
    if (!storedSource->isProtocolRequirement())
      continue;

    if (!visitedAssocReqs.insert(
                          {storedSource->getStoredType()->getCanonicalType(),
                           storedSource->getProtocolDecl()}).second)
      return true;
  }

  // For a nested type match, look for another type with that name.
  // FIXME: Actually, look for 5 of them. This is totally bogus.
  if (kind == NestedTypeNameMatch) {
    unsigned grossCount = 0;
    auto pa = storage.dyn_cast<const RequirementSource *>()
                ->getAffectedPotentialArchetype();
    while (auto parent = pa->getParent()) {
      if (pa->getNestedName() == nestedName) {
        if (++grossCount > 4) return true;
      }


      pa = parent;
    }
  }

  return false;
}

GenericSignatureBuilder::PotentialArchetype::~PotentialArchetype() {
  for (const auto &nested : NestedTypes) {
    for (auto pa : nested.second) {
      if (pa != this)
        delete pa;
    }
  }

  delete representativeOrEquivClass.dyn_cast<EquivalenceClass *>();
}

std::string GenericSignatureBuilder::PotentialArchetype::getDebugName() const {
  llvm::SmallString<64> result;

  auto parent = getParent();
  if (!parent) {
    return GenericTypeParamType::get(getGenericParamKey().Depth,
                                     getGenericParamKey().Index,
                                     getBuilder()->getASTContext())->getName()
             .str();
  }

  // Nested types.
  result += parent->getDebugName();

  // When building the name for debugging purposes, include the protocol into
  // which the associated type or type alias was resolved.
  ProtocolDecl *proto = nullptr;
  if (auto assocType = getResolvedAssociatedType()) {
    proto = assocType->getProtocol();
  } else if (auto typeAlias = getTypeAliasDecl()) {
    proto = typeAlias->getParent()->getAsProtocolOrProtocolExtensionContext();
  }

  if (proto) {
    result.push_back('[');
    result.push_back('.');
    result.append(proto->getName().str().begin(), proto->getName().str().end());
    result.push_back(']');
  }

  result.push_back('.');
  result.append(getNestedName().str().begin(), getNestedName().str().end());

  return result.str().str();
}

unsigned GenericSignatureBuilder::PotentialArchetype::getNestingDepth() const {
  unsigned Depth = 0;
  for (auto P = getParent(); P; P = P->getParent())
    ++Depth;
  return Depth;
}

void GenericSignatureBuilder::PotentialArchetype::resolveAssociatedType(
       AssociatedTypeDecl *assocType,
       GenericSignatureBuilder &builder) {
  assert(isUnresolvedNestedType && "associated type is already resolved");
  isUnresolvedNestedType = false;
  identifier.assocTypeOrAlias = assocType;
  assert(assocType->getName() == getNestedName());
  assert(builder.Impl->NumUnresolvedNestedTypes > 0 &&
         "Mismatch in number of unresolved nested types");
  --builder.Impl->NumUnresolvedNestedTypes;
}

void GenericSignatureBuilder::PotentialArchetype::resolveTypeAlias(
       TypeAliasDecl *typealias,
       GenericSignatureBuilder &builder) {
  assert(isUnresolvedNestedType && "nested type is already resolved");
  isUnresolvedNestedType = false;
  identifier.assocTypeOrAlias = typealias;
  assert(typealias->getName() == getNestedName());
  assert(builder.Impl->NumUnresolvedNestedTypes > 0 &&
         "Mismatch in number of unresolved nested types");
  --builder.Impl->NumUnresolvedNestedTypes;
}

Optional<ConcreteConstraint>
EquivalenceClass::findAnyConcreteConstraintAsWritten(
                                      PotentialArchetype *preferredPA) const {
  // If we don't have a concrete type, there's no source.
  if (!concreteType) return None;

  // Go look for a source with source-location information.
  Optional<ConcreteConstraint> result;
  for (const auto &constraint : concreteTypeConstraints) {
    if (constraint.source->getLoc().isValid()) {
      result = constraint;
      if (!preferredPA || constraint.archetype == preferredPA)
        return result;
    }
  }

  return result;
}

Optional<ConcreteConstraint>
EquivalenceClass::findAnySuperclassConstraintAsWritten(
                                      PotentialArchetype *preferredPA) const {
  // If we don't have a superclass, there's no source.
  if (!superclass) return None;

  // Go look for a source with source-location information.
  Optional<ConcreteConstraint> result;
  for (const auto &constraint : superclassConstraints) {
    if (constraint.source->getLoc().isValid() &&
        constraint.value->isEqual(superclass)) {
      result = constraint;

      if (!preferredPA || constraint.archetype == preferredPA)
        return result;
    }
  }

  return result;
}

bool EquivalenceClass::isConformanceSatisfiedBySuperclass(
                                                    ProtocolDecl *proto) const {
  auto known = conformsTo.find(proto);
  assert(known != conformsTo.end() && "doesn't conform to this protocol");
  for (const auto &constraint: known->second) {
    if (constraint.source->kind == RequirementSource::Superclass)
      return true;
  }

  return false;
}

void EquivalenceClass::dump(llvm::raw_ostream &out) const {
  out << "Equivalence class represented by "
    << members.front()->getRepresentative()->getDebugName() << ":\n";
  out << "Members: ";
  interleave(members, [&](PotentialArchetype *pa) {
    out << pa->getDebugName();
  }, [&]() {
    out << ", ";
  });
  out << "\nConformances:";
  interleave(conformsTo,
             [&](const std::pair<
                        ProtocolDecl *,
                        std::vector<Constraint<ProtocolDecl *>>> &entry) {
               out << entry.first->getNameStr();
             },
             [&] { out << ", "; });
  out << "\nSame-type constraints:";
  for (const auto &entry : sameTypeConstraints) {
    out << "\n  " << entry.first->getDebugName() << " == ";
    interleave(entry.second,
               [&](const Constraint<PotentialArchetype *> &constraint) {
                 out << constraint.value->getDebugName();

                 if (constraint.source->isDerivedRequirement())
                   out << " [derived]";
               }, [&] {
                 out << ", ";
               });
  }
  if (concreteType)
    out << "\nConcrete type: " << concreteType.getString();
  if (superclass)
    out << "\nSuperclass: " << superclass.getString();
  if (layout)
    out << "\nLayout: " << layout.getString();

  out << "\n";
}

void EquivalenceClass::dump() const {
  dump(llvm::errs());
}

ConstraintResult GenericSignatureBuilder::handleUnresolvedRequirement(
                                   RequirementKind kind,
                                   UnresolvedType lhs,
                                   RequirementRHS rhs,
                                   FloatingRequirementSource source,
                                   UnresolvedHandlingKind unresolvedHandling) {
  switch (unresolvedHandling) {
  case UnresolvedHandlingKind::GenerateConstraints:
    Impl->DelayedRequirements.push_back({kind, lhs, rhs, source});
    return ConstraintResult::Resolved;

  case UnresolvedHandlingKind::ReturnUnresolved:
    return ConstraintResult::Unresolved;
  }
}

const RequirementSource *GenericSignatureBuilder::resolveSuperConformance(
                      GenericSignatureBuilder::PotentialArchetype *pa,
                      ProtocolDecl *proto) {
  // Get the superclass constraint.
  Type superclass = pa->getSuperclass();
  if (!superclass) return nullptr;

  // Lookup the conformance of the superclass to this protocol.
  auto conformance =
    getLookupConformanceFn()(pa->getDependentType({ }, /*allowUnresolved=*/true)
                               ->getCanonicalType(),
                             superclass,
                             proto->getDeclaredInterfaceType()
                               ->castTo<ProtocolType>());
  if (!conformance) return nullptr;

  // Conformance to this protocol is redundant; update the requirement source
  // appropriately.
  auto paEquivClass = pa->getOrCreateEquivalenceClass();
  const RequirementSource *superclassSource;
  if (auto writtenSource =
        paEquivClass->findAnySuperclassConstraintAsWritten(pa))
    superclassSource = writtenSource->source;
  else
    superclassSource = paEquivClass->superclassConstraints.front().source;

  superclassSource =
    superclassSource->viaSuperclass(*this, conformance->getConcrete());
  paEquivClass->conformsTo[proto].push_back({pa, proto, superclassSource});
  return superclassSource;
}

struct GenericSignatureBuilder::ResolvedType {
  llvm::PointerUnion<PotentialArchetype *, Type> paOrT;

  explicit ResolvedType(PotentialArchetype *pa) : paOrT(pa) {}
  explicit ResolvedType(Type ty) : paOrT(ty) {}

public:
  static ResolvedType forConcreteType(Type t) {
    assert(!t->isTypeParameter() &&
           "concrete type with parameter should've been resolved");
    return ResolvedType(t);
  }

  static ResolvedType forPotentialArchetype(PotentialArchetype *pa) {
    return ResolvedType(pa);
  }

  Type getType() const { return paOrT.dyn_cast<Type>(); }
  PotentialArchetype *getPotentialArchetype() const {
    return paOrT.dyn_cast<PotentialArchetype *>();
  }

  bool isType() const { return paOrT.is<Type>(); }
};

/// If there is a same-type requirement to be added for the given nested type
/// due to a superclass constraint on the parent type, add it now.
static void maybeAddSameTypeRequirementForNestedType(
              GenericSignatureBuilder::PotentialArchetype *nestedPA,
              const RequirementSource *superSource,
              GenericSignatureBuilder &builder) {
  // If there's no super conformance, we're done.
  if (!superSource) return;

  auto assocType = nestedPA->getResolvedAssociatedType();
  assert(assocType && "Not resolved to an associated type?");

  // Dig out the type witness.
  auto superConformance = superSource->getProtocolConformance();
  auto concreteType =
    superConformance->getTypeWitness(assocType, builder.getLazyResolver());
  if (!concreteType) return;

  // Add the same-type constraint.
  auto nestedSource = superSource->viaParent(builder, assocType);
  concreteType = superConformance->getDeclContext()
      ->mapTypeOutOfContext(concreteType);

  builder.addSameTypeRequirement(nestedPA, concreteType, nestedSource,
        GenericSignatureBuilder::UnresolvedHandlingKind::GenerateConstraints);
}

/// Walk the members of a protocol.
///
/// This is essentially just a call to \c proto->getMembers(), except that
/// for Objective-C-imported protocols we can simply return an empty declaration
/// range because the generic signature builder only cares about nested types (which
/// Objective-C protocols don't have).
static DeclRange getProtocolMembers(ProtocolDecl *proto) {
  if (proto->hasClangNode())
    return DeclRange(DeclIterator(), DeclIterator());

  return proto->getMembers();
}

bool PotentialArchetype::addConformance(ProtocolDecl *proto,
                                        const RequirementSource *source,
                                        GenericSignatureBuilder &builder) {
  // Check whether we already knew about this conformance.
  auto equivClass = getOrCreateEquivalenceClass();
  auto known = equivClass->conformsTo.find(proto);
  if (known != equivClass->conformsTo.end()) {
    // We already knew about this conformance; record this specific constraint.
    known->second.push_back({this, proto, source});
    return false;
  }

  // Add the conformance along with this constraint.
  equivClass->conformsTo[proto].push_back({this, proto, source});

  // Determine whether there is a superclass constraint where the
  // superclass conforms to this protocol.
  (void)getBuilder()->resolveSuperConformance(this, proto);

  // Resolve any existing nested types that need it.
  for (auto &nested : NestedTypes) {
    (void)updateNestedTypeForConformance(nested.first, proto,
                                         NestedTypeUpdate::ResolveExisting);
  }

  return true;
}

auto PotentialArchetype::getOrCreateEquivalenceClass() const -> EquivalenceClass * {
  // The equivalence class is stored on the representative.
  auto representative = getRepresentative();
  if (representative != this)
    return representative->getOrCreateEquivalenceClass();

  // If we already have an equivalence class, return it.
  if (auto equivClass = getEquivalenceClassIfPresent())
    return equivClass;

  // Create a new equivalence class.
  auto equivClass =
    new EquivalenceClass(const_cast<PotentialArchetype *>(this));
  representativeOrEquivClass = equivClass;
  return equivClass;
}

auto PotentialArchetype::getRepresentative() const -> PotentialArchetype * {
  auto representative =
    representativeOrEquivClass.dyn_cast<PotentialArchetype *>();
  if (!representative)
    return const_cast<PotentialArchetype *>(this);

  // Find the representative.
  PotentialArchetype *result = representative;
  while (auto nextRepresentative =
           result->representativeOrEquivClass.dyn_cast<PotentialArchetype *>())
    result = nextRepresentative;

  // Perform (full) path compression.
  const PotentialArchetype *fixUp = this;
  while (auto nextRepresentative =
           fixUp->representativeOrEquivClass.dyn_cast<PotentialArchetype *>()) {
    fixUp->representativeOrEquivClass = nextRepresentative;
    fixUp = nextRepresentative;
  }

  return result;
}

/// Compare two associated types.
static int compareAssociatedTypes(AssociatedTypeDecl *assocType1,
                                  AssociatedTypeDecl *assocType2) {
  // - by name.
  if (int result = assocType1->getName().str().compare(
                                              assocType2->getName().str()))
    return result;

  // - by protocol, so t_n_m.`P.T` < t_n_m.`Q.T` (given P < Q)
  auto proto1 = assocType1->getProtocol();
  auto proto2 = assocType2->getProtocol();
  if (int compareProtocols = ProtocolType::compareProtocols(&proto1, &proto2))
    return compareProtocols;

  // Error case: if we have two associated types with the same name in the
  // same protocol, just tie-break based on address.
  if (assocType1 != assocType2)
    return assocType1 < assocType2 ? -1 : +1;

  return 0;
}

/// Compare two typealiases in protocols.
static int compareTypeAliases(TypeAliasDecl *typealias1,
                              TypeAliasDecl *typealias2) {
  // - by name.
  if (int result = typealias1->getName().str().compare(
                                              typealias2->getName().str()))
    return result;

  // - by protocol, so t_n_m.`P.T` < t_n_m.`Q.T` (given P < Q)
  auto proto1 =
    typealias1->getDeclContext()->getAsProtocolOrProtocolExtensionContext();
  auto proto2 =
    typealias2->getDeclContext()->getAsProtocolOrProtocolExtensionContext();
  if (int compareProtocols = ProtocolType::compareProtocols(&proto1, &proto2))
    return compareProtocols;

  // Error case: if we have two associated types with the same name in the
  // same protocol, just tie-break based on address.
  if (typealias1 != typealias2)
    return typealias1 < typealias2 ? -1 : +1;

  return 0;
}

/// Canonical ordering for dependent types in generic signatures.
static int compareDependentTypes(PotentialArchetype * const* pa,
                                 PotentialArchetype * const* pb) {
  auto a = *pa, b = *pb;

  // Fast-path check for equality.
  if (a == b)
    return 0;

  // Typealiases must be ordered *after* everything else, to ensure they
  // don't become representatives in the case where a typealias is equated
  // with an associated type.
  if (a->getParent() && b->getParent() &&
      !!a->getTypeAliasDecl() != !!b->getTypeAliasDecl())
    return a->getTypeAliasDecl() ? +1 : -1;

  // Types that are equivalent to concrete types follow types that are still
  // type parameters.
  if (a->isConcreteType() != b->isConcreteType())
    return a->isConcreteType() ? +1 : -1;

  // Ordering is as follows:
  // - Generic params
  if (a->isGenericParam() && b->isGenericParam())
    return a->getGenericParamKey() < b->getGenericParamKey() ? -1 : +1;

  // A generic parameter is always ordered before a nested type.
  if (a->isGenericParam() != b->isGenericParam())
    return a->isGenericParam() ? -1 : +1;

  // - Dependent members
  auto ppa = a->getParent();
  auto ppb = b->getParent();

  // - by base, so t_0_n.`P.T` < t_1_m.`P.T`
  if (int compareBases = compareDependentTypes(&ppa, &ppb))
    return compareBases;

  // - by name, so t_n_m.`P.T` < t_n_m.`P.U`
  if (int compareNames = a->getNestedName().str().compare(
                                                      b->getNestedName().str()))
    return compareNames;

  if (auto *aa = a->getResolvedAssociatedType()) {
    if (auto *ab = b->getResolvedAssociatedType()) {
      if (int result = compareAssociatedTypes(aa, ab))
        return result;
    } else {
      // A resolved archetype is always ordered before an unresolved one.
      return -1;
    }
  } else {
    // A resolved archetype is always ordered before an unresolved one.
    if (b->getResolvedAssociatedType())
      return +1;
  }

  // Make sure typealiases are properly ordered, to avoid crashers.
  if (auto *aa = a->getTypeAliasDecl()) {
    auto *ab = b->getTypeAliasDecl();
    assert(ab != nullptr && "Should have handled this case above");

    if (int result = compareTypeAliases(aa, ab))
      return result;
  }

  // Along the error path where one or both of the potential archetypes was
  // renamed due to typo correction,
  if (a->wasRenamed() || b->wasRenamed()) {
    if (a->wasRenamed() != b->wasRenamed())
      return a->wasRenamed() ? +1 : -1;

    if (int compareNames = a->getOriginalName().str().compare(
                                                    b->getOriginalName().str()))
      return compareNames;
  }

  llvm_unreachable("potential archetype total order failure");
}

PotentialArchetype *PotentialArchetype::getArchetypeAnchor(
                                           GenericSignatureBuilder &builder) {
  // Find the best archetype within this equivalence class.
  PotentialArchetype *rep = getRepresentative();
  PotentialArchetype *anchor;
  if (auto parent = getParent()) {
    // For a nested type, retrieve the parent archetype anchor first.
    auto parentAnchor = parent->getArchetypeAnchor(builder);
    anchor = parentAnchor->getNestedArchetypeAnchor(
                                          getNestedName(), builder,
                                          NestedTypeUpdate::ResolveExisting);

    // FIXME: Hack for cases where we couldn't resolve the nested type.
    if (!anchor)
      anchor = rep;
  } else {
    anchor = rep;
  }

  // Find the best type within this equivalence class.
  for (auto pa : rep->getEquivalenceClassMembers()) {
    if (compareDependentTypes(&pa, &anchor) < 0)
      anchor = pa;
  }

#ifndef NDEBUG
  // Make sure that we did, in fact, get one that is better than all others.
  for (auto pa : anchor->getEquivalenceClassMembers()) {
    assert((pa == anchor || compareDependentTypes(&anchor, &pa) < 0) &&
           compareDependentTypes(&pa, &anchor) >= 0 &&
           "archetype anchor isn't a total order");
  }
#endif

  return anchor;
}

namespace {
  /// Function object to diagnose a conflict in same-type constraints for a
  /// given potential archetype.
  struct DiagnoseSameTypeConflict {
    DiagnosticEngine &diags;
    const RequirementSource *source;
    PotentialArchetype *pa;

    void operator()(Type type1, Type type2) const {
      if (pa->getParent() && pa->getTypeAliasDecl() &&
          source->getLoc().isInvalid()) {
        diags.diagnose(pa->getTypeAliasDecl()->getLoc(),
                       diag::protocol_typealias_conflict,
                       pa->getTypeAliasDecl()->getName(),
                       type1, type2);
        return;
      }

      if (source->getLoc().isValid()) {
        diags.diagnose(source->getLoc(),
                       diag::requires_same_type_conflict,
                       pa->isGenericParam(),
                       pa->getDependentType(/*FIXME: */{ }, true),
                       type1, type2);
      }
    }
  };
} // end anonymous namespace

// Give a nested type the appropriately resolved concrete type, based off a
// parent PA that has a concrete type.
static void concretizeNestedTypeFromConcreteParent(
    GenericSignatureBuilder::PotentialArchetype *parent,
    const RequirementSource *parentConcreteSource,
    GenericSignatureBuilder::PotentialArchetype *nestedPA,
    GenericSignatureBuilder &builder,
    llvm::function_ref<ProtocolConformanceRef(ProtocolDecl *)>
        lookupConformance) {
  auto concreteParent = parent->getConcreteType();
  assert(concreteParent &&
         "attempting to resolve concrete nested type of non-concrete PA");

  // These requirements are all implied based on the parent's concrete
  // conformance.
  auto assocType = nestedPA->getResolvedAssociatedType();
  if (!assocType) return;

  auto source = parentConcreteSource->viaConcrete(builder, /*FIXME: */nullptr)
    ->viaParent(builder, assocType);

  // FIXME: Get the conformance from the parent.
  auto conformance = lookupConformance(assocType->getProtocol());

  Type witnessType;
  if (conformance.isConcrete()) {
    witnessType =
      conformance.getConcrete()
        ->getTypeWitness(assocType, builder.getLazyResolver());
  } else {
    witnessType = DependentMemberType::get(concreteParent, assocType);
  }

  builder.addSameTypeRequirement(
         nestedPA, witnessType, source,
         GenericSignatureBuilder::UnresolvedHandlingKind::GenerateConstraints,
         DiagnoseSameTypeConflict{
           builder.getASTContext().Diags,
           source, nestedPA
         });
}

PotentialArchetype *PotentialArchetype::getNestedType(
                                           Identifier nestedName,
                                           GenericSignatureBuilder &builder) {
  // If we already have a nested type with this name, return it.
  auto known = NestedTypes.find(nestedName);
  if (known != NestedTypes.end())
    return known->second.front();

  // Retrieve the nested archetype anchor, which is the best choice (so far)
  // for this nested type.
  return getNestedArchetypeAnchor(nestedName, builder);
}

PotentialArchetype *PotentialArchetype::getNestedType(
                                            AssociatedTypeDecl *assocType,
                                            GenericSignatureBuilder &builder) {
  return updateNestedTypeForConformance(assocType,
                                        NestedTypeUpdate::AddIfMissing);
}

PotentialArchetype *PotentialArchetype::getNestedType(
                                            TypeAliasDecl *typealias,
                                            GenericSignatureBuilder &builder) {
  return updateNestedTypeForConformance(typealias,
                                        NestedTypeUpdate::AddIfMissing);
}

PotentialArchetype *PotentialArchetype::getNestedArchetypeAnchor(
                                           Identifier name,
                                           GenericSignatureBuilder &builder,
                                           NestedTypeUpdate kind) {
  // Look for the best associated type or typealias within the protocols
  // we know about.
  AssociatedTypeDecl *bestAssocType = nullptr;
  TypeAliasDecl *bestTypeAlias = nullptr;
  SmallVector<TypeAliasDecl *, 4> typealiases;
  auto rep = getRepresentative();
  for (auto proto : rep->getConformsTo()) {
    // Look for an associated type and/or typealias with this name.
    AssociatedTypeDecl *assocType = nullptr;
    TypeAliasDecl *typealias = nullptr;
    for (auto member : proto->lookupDirect(name,
                                           /*ignoreNewExtensions=*/true)) {
      if (!assocType)
        assocType = dyn_cast<AssociatedTypeDecl>(member);

      // FIXME: Filter out typealiases that aren't in the protocol itself?
      if (!typealias)
        typealias = dyn_cast<TypeAliasDecl>(member);
    }

    if (assocType &&
        (!bestAssocType ||
         compareAssociatedTypes(assocType, bestAssocType) < 0))
      bestAssocType = assocType;

    if (typealias) {
      // Record every typealias.
      typealiases.push_back(typealias);

      // Track the best typealias.
      if (!bestTypeAlias || compareTypeAliases(typealias, bestTypeAlias) < 0)
        bestTypeAlias = typealias;
    }
  }

  // If we found an associated type, use it.
  PotentialArchetype *resultPA = nullptr;
  if (bestAssocType) {
    resultPA = updateNestedTypeForConformance(bestAssocType,
                                              NestedTypeUpdate::AddIfMissing);
  }

  // If we have an associated type, drop any typealiases that aren't in
  // the same module as the protocol.
  // FIXME: This is an unprincipled hack for an unprincipled feature.
  typealiases.erase(
    std::remove_if(typealiases.begin(), typealiases.end(),
                   [&](TypeAliasDecl *typealias) {
      return typealias->getParentModule() !=
        typealias->getDeclContext()
          ->getAsNominalTypeOrNominalTypeExtensionContext()->getParentModule();
    }),
    typealiases.end());

  // Update for all of the typealiases with this name, which will introduce
  // various same-type constraints.
  for (auto typealias : typealiases) {
    auto typealiasPA = updateNestedTypeForConformance(typealias,
                                          NestedTypeUpdate::AddIfMissing);
    if (!resultPA && typealias == bestTypeAlias)
      resultPA = typealiasPA;
  }

  if (resultPA)
    return resultPA;

  // Check whether we can add a missing nested type for this case.
  switch (kind) {
  case NestedTypeUpdate::AddIfBetterAnchor:
  case NestedTypeUpdate::AddIfMissing:
    break;

  case NestedTypeUpdate::ResolveExisting:
    // Don't add a new type;
    return nullptr;
  }

  // Build an unresolved type if we don't have one yet.
  auto &nested = NestedTypes[name];
  if (nested.empty()) {
    nested.push_back(new PotentialArchetype(this, name));
    ++builder.Impl->NumUnresolvedNestedTypes;

    auto rep = getRepresentative();
    if (rep != this) {
      auto existingPA = rep->getNestedType(name, builder);

      auto sameNamedSource =
        RequirementSource::forNestedTypeNameMatch(existingPA);
      builder.addSameTypeRequirement(
                                 existingPA, nested.back(), sameNamedSource,
                                 UnresolvedHandlingKind::GenerateConstraints);
    }
  }

  return nested.front();
}


PotentialArchetype *PotentialArchetype::updateNestedTypeForConformance(
                                                       Identifier name,
                                                       ProtocolDecl *proto,
                                                       NestedTypeUpdate kind) {
  /// Determine whether there is an associated type or typealias with this name
  /// in this protocol. If not, there's nothing to do.
  AssociatedTypeDecl *assocType = nullptr;
  TypeAliasDecl *typealias = nullptr;
  for (auto member : proto->lookupDirect(name, /*ignoreNewExtensions=*/true)) {
    if (!assocType)
      assocType = dyn_cast<AssociatedTypeDecl>(member);

    // FIXME: Filter out typealiases that aren't in the protocol itself?
    if (!typealias)
      typealias = dyn_cast<TypeAliasDecl>(member);
  }

  // There is no associated type or typealias with this name in this protocol
  if (!assocType && !typealias)
    return nullptr;

  // If we had both an associated type and a typealias, ignore the latter. This
  // is for ill-formed code.
  if (assocType)
    return updateNestedTypeForConformance(assocType, kind);

  return updateNestedTypeForConformance(typealias, kind);
}

PotentialArchetype *PotentialArchetype::updateNestedTypeForConformance(
                      PointerUnion<AssociatedTypeDecl *, TypeAliasDecl *> type,
                      NestedTypeUpdate kind) {
  AssociatedTypeDecl *assocType = type.dyn_cast<AssociatedTypeDecl *>();
  TypeAliasDecl *typealias = type.dyn_cast<TypeAliasDecl *>();
  if (!assocType && !typealias)
    return nullptr;

  Identifier name = assocType ? assocType->getName() : typealias->getName();
  ProtocolDecl *proto =
    assocType ? assocType->getProtocol()
              : typealias->getDeclContext()
                  ->getAsProtocolOrProtocolExtensionContext();

  // Look for either an unresolved potential archetype (which we can resolve
  // now) or a potential archetype with the appropriate associated type or
  // typealias.
  PotentialArchetype *resultPA = nullptr;
  auto knownNestedTypes = NestedTypes.find(name);
  bool shouldUpdatePA = false;
  auto &builder = *getBuilder();
  if (knownNestedTypes != NestedTypes.end()) {
    for (auto existingPA : knownNestedTypes->second) {
      // Resolve an unresolved potential archetype.
      if (existingPA->isUnresolvedNestedType) {
        if (assocType) {
          existingPA->resolveAssociatedType(assocType, builder);
        } else {
          existingPA->resolveTypeAlias(typealias, builder);
        }

        // We've resolved this nested type; nothing more to do.
        resultPA = existingPA;
        shouldUpdatePA = true;
        break;
      }

      // Do we have an associated-type match?
      if (assocType && existingPA->getResolvedAssociatedType() == assocType) {
        resultPA = existingPA;
        break;
      }

      // Do we have a typealias match?
      if (typealias && existingPA->getTypeAliasDecl() == typealias) {
        resultPA = existingPA;
        break;
      }
    }
  }

  // If we don't have a result potential archetype yet, we may need to add one.
  if (!resultPA) {
    switch (kind) {
    case NestedTypeUpdate::AddIfBetterAnchor:
      // FIXME: The loop above should have kept track of whether this type
      // would make a better anchor, so we can bail out here if the answer is
      // "no".
      LLVM_FALLTHROUGH;

    case NestedTypeUpdate::AddIfMissing: {
      if (assocType)
        resultPA = new PotentialArchetype(this, assocType);
      else
        resultPA = new PotentialArchetype(this, typealias);

      auto &allNested = NestedTypes[name];
      allNested.push_back(resultPA);

      // We created a new type, which might be equivalent to a type by the
      // same name elsewhere.
      PotentialArchetype *existingPA = nullptr;
      if (allNested.size() > 1) {
        existingPA = allNested.front();
      } else {
        auto rep = getRepresentative();
        if (rep != this) {
          if (assocType)
            existingPA = rep->getNestedType(assocType, builder);
          else
            existingPA = rep->getNestedType(name, builder);
        }
      }

      if (existingPA) {
        auto sameNamedSource =
          RequirementSource::forNestedTypeNameMatch(existingPA);
        builder.addSameTypeRequirement(
                                 existingPA, resultPA, sameNamedSource,
                                 UnresolvedHandlingKind::GenerateConstraints);
      }

      shouldUpdatePA = true;
      break;
    }

    case NestedTypeUpdate::ResolveExisting:
      break;
    }
  }

  // If we still don't have a result potential archetype, we're done.
  if (!resultPA)
    return nullptr;

  // If we have a potential archetype that requires more processing, do so now.
  if (shouldUpdatePA) {
    // For typealiases, introduce a same-type requirement to the aliased type.
    if (typealias) {
      // FIXME (recursive decl validation): if the alias doesn't have an
      // interface type when getNestedType is called while building a
      // protocol's generic signature (i.e. during validation), then it'll
      // fail completely, because building that alias's interface type
      // requires the protocol to be validated. This seems to occur when the
      // alias's RHS involves archetypes from the protocol.
      if (!typealias->hasInterfaceType())
        builder.getLazyResolver()->resolveDeclSignature(typealias);
      if (typealias->hasInterfaceType()) {
        // The protocol typealias has an underlying type written in terms
        // of the protocol's 'Self' type.
        auto type = typealias->getDeclaredInterfaceType();

        // Substitute in the type of the current PotentialArchetype in
        // place of 'Self' here.
        auto subMap = SubstitutionMap::getProtocolSubstitutions(
          proto, getDependentType(/*genericParams=*/{},
                                  /*allowUnresolved=*/true),
          ProtocolConformanceRef(proto));
        type = type.subst(subMap, SubstFlags::UseErrorType);

        builder.addSameTypeRequirement(
                         UnresolvedType(resultPA),
                         UnresolvedType(type),
                         RequirementSource::forNestedTypeNameMatch(resultPA),
                         UnresolvedHandlingKind::GenerateConstraints);
      }
    }

    // If there's a superclass constraint that conforms to the protocol,
    // add the appropriate same-type relationship.
    if (auto superSource = builder.resolveSuperConformance(this, proto))
      maybeAddSameTypeRequirementForNestedType(resultPA, superSource, builder);

    // We know something concrete about the parent PA, so we need to propagate
    // that information to this new archetype.
    // FIXME: This feels like massive overkill. Why do we have to loop?
    if (isConcreteType()) {
      for (auto equivT : getRepresentative()->getEquivalenceClassMembers()) {
        concretizeNestedTypeFromConcreteParent(
            equivT, RequirementSource::forNestedTypeNameMatch(this),
            resultPA, builder,
            [&](ProtocolDecl *proto) -> ProtocolConformanceRef {
              auto depTy = resultPA->getDependentType({},
                                                      /*allowUnresolved=*/true)
                               ->getCanonicalType();
              auto protocolTy =
                  proto->getDeclaredInterfaceType()->castTo<ProtocolType>();
              auto conformance = builder.getLookupConformanceFn()(
                  depTy, getConcreteType(), protocolTy);
              assert(conformance &&
                     "failed to find PA's conformance to known protocol");
              return *conformance;
            });
      }
    }
  }

  return resultPA;
}

Type GenericSignatureBuilder::PotentialArchetype::getTypeInContext(
                                               GenericSignatureBuilder &builder,
                                               GenericEnvironment *genericEnv) {
  ArrayRef<GenericTypeParamType *> genericParams =
    genericEnv->getGenericParams();

  // Retrieve the archetype from the archetype anchor in this equivalence class.
  // The anchor must not have any concrete parents (otherwise we would just
  // use the representative).
  auto archetypeAnchor = getArchetypeAnchor(builder);
  if (archetypeAnchor != this)
    return archetypeAnchor->getTypeInContext(builder, genericEnv);

  auto representative = getRepresentative();
  auto equivClass = representative->getOrCreateEquivalenceClass();
  ASTContext &ctx = genericEnv->getGenericSignature()->getASTContext();

  // Return a concrete type or archetype we've already resolved.
  if (Type concreteType = representative->getConcreteType()) {
    // Otherwise, substitute in the archetypes in the environment.
    // If this has a recursive type, return an error type.
    if (representative->RecursiveConcreteType) {
      return ErrorType::get(getDependentType(genericParams,
                                             /*allowUnresolved=*/true));
    }

    return genericEnv->mapTypeIntoContext(concreteType,
                                          builder.getLookupConformanceFn());
  }

  // Local function to check whether we have a generic parameter that has
  // already been recorded
  auto getAlreadyRecoveredGenericParam = [&]() -> Type {
    if (!isGenericParam()) return Type();

    auto type = genericEnv->getMappingIfPresent(getGenericParamKey());
    if (!type) return Type();

    // We already have a mapping for this generic parameter in the generic
    // environment. Return it.
    return *type;
  };

  AssociatedTypeDecl *assocType = nullptr;
  ArchetypeType *ParentArchetype = nullptr;
  if (auto parent = getParent()) {
    // For nested types, first substitute into the parent so we can form the
    // proper nested type.
    auto parentTy = parent->getTypeInContext(builder, genericEnv);
    if (!parentTy)
      return ErrorType::get(getDependentType(genericParams,
                                             /*allowUnresolved=*/true));

    ParentArchetype = parentTy->getAs<ArchetypeType>();
    if (!ParentArchetype) {
      LazyResolver *resolver = ctx.getLazyResolver();
      assert(resolver && "need a lazy resolver");
      (void) resolver;

      // Resolve the member type.
      auto type = getDependentType(genericParams, /*allowUnresolved=*/false);
      if (type->hasError())
        return type;

      auto depMemberType = type->castTo<DependentMemberType>();
      Type memberType =
        depMemberType->substBaseType(parentTy,
                                     builder.getLookupConformanceFn());

      // If the member type maps to an archetype, resolve that archetype.
      if (auto memberPA =
            builder.resolveArchetype(
                               memberType,
                               ArchetypeResolutionKind::CompleteWellFormed)) {
        if (memberPA->getRepresentative() != representative) {
          return memberPA->getTypeInContext(builder, genericEnv);
        }

        llvm_unreachable("we have no parent archetype");
      }


      // Otherwise, it's a concrete type.
      return genericEnv->mapTypeIntoContext(memberType,
                                            builder.getLookupConformanceFn());
    }

    // Check whether the parent already has a nested type with this name. If
    // so, return it directly.
    if (auto nested = ParentArchetype->getNestedTypeIfKnown(getNestedName()))
      return *nested;

    // We will build the archetype below.
    assocType = getResolvedAssociatedType();
  } else if (auto result = getAlreadyRecoveredGenericParam()) {
    return result;
  }

  // Determine the superclass for the archetype. If it exists and involves
  // type parameters, substitute them.
  Type superclass = representative->getSuperclass();
  if (superclass && superclass->hasTypeParameter()) {
    if (representative->RecursiveSuperclassType) {
      superclass = ErrorType::get(superclass);
    } else {
      superclass = genericEnv->mapTypeIntoContext(
                                              superclass,
                                              builder.getLookupConformanceFn());

      // We might have recursively recorded the archetype; if so, return early.
      // FIXME: This should be detectable before we end up building archetypes.
      if (auto result = getAlreadyRecoveredGenericParam())
        return result;
    }
  }

  LayoutConstraint layout = representative->getLayout();

  // Build a new archetype.

  // Collect the protocol conformances for the archetype.
  SmallVector<ProtocolDecl *, 4> Protos;
  for (auto proto : representative->getConformsTo()) {
    if (!equivClass || !equivClass->isConformanceSatisfiedBySuperclass(proto))
      Protos.push_back(proto);
  }

  // Create the archetype.
  //
  // Note that we delay the computation of the superclass until after we
  // create the archetype, in case the superclass references the archetype
  // itself.
  ArchetypeType *arch;
  if (ParentArchetype) {
    // If we were unable to resolve this as an associated type, produce an
    // error type.
    if (!assocType) {
      return ErrorType::get(getDependentType(genericParams,
                                             /*allowUnresolved=*/true));
    }

    // Create a nested archetype.
    arch = ArchetypeType::getNew(ctx, ParentArchetype, assocType, Protos,
                                 superclass, layout);

    // Register this archetype with its parent.
    ParentArchetype->registerNestedType(getNestedName(), arch);
  } else {
    // Create a top-level archetype.
    Identifier name =
      genericParams[getGenericParamKey().findIndexIn(genericParams)]->getName();
    arch = ArchetypeType::getNew(ctx, genericEnv, name, Protos,
                                 superclass, layout);

    // Register the archetype with the generic environment.
    genericEnv->addMapping(getGenericParamKey(), arch);
  }

  return arch;
}

void ArchetypeType::resolveNestedType(
                                    std::pair<Identifier, Type> &nested) const {
  auto genericEnv = getGenericEnvironment();
  auto &builder = *genericEnv->getGenericSignatureBuilder();

  Type interfaceType =
    genericEnv->mapTypeOutOfContext(const_cast<ArchetypeType *>(this));
  auto parentPA =
    builder.resolveArchetype(interfaceType,
                             ArchetypeResolutionKind::CompleteWellFormed);
  auto memberPA = parentPA->getNestedType(nested.first, builder);
  auto result = memberPA->getTypeInContext(builder, genericEnv);
  assert(!nested.second ||
         nested.second->isEqual(result) ||
         (nested.second->hasError() && result->hasError()));
  nested.second = result;
}

Type GenericSignatureBuilder::PotentialArchetype::getDependentType(
                                ArrayRef<GenericTypeParamType *> genericParams,
                                bool allowUnresolved) {
  if (auto parent = getParent()) {
    Type parentType = parent->getDependentType(genericParams,
                                               allowUnresolved);
    if (parentType->hasError())
      return parentType;

    // If we've resolved to an associated type, use it.
    if (auto assocType = getResolvedAssociatedType())
      return DependentMemberType::get(parentType, assocType);

    // If we don't allow unresolved dependent member types, fail.
    if (!allowUnresolved)
      return ErrorType::get(getDependentType(genericParams,
                                             /*allowUnresolved=*/true));

    return DependentMemberType::get(parentType, getNestedName());
  }
  
  assert(isGenericParam() && "Not a generic parameter?");

  // FIXME: This is a temporary workaround.
  if (genericParams.empty())
    genericParams = getBuilder()->Impl->GenericParams;

  unsigned index = getGenericParamKey().findIndexIn(genericParams);
  return genericParams[index];
}

void GenericSignatureBuilder::PotentialArchetype::dump() const {
  dump(llvm::errs(), nullptr, 0);
}

void GenericSignatureBuilder::PotentialArchetype::dump(llvm::raw_ostream &Out,
                                                       SourceManager *SrcMgr,
                                                       unsigned Indent) const {
  // Print name.
  if (Indent == 0 || isGenericParam())
    Out << getDebugName();
  else
    Out.indent(Indent) << getNestedName();

  auto equivClass = getEquivalenceClassIfPresent();

  // Print superclass.
  if (equivClass && equivClass->superclass) {
    for (const auto &constraint : equivClass->superclassConstraints) {
      if (constraint.archetype != this) continue;

      Out << " : ";
      constraint.value.print(Out);

      Out << " ";
      if (!constraint.source->isDerivedRequirement())
        Out << "*";
      Out << "[";
      constraint.source->print(Out, SrcMgr);
      Out << "]";
    }
  }

  // Print concrete type.
  if (equivClass && equivClass->concreteType) {
    for (const auto &constraint : equivClass->concreteTypeConstraints) {
      if (constraint.archetype != this) continue;

      Out << " == ";
      constraint.value.print(Out);

      Out << " ";
      if (!constraint.source->isDerivedRequirement())
        Out << "*";
      Out << "[";
      constraint.source->print(Out, SrcMgr);
      Out << "]";
    }
  }

  // Print requirements.
  if (equivClass) {
    bool First = true;
    for (const auto &entry : equivClass->conformsTo) {
      for (const auto &constraint : entry.second) {
        if (constraint.archetype != this) continue;

        if (First) {
          First = false;
          Out << ": ";
        } else {
          Out << " & ";
        }

        Out << constraint.value->getName().str() << " ";
        if (!constraint.source->isDerivedRequirement())
          Out << "*";
        Out << "[";
        constraint.source->print(Out, SrcMgr);
        Out << "]";
      }
    }
  }

  if (getRepresentative() != this) {
    Out << " [represented by " << getRepresentative()->getDebugName() << "]";
  }

  if (getEquivalenceClassMembers().size() > 1) {
    Out << " [equivalence class ";
    bool isFirst = true;
    for (auto equiv : getEquivalenceClassMembers()) {
      if (equiv == this) continue;

      if (isFirst) isFirst = false;
      else Out << ", ";

      Out << equiv->getDebugName();
    }
    Out << "]";
  }

  Out << "\n";

  // Print nested types.
  for (const auto &nestedVec : NestedTypes) {
    for (auto nested : nestedVec.second) {
      nested->dump(Out, SrcMgr, Indent + 2);
    }
  }
}

#pragma mark Equivalence classes
EquivalenceClass::EquivalenceClass(PotentialArchetype *representative) {
  members.push_back(representative);
}

GenericSignatureBuilder::GenericSignatureBuilder(
                               ASTContext &ctx,
                               std::function<GenericFunction> lookupConformance)
  : Context(ctx), Diags(Context.Diags), Impl(new Implementation) {
  Impl->LookupConformance = std::move(lookupConformance);
}

GenericSignatureBuilder::GenericSignatureBuilder(GenericSignatureBuilder &&) = default;

GenericSignatureBuilder::~GenericSignatureBuilder() {
  if (!Impl)
    return;

  SmallVector<RequirementSource *, 4> requirementSources;
  for (auto &reqSource : Impl->RequirementSources)
    requirementSources.push_back(&reqSource);
  Impl->RequirementSources.clear();
  for (auto reqSource : requirementSources)
    delete reqSource;

  for (auto PA : Impl->PotentialArchetypes)
    delete PA;
}

std::function<GenericFunction>
GenericSignatureBuilder::getLookupConformanceFn() const {
  return Impl->LookupConformance;
}

LazyResolver *GenericSignatureBuilder::getLazyResolver() const { 
  return Context.getLazyResolver();
}

PotentialArchetype *GenericSignatureBuilder::resolveArchetype(
                                     Type type,
                                     ArchetypeResolutionKind resolutionKind) {
  if (auto genericParam = type->getAs<GenericTypeParamType>()) {
    unsigned index = GenericParamKey(genericParam).findIndexIn(
                                                           Impl->GenericParams);
    if (index < Impl->GenericParams.size())
      return Impl->PotentialArchetypes[index];

    return nullptr;
  }

  if (auto dependentMember = type->getAs<DependentMemberType>()) {
    auto base = resolveArchetype(dependentMember->getBase(), resolutionKind);
    if (!base)
      return nullptr;

    // Figure out what kind of nested type update we want.
    typedef PotentialArchetype::NestedTypeUpdate NestedTypeUpdate;
    NestedTypeUpdate updateKind;
    switch (resolutionKind) {
    case ArchetypeResolutionKind::AlreadyKnown:
      updateKind = NestedTypeUpdate::ResolveExisting;
      break;

    case ArchetypeResolutionKind::AlwaysPartial:
    case ArchetypeResolutionKind::CompleteWellFormed:
      updateKind = NestedTypeUpdate::AddIfMissing;
      break;
    }

    // If we know the associated type already, get that specific type.
    if (auto assocType = dependentMember->getAssocType())
      return base->updateNestedTypeForConformance(assocType, updateKind);

    // Resolve based on name alone.
    auto name = dependentMember->getName();
    switch (resolutionKind) {
    case ArchetypeResolutionKind::AlreadyKnown: {
      auto known = base->NestedTypes.find(name);
      if (known == base->NestedTypes.end())
        return nullptr;

      return known->second.front();
    }

    case ArchetypeResolutionKind::AlwaysPartial:
    case ArchetypeResolutionKind::CompleteWellFormed:
      return base->getNestedArchetypeAnchor(name, *this, updateKind);
    }
  }

  return nullptr;
}

auto GenericSignatureBuilder::resolve(UnresolvedType paOrT,
                                      FloatingRequirementSource source)
    -> Optional<ResolvedType> {
  auto pa = paOrT.dyn_cast<PotentialArchetype *>();
  if (auto type = paOrT.dyn_cast<Type>()) {
    // If it's not a type parameter,
    if (!type->isTypeParameter())
      return ResolvedType::forConcreteType(type);

    // Determine what kind of resolution we want.
    ArchetypeResolutionKind resolutionKind =
      ArchetypeResolutionKind::AlwaysPartial;
    if (!source.isExplicit() && source.isRecursive(type, *this))
      resolutionKind = ArchetypeResolutionKind::AlreadyKnown;

    // Attempt to resolve the type parameter to a potential archetype. If this
    // fails, it's because we weren't allowed to resolve anything now.
    pa = resolveArchetype(type, resolutionKind);
    if (!pa) return None;
  }

  auto rep = pa->getRepresentative();
  if (!rep->getParent() || !rep->getTypeAliasDecl())
    return ResolvedType::forPotentialArchetype(pa);

  // We're assuming that an equivalence class with a type alias representative
  // doesn't have a "true" (i.e. associated type) potential archetype.
  assert(llvm::all_of(rep->getEquivalenceClassMembers(),
                      [&](PotentialArchetype *pa) {
                        return pa->getParent() && pa->getTypeAliasDecl();
                      }) &&
         "unexpected typealias representative with non-typealias equivalent");

  return ResolvedType::forPotentialArchetype(pa);
}

void GenericSignatureBuilder::addGenericParameter(GenericTypeParamDecl *GenericParam) {
  addGenericParameter(
     GenericParam->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
}

bool GenericSignatureBuilder::addGenericParameterRequirements(
                                           GenericTypeParamDecl *GenericParam) {
  GenericParamKey Key(GenericParam);
  auto PA = Impl->PotentialArchetypes[Key.findIndexIn(Impl->GenericParams)];
  
  // Add the requirements from the declaration.
  return isErrorResult(
           addInheritedRequirements(GenericParam, PA, nullptr,
                                    GenericParam->getModuleContext()));
}

void GenericSignatureBuilder::addGenericParameter(GenericTypeParamType *GenericParam) {
  GenericParamKey Key(GenericParam);
  assert(Impl->GenericParams.empty() ||
         ((Key.Depth == Impl->GenericParams.back()->getDepth() &&
           Key.Index == Impl->GenericParams.back()->getIndex() + 1) ||
          (Key.Depth > Impl->GenericParams.back()->getDepth() &&
           Key.Index == 0)));

  // Create a potential archetype for this type parameter.
  auto PA = new PotentialArchetype(this, GenericParam);
  Impl->GenericParams.push_back(GenericParam);
  Impl->PotentialArchetypes.push_back(PA);
}

/// Visit all of the types that show up in the list of inherited
/// types.
static ConstraintResult visitInherited(
         ArrayRef<TypeLoc> inheritedTypes,
         llvm::function_ref<ConstraintResult(Type, const TypeRepr *)> visitType,
         llvm::function_ref<ConstraintResult(LayoutConstraint, const TypeRepr *)> visitLayout) {
  // Local function that (recursively) adds inherited types.
  ConstraintResult result = ConstraintResult::Resolved;
  std::function<void(Type, const TypeRepr *)> visitInherited;

  // FIXME: Should this whole thing use getExistentialLayout() instead?

  visitInherited = [&](Type inheritedType, const TypeRepr *typeRepr) {
    // Decompose explicitly-written protocol compositions.
    if (auto composition = dyn_cast_or_null<CompositionTypeRepr>(typeRepr)) {
      if (auto compositionType
            = inheritedType->getAs<ProtocolCompositionType>()) {
        unsigned index = 0;
        for (auto memberType : compositionType->getMembers()) {
          visitInherited(memberType, composition->getTypes()[index]);
          index++;
        }

        auto layout = compositionType->getExistentialLayout()
          .getLayoutConstraint();
        if (layout)
          visitLayout(layout, composition);

        return;
      }
    }

    auto recursiveResult = visitType(inheritedType, typeRepr);
    if (isErrorResult(recursiveResult) && !isErrorResult(result))
      result = recursiveResult;
  };

  // Visit all of the inherited types.
  for (auto inherited : inheritedTypes) {
    visitInherited(inherited.getType(), inherited.getTypeRepr());
  }

  return result;
}

ConstraintResult GenericSignatureBuilder::addConformanceRequirement(
                               PotentialArchetype *PAT,
                               ProtocolDecl *Proto,
                               const RequirementSource *Source) {
  // Add the requirement, if we haven't done so already.
  if (!PAT->addConformance(Proto, Source, *this))
    return ConstraintResult::Resolved;

  auto concreteSelf = PAT->getDependentType({}, /*allowUnresolved=*/true);
  auto protocolSubMap = SubstitutionMap::getProtocolSubstitutions(
      Proto, concreteSelf, ProtocolConformanceRef(Proto));

  // Use the requirement signature to avoid rewalking the entire protocol.  This
  // cannot compute the requirement signature directly, because that may be
  // infinitely recursive: this code is also used to construct it.
  if (Proto->isRequirementSignatureComputed()) {
    auto reqSig = Proto->getRequirementSignature();

    auto innerSource =
      FloatingRequirementSource::viaProtocolRequirement(Source, Proto,
                                                        /*inferred=*/false);
    for (auto req : reqSig->getRequirements()) {
      auto reqResult = addRequirement(req, innerSource, nullptr,
                                      &protocolSubMap);
      if (isErrorResult(reqResult)) return reqResult;
    }

    return ConstraintResult::Resolved;
  }

  // Add all of the inherited protocol requirements, recursively.
  if (auto resolver = getLazyResolver())
    resolver->resolveInheritedProtocols(Proto);

  auto protoModule = Proto->getParentModule();

  auto inheritedReqResult =
    addInheritedRequirements(Proto, PAT, Source, protoModule);
  if (isErrorResult(inheritedReqResult))
    return inheritedReqResult;

  // Add any requirements in the where clause on the protocol.
  if (auto WhereClause = Proto->getTrailingWhereClause()) {
    for (auto &req : WhereClause->getRequirements()) {
      auto innerSource = FloatingRequirementSource::viaProtocolRequirement(
          Source, Proto, &req, /*inferred=*/false);
      addRequirement(&req, innerSource, &protocolSubMap, protoModule);
    }
  }

  // Collect all of the inherited associated types and typealiases in the
  // inherited protocols (recursively).
  DenseMap<DeclName, TinyPtrVector<TypeDecl *>> inheritedTypeDecls;
  {
    Proto->walkInheritedProtocols(
        [&](ProtocolDecl *inheritedProto) -> TypeWalker::Action {
      if (inheritedProto == Proto) return TypeWalker::Action::Continue;

      for (auto req : getProtocolMembers(inheritedProto)) {
        if (auto typeReq = dyn_cast<TypeDecl>(req))
          inheritedTypeDecls[typeReq->getFullName()].push_back(typeReq);
      }
      return TypeWalker::Action::Continue;
    });
  }

  // Local function to find the insertion point for the protocol's "where"
  // clause, as well as the string to start the insertion ("where" or ",");
  auto getProtocolWhereLoc = [&]() -> std::pair<SourceLoc, const char *> {
    // Already has a trailing where clause.
    if (auto trailing = Proto->getTrailingWhereClause())
      return { trailing->getRequirements().back().getSourceRange().End, ", " };

    // Inheritance clause.
    return { Proto->getInherited().back().getSourceRange().End, " where " };
  };

  // Retrieve the set of requirements that a given associated type declaration
  // produces, in the form that would be seen in the where clause.
  auto getAssociatedTypeReqs = [&](AssociatedTypeDecl *assocType,
                                   const char *start) {
    std::string result;
    {
      llvm::raw_string_ostream out(result);
      out << start;
      interleave(assocType->getInherited(), [&](TypeLoc inheritedType) {
        out << assocType->getFullName() << ": ";
        if (auto inheritedTypeRepr = inheritedType.getTypeRepr())
          inheritedTypeRepr->print(out);
        else
          inheritedType.getType().print(out);
      }, [&] {
        out << ", ";
      });
    }
    return result;
  };

  // Retrieve the requirement that a given typealias introduces when it
  // overrides an inherited associated type with the same name, as a string
  // suitable for use in a where clause.
  auto getTypeAliasReq = [&](TypeAliasDecl *typealias, const char *start) {
    std::string result;
    {
      llvm::raw_string_ostream out(result);
      out << start;
      out << typealias->getFullName() << " == ";
      if (auto underlyingTypeRepr =
            typealias->getUnderlyingTypeLoc().getTypeRepr())
        underlyingTypeRepr->print(out);
      else
        typealias->getUnderlyingTypeLoc().getType().print(out);
    }
    return result;
  };

  // Add requirements for each of the associated types.
  for (auto Member : getProtocolMembers(Proto)) {
    if (auto AssocType = dyn_cast<AssociatedTypeDecl>(Member)) {
      // Add requirements placed directly on this associated type.
      Type assocType = DependentMemberType::get(concreteSelf, AssocType);
      auto assocResult =
        addInheritedRequirements(AssocType, assocType, Source, protoModule);
      if (isErrorResult(assocResult))
        return assocResult;

      if (auto WhereClause = AssocType->getTrailingWhereClause()) {
        for (auto &req : WhereClause->getRequirements()) {
          auto innerSource =
            FloatingRequirementSource::viaProtocolRequirement(
                                      Source, Proto, &req, /*inferred=*/false);
          addRequirement(&req, innerSource, &protocolSubMap, protoModule);
        }
      }

      // Check whether we inherited any types with the same name.
      auto knownInherited = inheritedTypeDecls.find(AssocType->getFullName());
      if (knownInherited == inheritedTypeDecls.end()) continue;

      bool shouldWarnAboutRedeclaration =
        Source->kind == RequirementSource::RequirementSignatureSelf &&
        AssocType->getDefaultDefinitionLoc().isNull();
      for (auto inheritedType : knownInherited->getSecond()) {
        // If we have inherited associated type...
        if (auto inheritedAssocTypeDecl =
              dyn_cast<AssociatedTypeDecl>(inheritedType)) {
          // FIXME: Wire up same-type constraint.

          // Complain about the first redeclaration.
          if (shouldWarnAboutRedeclaration) {
            auto inheritedFromProto = inheritedAssocTypeDecl->getProtocol();
            auto fixItWhere = getProtocolWhereLoc();
            Diags.diagnose(AssocType,
                           diag::inherited_associated_type_redecl,
                           AssocType->getFullName(),
                           inheritedFromProto->getDeclaredInterfaceType())
              .fixItInsertAfter(
                         fixItWhere.first,
                         getAssociatedTypeReqs(AssocType, fixItWhere.second))
              .fixItRemove(AssocType->getSourceRange());

            Diags.diagnose(inheritedAssocTypeDecl, diag::decl_declared_here,
                           inheritedAssocTypeDecl->getFullName());

            shouldWarnAboutRedeclaration = false;
          }

          continue;
        }

        // FIXME: this is a weird situation.
      }

      inheritedTypeDecls.erase(knownInherited);
      continue;
    }

    if (auto typealias = dyn_cast<TypeAliasDecl>(Member)) {
      // Check whether we inherited any types with the same name.
      auto knownInherited = inheritedTypeDecls.find(typealias->getFullName());
      if (knownInherited == inheritedTypeDecls.end()) continue;

      bool shouldWarnAboutRedeclaration =
        Source->kind == RequirementSource::RequirementSignatureSelf;

      for (auto inheritedType : knownInherited->getSecond()) {
        // If we have inherited associated type...
        if (auto inheritedAssocTypeDecl =
              dyn_cast<AssociatedTypeDecl>(inheritedType)) {
          // FIXME: Wire up same-type constraint.

          // Warn that one should use where clauses for this.
          if (shouldWarnAboutRedeclaration) {
            auto inheritedFromProto = inheritedAssocTypeDecl->getProtocol();
            auto fixItWhere = getProtocolWhereLoc();
            Diags.diagnose(typealias,
                           diag::typealias_override_associated_type,
                           typealias->getFullName(),
                           inheritedFromProto->getDeclaredInterfaceType())
              .fixItInsertAfter(fixItWhere.first,
                                getTypeAliasReq(typealias, fixItWhere.second))
              .fixItRemove(typealias->getSourceRange());
            Diags.diagnose(inheritedAssocTypeDecl, diag::decl_declared_here,
                           inheritedAssocTypeDecl->getFullName());

            shouldWarnAboutRedeclaration = false;
          }

          continue;
        }

        // FIXME: More typealiases
      }

      inheritedTypeDecls.erase(knownInherited);
      continue;
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addLayoutRequirementDirect(
                                             PotentialArchetype *PAT,
                                             LayoutConstraint Layout,
                                             const RequirementSource *Source) {
  auto equivClass = PAT->getOrCreateEquivalenceClass();

  // Record this layout constraint.
  equivClass->layoutConstraints.push_back({PAT, Layout, Source});

  // Update the layout in the equivalence class, if we didn't have one already.
  if (!equivClass->layout)
    equivClass->layout = Layout;
  else {
    // Try to merge layout constraints.
    auto mergedLayout = equivClass->layout.merge(Layout);
    if (mergedLayout->isKnownLayout() && mergedLayout != equivClass->layout)
      equivClass->layout = mergedLayout;
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addLayoutRequirement(
                                             UnresolvedType subject,
                                             LayoutConstraint layout,
                                             FloatingRequirementSource source,
                                             UnresolvedHandlingKind unresolvedHandling) {
  // Resolve the subject.
  auto resolvedSubject = resolve(subject, source);
  if (!resolvedSubject) {
    return handleUnresolvedRequirement(RequirementKind::Layout, subject,
                                       layout, source, unresolvedHandling);
  }

  // If this layout constraint applies to a concrete type, we can fully
  // resolve it now.
  if (resolvedSubject->isType()) {
    // If a layout requirement was explicitly written on a concrete type,
    // complain.
    if (source.isExplicit() && source.getLoc().isValid()) {
      Diags.diagnose(source.getLoc(), diag::requires_not_suitable_archetype,
                     0, TypeLoc::withoutLoc(resolvedSubject->getType()), 0);
      return ConstraintResult::Concrete;
    }

    // FIXME: Check whether the layout constraint makes sense for this
    // concrete type!

    return ConstraintResult::Resolved;
  }

  auto pa = resolvedSubject->getPotentialArchetype();
  return addLayoutRequirementDirect(pa, layout, source.getSource(pa));
}

void GenericSignatureBuilder::updateSuperclass(
                                           PotentialArchetype *T,
                                           Type superclass,
                                           const RequirementSource *source) {
  auto equivClass = T->getOrCreateEquivalenceClass();

  // Local function to handle the update of superclass conformances
  // when the superclass constraint changes.
  auto updateSuperclassConformances = [&] {
    for (auto proto : T->getConformsTo()) {
      if (auto superSource = resolveSuperConformance(T, proto)) {
        for (auto req : getProtocolMembers(proto)) {
          auto assocType = dyn_cast<AssociatedTypeDecl>(req);
          if (!assocType) continue;

          const auto &nestedTypes = T->getNestedTypes();
          auto nested = nestedTypes.find(assocType->getName());
          if (nested == nestedTypes.end()) continue;

          for (auto nestedPA : nested->second) {
            if (nestedPA->getResolvedAssociatedType() == assocType)
              maybeAddSameTypeRequirementForNestedType(nestedPA, superSource,
                                                       *this);
          }
        }
      }
    }
  };

  // If we haven't yet recorded a superclass constraint for this equivalence
  // class, do so now.
  if (!equivClass->superclass) {
    equivClass->superclass = superclass;
    updateSuperclassConformances();
    // Presence of a superclass constraint implies a _Class layout
    // constraint.
    auto layoutReqSource = source->viaSuperclass(*this, nullptr);
    addLayoutRequirementDirect(T,
                         LayoutConstraint::getLayoutConstraint(
                             superclass->getClassOrBoundGenericClass()->isObjC()
                                 ? LayoutConstraintKind::Class
                                 : LayoutConstraintKind::NativeClass,
                             getASTContext()),
                         layoutReqSource);
    return;
  }

  // T already has a superclass; make sure it's related.
  auto existingSuperclass = equivClass->superclass;
  // TODO: In principle, this could be isBindableToSuperclassOf instead of
  // isExactSubclassOf. If you had:
  //
  //   class Foo<T>
  //   class Bar: Foo<Int>
  //
  //   func foo<T, U where U: Foo<T>, U: Bar>(...) { ... }
  //
  // then the second constraint should be allowed, constraining U to Bar
  // and secondarily imposing a T == Int constraint.
  if (existingSuperclass->isExactSuperclassOf(superclass)) {
    equivClass->superclass = superclass;

    // We've strengthened the bound, so update superclass conformances.
    updateSuperclassConformances();
    return;
  }

  return;
}

ConstraintResult GenericSignatureBuilder::addSuperclassRequirementDirect(
                                            PotentialArchetype *T,
                                            Type superclass,
                                            const RequirementSource *source) {
  // Record the constraint.
  T->getOrCreateEquivalenceClass()->superclassConstraints
    .push_back(ConcreteConstraint{T, superclass, source});

  // Update the equivalence class with the constraint.
  updateSuperclass(T, superclass, source);
  return ConstraintResult::Resolved;
}

/// Map an unresolved type to a requirement right-hand-side.
static GenericSignatureBuilder::RequirementRHS
toRequirementRHS(GenericSignatureBuilder::UnresolvedType unresolved) {
  if (auto pa = unresolved.dyn_cast<PotentialArchetype *>())
    return pa;

  return unresolved.dyn_cast<Type>();
}

ConstraintResult GenericSignatureBuilder::addTypeRequirement(
                             UnresolvedType subject,
                             UnresolvedType constraint,
                             FloatingRequirementSource source,
                             UnresolvedHandlingKind unresolvedHandling) {
  // Resolve the constraint.
  auto resolvedConstraint = resolve(constraint, source);
  if (!resolvedConstraint) {
    return handleUnresolvedRequirement(RequirementKind::Conformance, subject,
                                       toRequirementRHS(constraint), source,
                                       unresolvedHandling);
  }

  // The right-hand side needs to be concrete.
  if (auto constraintPA = resolvedConstraint->getPotentialArchetype()) {
    // The constraint type isn't a statically-known constraint.
    if (source.getLoc().isValid()) {
      auto constraintType =
        constraintPA->getDependentType(Impl->GenericParams,
                                       /*allowUnresolved=*/true);
      Diags.diagnose(source.getLoc(), diag::requires_not_suitable_archetype,
                     1, TypeLoc::withoutLoc(constraintType), 0);
    }

    return ConstraintResult::Concrete;
  }

  // Check whether we have a reasonable constraint type at all.
  auto constraintType = resolvedConstraint->getType();
  assert(constraintType && "Missing constraint type?");
  if (!constraintType->isExistentialType() &&
      !constraintType->getClassOrBoundGenericClass()) {
    if (source.getLoc().isValid() && !constraintType->hasError()) {
      Type subjectType = subject.dyn_cast<Type>();
      if (!subjectType)
        subjectType = subject.get<PotentialArchetype *>()
                        ->getDependentType(Impl->GenericParams,
                                           /*allowUnresolved=*/true);

      Diags.diagnose(source.getLoc(), diag::requires_conformance_nonprotocol,
                     TypeLoc::withoutLoc(subjectType),
                     TypeLoc::withoutLoc(constraintType));
    }

    return ConstraintResult::Conflicting;
  }

  // Resolve the subject. If we can't, delay the constraint.
  auto resolvedSubject = resolve(subject, source);
  if (!resolvedSubject) {
    auto recordedKind =
      constraintType->isExistentialType()
        ? RequirementKind::Conformance
        : RequirementKind::Superclass;
    return handleUnresolvedRequirement(recordedKind, subject, constraintType,
                                       source, unresolvedHandling);
  }

  // If the resolved subject is a type, we can probably perform diagnostics
  // here.
  if (resolvedSubject->isType()) {
    // One cannot explicitly write a constraint on a concrete type.
    if (source.isExplicit()) {
      if (source.getLoc().isValid()) {
        Diags.diagnose(source.getLoc(), diag::requires_not_suitable_archetype,
                       0, TypeLoc::withoutLoc(resolvedSubject->getType()), 0);
      }

      return ConstraintResult::Concrete;
    }

    // FIXME: Check the constraint now.
    return ConstraintResult::Resolved;
  }

  auto subjectPA = resolvedSubject->getPotentialArchetype();
  assert(subjectPA && "No potential archetype?");

  auto resolvedSource = source.getSource(subjectPA);

  // Protocol requirements.
  if (constraintType->isExistentialType()) {
    bool anyErrors = false;
    auto layout = constraintType->getExistentialLayout();

    if (auto layoutConstraint = layout.getLayoutConstraint()) {
      if (isErrorResult(addLayoutRequirementDirect(subjectPA,
                                                   layoutConstraint,
                                                   resolvedSource)))
        anyErrors = true;
    }

    if (layout.superclass) {
      if (isErrorResult(addSuperclassRequirementDirect(subjectPA,
                                                       layout.superclass,
                                                       resolvedSource)))
        anyErrors = true;
    }

    for (auto *proto : layout.getProtocols()) {
      auto *protoDecl = proto->getDecl();
      if (isErrorResult(addConformanceRequirement(subjectPA, protoDecl,
                                                  resolvedSource)))
        anyErrors = true;
    }

    return anyErrors ? ConstraintResult::Conflicting
                     : ConstraintResult::Resolved;
  }

  // Superclass constraint.
  return addSuperclassRequirementDirect(subjectPA, constraintType,
                                        resolvedSource);
}

void GenericSignatureBuilder::PotentialArchetype::addSameTypeConstraint(
                                             PotentialArchetype *otherPA,
                                             const RequirementSource *source) {
  // Update the same-type constraints of this PA to reference the other PA.
  getOrCreateEquivalenceClass()->sameTypeConstraints[this]
    .push_back({this, otherPA, source});

  if (this != otherPA) {
    // Update the same-type constraints of the other PA to reference this PA.
    otherPA->getOrCreateEquivalenceClass()->sameTypeConstraints[otherPA]
      .push_back({otherPA, this, source});
  }
}

ConstraintResult
GenericSignatureBuilder::addSameTypeRequirementBetweenArchetypes(
       PotentialArchetype *OrigT1,
       PotentialArchetype *OrigT2,
       const RequirementSource *Source) 
{
  // Record the same-type constraint.
  OrigT1->addSameTypeConstraint(OrigT2, Source);

  // Operate on the representatives
  auto T1 = OrigT1->getRepresentative();
  auto T2 = OrigT2->getRepresentative();

  // If the representatives are already the same, we're done.
  if (T1 == T2)
    return ConstraintResult::Resolved;

  // Decide which potential archetype is to be considered the representative.
  // It doesn't specifically matter which we use, but it's a minor optimization
  // to prefer the canonical type.
  if (compareDependentTypes(&T2, &T1) < 0) {
    std::swap(T1, T2);
    std::swap(OrigT1, OrigT2);
  }

  // Merge the equivalence classes.
  auto equivClass = T1->getOrCreateEquivalenceClass();
  auto equivClass2Members = T2->getEquivalenceClassMembers();
  for (auto equiv : equivClass2Members)
    equivClass->members.push_back(equiv);

  // Grab the old equivalence class, if present. We'll delete it at the end.
  auto equivClass2 = T2->getEquivalenceClassIfPresent();
  SWIFT_DEFER {
    delete equivClass2;
  };

  // Same-type requirements.
  if (equivClass2) {
    for (auto &paSameTypes : equivClass2->sameTypeConstraints) {
      auto inserted =
        equivClass->sameTypeConstraints.insert(std::move(paSameTypes));
      (void)inserted;
      assert(inserted.second && "equivalence class already has entry for PA?");
    }
  }

  // Same-type-to-concrete requirements.
  if (equivClass2 && equivClass2->concreteType) {
    if (equivClass->concreteType) {
      (void)addSameTypeRequirement(equivClass->concreteType,
                                   equivClass2->concreteType, Source,
                                   UnresolvedHandlingKind::GenerateConstraints,
                                   DiagnoseSameTypeConflict{Diags, Source, T1});
    } else {
      equivClass->concreteType = equivClass2->concreteType;
    }

    equivClass->concreteTypeConstraints.insert(
                                 equivClass->concreteTypeConstraints.end(),
                                 equivClass2->concreteTypeConstraints.begin(),
                                 equivClass2->concreteTypeConstraints.end());
  }

  // Make T1 the representative of T2, merging the equivalence classes.
  T2->representativeOrEquivClass = T1;

  // Superclass requirements.
  if (equivClass2 && equivClass2->superclass) {
    const RequirementSource *source2;
    if (auto existingSource2 =
          equivClass2->findAnySuperclassConstraintAsWritten(OrigT2))
      source2 = existingSource2->source;
    else
      source2 = equivClass2->superclassConstraints.front().source;

    (void)updateSuperclass(T1, equivClass2->superclass, source2);

    equivClass->superclassConstraints.insert(
                                   equivClass->superclassConstraints.end(),
                                   equivClass2->superclassConstraints.begin(),
                                   equivClass2->superclassConstraints.end());
  }

  // Add all of the protocol conformance requirements of T2 to T1.
  if (equivClass2) {
    for (const auto &entry : equivClass2->conformsTo) {
      T1->addConformance(entry.first, entry.second.front().source, *this);

      auto &constraints1 = equivClass->conformsTo[entry.first];
      constraints1.insert(constraints1.end(),
                          entry.second.begin() + 1,
                          entry.second.end());
    }
  }

  // Recursively merge the associated types of T2 into T1.
  auto dependentT1 = T1->getDependentType({ }, /*allowUnresolved=*/true);
  for (auto equivT2 : equivClass2Members) {
    for (auto T2Nested : equivT2->NestedTypes) {
      Type nestedT1 = DependentMemberType::get(dependentT1, T2Nested.first);
      if (isErrorResult(
            addSameTypeRequirement(
               nestedT1, T2Nested.second.front(),
               FloatingRequirementSource::forNestedTypeNameMatch(
                                                      Source, T2Nested.first),
               UnresolvedHandlingKind::GenerateConstraints)))
        return ConstraintResult::Conflicting;
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementToConcrete(
       PotentialArchetype *T,
       Type Concrete,
       const RequirementSource *Source) {
  auto rep = T->getRepresentative();
  auto equivClass = rep->getOrCreateEquivalenceClass();

  // Record the concrete type and its source.
  equivClass->concreteTypeConstraints.push_back(
                                      ConcreteConstraint{T, Concrete, Source});

  // If we've already been bound to a type, match that type.
  if (equivClass->concreteType) {
    return addSameTypeRequirement(equivClass->concreteType, Concrete, Source,
                                  UnresolvedHandlingKind::GenerateConstraints,
                                  DiagnoseSameTypeConflict{ Diags, Source, T});

  }

  // Record the requirement.
  equivClass->concreteType = Concrete;

  // Make sure the concrete type fulfills the requirements on the archetype.
  // FIXME: Move later...
  DenseMap<ProtocolDecl *, ProtocolConformanceRef> conformances;
  CanType depTy = rep->getDependentType({ }, /*allowUnresolved=*/true)
                    ->getCanonicalType();
  for (auto protocol : rep->getConformsTo()) {
    auto conformance =
      getLookupConformanceFn()(depTy, Concrete,
                               protocol->getDeclaredInterfaceType()
                                 ->castTo<ProtocolType>());
    if (!conformance) {
      if (!Concrete->hasError()) {
        Diags.diagnose(Source->getLoc(),
                       diag::requires_generic_param_same_type_does_not_conform,
                       Concrete, protocol->getName());
      }
      return ConstraintResult::Conflicting;
    }

    conformances.insert({protocol, *conformance});

    // Abstract conformances are acceptable for existential types.
    assert(conformance->isConcrete() || Concrete->isExistentialType());

    // Update the requirement source now that we know it's concrete.
    // FIXME: Bad concrete source info.
    auto concreteSource = Source->viaConcrete(*this,
                                              conformance->isConcrete()
                                                ? conformance->getConcrete()
                                                : nullptr);
    equivClass->conformsTo[protocol].push_back({T, protocol, concreteSource});
  }

  // Eagerly resolve any existing nested types to their concrete forms (others
  // will be "concretized" as they are constructed, in getNestedType).
  for (auto equivT : rep->getEquivalenceClassMembers()) {
    for (auto nested : equivT->getNestedTypes()) {
      concretizeNestedTypeFromConcreteParent(
          equivT, Source, nested.second.front(), *this,
          [&](ProtocolDecl *proto) -> ProtocolConformanceRef {
            return conformances.find(proto)->second;
          });
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementBetweenConcrete(
    Type type1, Type type2, FloatingRequirementSource source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  // Local class to handle matching the two sides of the same-type constraint.
  class ReqTypeMatcher : public TypeMatcher<ReqTypeMatcher> {
    GenericSignatureBuilder &builder;
    FloatingRequirementSource source;
    Type outerType1, outerType2;
    llvm::function_ref<void(Type, Type)> diagnoseMismatch;

  public:
    ReqTypeMatcher(GenericSignatureBuilder &builder,
                   FloatingRequirementSource source,
                   Type outerType1, Type outerType2,
                   llvm::function_ref<void(Type, Type)> diagnoseMismatch)
        : builder(builder), source(source), outerType1(outerType1),
          outerType2(outerType2), diagnoseMismatch(diagnoseMismatch) {}

    bool mismatch(TypeBase *firstType, TypeBase *secondType,
                  Type sugaredFirstType) {
      // If the mismatch was in the first layer (i.e. what was fed to
      // addSameTypeRequirementBetweenConcrete), then this is a fundamental
      // mismatch, and we need to diagnose it. This is what breaks the mutual
      // recursion between addSameTypeRequirement and
      // addSameTypeRequirementBetweenConcrete.
      if (outerType1->isEqual(firstType) && outerType2->isEqual(secondType)) {
        diagnoseMismatch(sugaredFirstType, secondType);
        return false;
      }

      auto failed = builder.addSameTypeRequirement(
          sugaredFirstType, Type(secondType), source,
          UnresolvedHandlingKind::GenerateConstraints, diagnoseMismatch);
      return !isErrorResult(failed);
    }
  } matcher(*this, source, type1, type2, diagnoseMismatch);

  return matcher.match(type1, type2) ? ConstraintResult::Resolved
                                     : ConstraintResult::Conflicting;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirement(
                                 UnresolvedType paOrT1,
                                 UnresolvedType paOrT2,
                                 FloatingRequirementSource source,
                                 UnresolvedHandlingKind unresolvedHandling) {
  return addSameTypeRequirement(paOrT1, paOrT2, source, unresolvedHandling,
                                [&](Type type1, Type type2) {
      Diags.diagnose(source.getLoc(), diag::requires_same_concrete_type,
                     type1, type2);
    });
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirement(
    UnresolvedType paOrT1, UnresolvedType paOrT2,
    FloatingRequirementSource source,
    UnresolvedHandlingKind unresolvedHandling,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {

  auto resolved1 = resolve(paOrT1, source);
  if (!resolved1) {
    return handleUnresolvedRequirement(RequirementKind::SameType, paOrT1,
                                       toRequirementRHS(paOrT2), source,
                                       unresolvedHandling);
  }

  auto resolved2 = resolve(paOrT2, source);
  if (!resolved2) {
    return handleUnresolvedRequirement(RequirementKind::SameType, paOrT1,
                                       toRequirementRHS(paOrT2), source,
                                       unresolvedHandling);
  }

  return addSameTypeRequirementDirect(*resolved1, *resolved2, source,
                                      diagnoseMismatch);
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementDirect(
                                           ResolvedType paOrT1,
                                           ResolvedType paOrT2,
                                           FloatingRequirementSource source) {
  return addSameTypeRequirementDirect(paOrT1, paOrT2, source,
                                      [&](Type type1, Type type2) {
    Diags.diagnose(source.getLoc(), diag::requires_same_concrete_type,
                   type1, type2);
  });
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementDirect(
    ResolvedType paOrT1, ResolvedType paOrT2, FloatingRequirementSource source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  auto pa1 = paOrT1.getPotentialArchetype();
  auto pa2 = paOrT2.getPotentialArchetype();
  auto t1 = paOrT1.getType();
  auto t2 = paOrT2.getType();

  // If both sides of the requirement are type parameters, equate them.
  if (pa1 && pa2) {
    return addSameTypeRequirementBetweenArchetypes(pa1, pa2,
                                                   source.getSource(pa1));
    // If just one side is a type parameter, map it to a concrete type.
  } else if (pa1) {
    return addSameTypeRequirementToConcrete(pa1, t2, source.getSource(pa1));
  } else if (pa2) {
    return addSameTypeRequirementToConcrete(pa2, t1, source.getSource(pa2));
  } else {
    return addSameTypeRequirementBetweenConcrete(t1, t2, source,
                                                 diagnoseMismatch);
  }
}

// Local function to mark the given associated type as recursive,
// diagnosing it if this is the first such occurrence.
void GenericSignatureBuilder::markPotentialArchetypeRecursive(
    PotentialArchetype *pa, ProtocolDecl *proto, const RequirementSource *source) {
  if (pa->isRecursive())
    return;
  pa->setIsRecursive();

  pa->addConformance(proto, source, *this);
  if (!pa->getParent())
    return;

  auto assocType = pa->getResolvedAssociatedType();
  if (!assocType || assocType->isInvalid())
    return;

  Diags.diagnose(assocType->getLoc(), diag::recursive_requirement_reference);

  // Silence downstream errors referencing this associated type.
  assocType->setInvalid();
}

ConstraintResult GenericSignatureBuilder::addInheritedRequirements(
                             TypeDecl *decl,
                             UnresolvedType type,
                             const RequirementSource *parentSource,
                             ModuleDecl *inferForModule) {
  if (isa<AssociatedTypeDecl>(decl) &&
      decl->hasInterfaceType() &&
      decl->getInterfaceType()->is<ErrorType>())
    return ConstraintResult::Resolved;

  // Walk the 'inherited' list to identify requirements.
  if (auto resolver = getLazyResolver())
    resolver->resolveInheritanceClause(decl);

  // Local function to get the source.
  auto getFloatingSource = [&](const TypeRepr *typeRepr, bool forInferred) {
    if (parentSource) {
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
        auto proto = assocType->getProtocol();
        return FloatingRequirementSource::viaProtocolRequirement(
          parentSource, proto, typeRepr, forInferred);
      }

      auto proto = cast<ProtocolDecl>(decl);
      return FloatingRequirementSource::viaProtocolRequirement(
        parentSource, proto, typeRepr, forInferred);
    }

    // We are inferring requirements.
    if (forInferred)
      return FloatingRequirementSource::forInferred(typeRepr);

    // Explicit requirement.
    if (typeRepr)
      return FloatingRequirementSource::forExplicit(typeRepr);

    // An abstract explicit requirement.
    return FloatingRequirementSource::forAbstract();
  };

  auto visitType = [&](Type inheritedType, const TypeRepr *typeRepr) {
    if (inferForModule) {
      inferRequirements(*inferForModule,
                        TypeLoc(const_cast<TypeRepr *>(typeRepr),
                                inheritedType),
                        getFloatingSource(typeRepr, /*forInferred=*/true));
    }

    // Check for direct recursion.
    if (auto assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
      auto proto = assocType->getProtocol();
      if (auto inheritedProto = inheritedType->getAs<ProtocolType>()) {
        if (inheritedProto->getDecl() == proto ||
            inheritedProto->getDecl()->inheritsFrom(proto)) {
          auto source = getFloatingSource(typeRepr, /*forInferred=*/false);
          if (auto resolved = resolve(type, source)) {
            if (auto pa = resolved->getPotentialArchetype()) {
              markPotentialArchetypeRecursive(pa, proto, source.getSource(pa));
              return ConstraintResult::Conflicting;
            }
          }
        }
      }
    }

    return addTypeRequirement(type, inheritedType,
                              getFloatingSource(typeRepr,
                                                /*forInferred=*/false),
                              UnresolvedHandlingKind::GenerateConstraints);
  };

  auto visitLayout = [&](LayoutConstraint layout, const TypeRepr *typeRepr) {
    return addLayoutRequirement(type, layout,
                                getFloatingSource(typeRepr,
                                                  /*forInferred=*/false),
                                UnresolvedHandlingKind::GenerateConstraints);
  };

  return visitInherited(decl->getInherited(), visitType, visitLayout);
}

ConstraintResult GenericSignatureBuilder::addRequirement(
                                                 const RequirementRepr *req,
                                                 ModuleDecl *inferForModule) {
  return addRequirement(req,
                        FloatingRequirementSource::forExplicit(req),
                        nullptr,
                        inferForModule);
}

ConstraintResult GenericSignatureBuilder::addRequirement(
                                             const RequirementRepr *Req,
                                             FloatingRequirementSource source,
                                             const SubstitutionMap *subMap,
                                             ModuleDecl *inferForModule) {
  auto subst = [&](Type t) {
    if (subMap)
      return t.subst(*subMap);

    return t;
  };

  auto getInferredTypeLoc = [=](Type type, TypeLoc existingTypeLoc) {
    if (subMap) return TypeLoc::withoutLoc(type);
    return existingTypeLoc;
  };

  switch (Req->getKind()) {
  case RequirementReprKind::LayoutConstraint: {
    auto subject = subst(Req->getSubject());
    if (inferForModule) {
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(subject, Req->getSubjectLoc()),
                        source.asInferred(Req->getSubjectLoc().getTypeRepr()));
    }

    return addLayoutRequirement(subject,
                                Req->getLayoutConstraint(),
                                source,
                                UnresolvedHandlingKind::GenerateConstraints);
  }

  case RequirementReprKind::TypeConstraint: {
    auto subject = subst(Req->getSubject());
    auto constraint = subst(Req->getConstraint());
    if (inferForModule) {
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(subject, Req->getSubjectLoc()),
                        source.asInferred(Req->getSubjectLoc().getTypeRepr()));
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(constraint,
                                           Req->getConstraintLoc()),
                        source.asInferred(
                                      Req->getConstraintLoc().getTypeRepr()));
    }
    return addTypeRequirement(subject, constraint, source,
                              UnresolvedHandlingKind::GenerateConstraints);
  }

  case RequirementReprKind::SameType: {
    // Require that at least one side of the requirement contain a type
    // parameter.
    if (!Req->getFirstType()->hasTypeParameter() &&
        !Req->getSecondType()->hasTypeParameter()) {
      if (!Req->getFirstType()->hasError() &&
          !Req->getSecondType()->hasError()) {
        Diags.diagnose(Req->getEqualLoc(),
                       diag::requires_no_same_type_archetype)
          .highlight(Req->getFirstTypeLoc().getSourceRange())
          .highlight(Req->getSecondTypeLoc().getSourceRange());
      }

      return ConstraintResult::Concrete;
    }

    auto firstType = subst(Req->getFirstType());
    auto secondType = subst(Req->getSecondType());
    if (inferForModule) {
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(firstType, Req->getFirstTypeLoc()),
                        source.asInferred(
                                        Req->getFirstTypeLoc().getTypeRepr()));
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(secondType,
                                           Req->getSecondTypeLoc()),
                        source.asInferred(
                                        Req->getSecondTypeLoc().getTypeRepr()));
    }
    return addRequirement(Requirement(RequirementKind::SameType,
                                      firstType, secondType),
                          source, nullptr);
  }
  }

  llvm_unreachable("Unhandled requirement?");
}

ConstraintResult GenericSignatureBuilder::addRequirement(
                            const Requirement &req,
                            FloatingRequirementSource source,
                            ModuleDecl *inferForModule,
                            const SubstitutionMap *subMap) {
  auto subst = [&](Type t) {
    if (subMap)
      return t.subst(*subMap);

    return t;
  };


  switch (req.getKind()) {
  case RequirementKind::Superclass:
  case RequirementKind::Conformance: {
    auto firstType = subst(req.getFirstType());
    auto secondType = subst(req.getSecondType());
    if (!firstType || !secondType)
      return ConstraintResult::Conflicting;

    if (inferForModule) {
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(firstType),
                        FloatingRequirementSource::forInferred(nullptr));
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(secondType),
                        FloatingRequirementSource::forInferred(nullptr));
    }

    return addTypeRequirement(firstType, secondType, source,
                              UnresolvedHandlingKind::GenerateConstraints);
  }

  case RequirementKind::Layout: {
    auto firstType = subst(req.getFirstType());
    if (!firstType)
      return ConstraintResult::Conflicting;

    if (inferForModule) {
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(firstType),
                        FloatingRequirementSource::forInferred(nullptr));
    }

    return addLayoutRequirement(firstType, req.getLayoutConstraint(), source,
                                UnresolvedHandlingKind::GenerateConstraints);
  }

  case RequirementKind::SameType: {
    auto firstType = subst(req.getFirstType());
    auto secondType = subst(req.getSecondType());
    if (!firstType || !secondType)
      return ConstraintResult::Conflicting;

    if (inferForModule) {
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(firstType),
                        FloatingRequirementSource::forInferred(nullptr));
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(secondType),
                        FloatingRequirementSource::forInferred(nullptr));
    }

    return addSameTypeRequirement(
        firstType, secondType, source,
        UnresolvedHandlingKind::GenerateConstraints,
        [&](Type type1, Type type2) {
          if (source.getLoc().isValid())
            Diags.diagnose(source.getLoc(), diag::requires_same_concrete_type,
                           type1, type2);
        });
  }
  }

  llvm_unreachable("Unhandled requirement?");
}

/// AST walker that infers requirements from type representations.
class GenericSignatureBuilder::InferRequirementsWalker : public TypeWalker {
  ModuleDecl &module;
  GenericSignatureBuilder &Builder;
  FloatingRequirementSource source;

public:
  InferRequirementsWalker(ModuleDecl &module,
                          GenericSignatureBuilder &builder,
                          FloatingRequirementSource source)
    : module(module), Builder(builder), source(source) { }

  Action walkToTypePost(Type ty) override {
    auto boundGeneric = ty->getAs<BoundGenericType>();
    if (!boundGeneric)
      return Action::Continue;

    auto *decl = boundGeneric->getDecl();
    auto genericSig = decl->getGenericSignature();
    if (!genericSig)
      return Action::Stop;

    /// Retrieve the substitution.
    auto subMap = boundGeneric->getContextSubstitutionMap(
      &module, decl, decl->getGenericEnvironment());

    // Handle the requirements.
    // FIXME: Inaccurate TypeReprs.
    for (const auto &req : genericSig->getRequirements()) {
      Builder.addRequirement(req, source, nullptr, &subMap);
    }

    return Action::Continue;
  }
};

void GenericSignatureBuilder::inferRequirements(
                                          ModuleDecl &module,
                                          TypeLoc type,
                                          FloatingRequirementSource source) {
  if (!type.getType())
    return;
  // FIXME: Crummy source-location information.
  InferRequirementsWalker walker(module, *this, source);
  type.getType().walk(walker);
}

void GenericSignatureBuilder::inferRequirements(
                                          ModuleDecl &module,
                                          ParameterList *params,
                                          GenericParamList *genericParams) {
  if (genericParams == nullptr)
    return;

  for (auto P : *params) {
    inferRequirements(module, P->getTypeLoc(),
                      FloatingRequirementSource::forInferred(
                                            P->getTypeLoc().getTypeRepr()));
  }
}

/// Perform typo correction on the given nested type, producing the
/// corrected name (if successful).
static Identifier typoCorrectNestedType(
                    GenericSignatureBuilder::PotentialArchetype *pa) {
  StringRef name = pa->getNestedName().str();

  // Look through all of the associated types of all of the protocols
  // to which the parent conforms.
  llvm::SmallVector<Identifier, 2> bestMatches;
  unsigned bestEditDistance = 0;
  unsigned maxScore = (name.size() + 1) / 3;
  for (auto proto : pa->getParent()->getConformsTo()) {
    for (auto member : getProtocolMembers(proto)) {
      auto assocType = dyn_cast<AssociatedTypeDecl>(member);
      if (!assocType)
        continue;

      unsigned dist = name.edit_distance(assocType->getName().str(),
                                         /*AllowReplacements=*/true,
                                         maxScore);
      assert(dist > 0 && "nested type should have matched associated type");
      if (bestEditDistance == 0 || dist == bestEditDistance) {
        bestEditDistance = dist;
        maxScore = bestEditDistance;
        bestMatches.push_back(assocType->getName());
      } else if (dist < bestEditDistance) {
        bestEditDistance = dist;
        maxScore = bestEditDistance;
        bestMatches.clear();
        bestMatches.push_back(assocType->getName());
      }
    }
  }

  // FIXME: Look through the superclass.

  // If we didn't find any matches at all, fail.
  if (bestMatches.empty())
    return Identifier();

  // Make sure that we didn't find more than one match at the best
  // edit distance.
  for (auto other : llvm::makeArrayRef(bestMatches).slice(1)) {
    if (other != bestMatches.front())
      return Identifier();
  }

  return bestMatches.front();
}

namespace swift {
  template<typename T>
  bool operator<(const Constraint<T> &lhs, const Constraint<T> &rhs) {
    auto lhsPA = lhs.archetype;
    auto rhsPA = rhs.archetype;
    if (int result = compareDependentTypes(&lhsPA, &rhsPA))
      return result < 0;

    if (int result = lhs.source->compare(rhs.source))
      return result < 0;

    return false;
  }

  template<typename T>
  bool operator==(const Constraint<T> &lhs, const Constraint<T> &rhs){
    return lhs.archetype == rhs.archetype &&
           lhs.value == rhs.value &&
           lhs.source == rhs.source;
  }

  template<>
  bool operator==(const Constraint<Type> &lhs, const Constraint<Type> &rhs){
    return lhs.archetype == rhs.archetype &&
           lhs.value->isEqual(rhs.value) &&
           lhs.source == rhs.source;
  }
} // namespace swift

namespace {
  /// Retrieve the representative constraint that will be used for diagnostics.
  template<typename T>
  Optional<Constraint<T>> findRepresentativeConstraint(
                            ArrayRef<Constraint<T>> constraints,
                            llvm::function_ref<bool(const Constraint<T> &)>
                                                   isSuitableRepresentative) {
    // Find a representative constraint.
    Optional<Constraint<T>> representativeConstraint;
    for (const auto &constraint : constraints) {
      // If this isn't a suitable representative constraint, ignore it.
      if (!isSuitableRepresentative(constraint))
        continue;

      // Check whether this constraint is better than the best we've seen so far
      // at being the representative constraint against which others will be
      // compared.
      if (!representativeConstraint) {
        representativeConstraint = constraint;
        continue;
      }

      // We prefer constraints rooted at inferred requirements to ones rooted
      // on explicit requirements, because the former won't be diagnosed
      // directly.
      bool thisIsInferred = constraint.source->isInferredRequirement();
      bool representativeIsInferred = representativeConstraint->source->isInferredRequirement();
      if (thisIsInferred != representativeIsInferred) {
        if (thisIsInferred)
          representativeConstraint = constraint;
        continue;
      }

      // We prefer derived constraints to non-derived constraints.
      bool thisIsDerived = constraint.source->isDerivedRequirement();
      bool representativeIsDerived =
        representativeConstraint->source->isDerivedRequirement();
      if (thisIsDerived != representativeIsDerived) {
        if (thisIsDerived)
          representativeConstraint = constraint;

        continue;
      }

      // We prefer constraints with locations to constraints without locations.
      bool thisHasValidSourceLoc = constraint.source->getLoc().isValid();
      bool representativeHasValidSourceLoc =
        representativeConstraint->source->getLoc().isValid();
      if (thisHasValidSourceLoc != representativeHasValidSourceLoc) {
        if (thisHasValidSourceLoc)
          representativeConstraint = constraint;

        continue;
      }

      // Otherwise, order via the constraint itself.
      if (constraint < *representativeConstraint)
        representativeConstraint = constraint;
    }

    return representativeConstraint;
  }
} // end anonymous namespace

void
GenericSignatureBuilder::finalize(SourceLoc loc,
                           ArrayRef<GenericTypeParamType *> genericParams,
                           bool allowConcreteGenericParams) {
  // Process any delayed requirements that we can handle now.
  processDelayedRequirements();

  assert(!Impl->finalized && "Already finalized builder");
#ifndef NDEBUG
  Impl->finalized = true;
#endif

  // Local function (+ cache) describing the set of potential archetypes
  // directly referenced by the concrete same-type constraint of the given
  // potential archetype. Both the inputs and results are the representatives
  // of their equivalence classes.
  llvm::DenseMap<PotentialArchetype *,
                 SmallPtrSet<PotentialArchetype *, 4>> concretePAs;
  auto getConcreteReferencedPAs
      = [&](PotentialArchetype *pa) -> SmallPtrSet<PotentialArchetype *, 4> {
    assert(pa == pa->getRepresentative() && "Only use with representatives");
    auto known = concretePAs.find(pa);
    if (known != concretePAs.end())
      return known->second;

    SmallPtrSet<PotentialArchetype *, 4> referencedPAs;
    if (!pa->isConcreteType() || !pa->getConcreteType()->hasTypeParameter())
      return referencedPAs;

    if (auto concreteType = pa->getConcreteType()) {
      if (concreteType->hasTypeParameter()) {
        concreteType.visit([&](Type type) {
          if (type->isTypeParameter()) {
            if (auto referencedPA =
                  resolveArchetype(type,
                                   ArchetypeResolutionKind::AlreadyKnown)) {
              referencedPAs.insert(referencedPA->getRepresentative());
            }
          }
        });
      }
    }

    concretePAs[pa] = referencedPAs;
    return referencedPAs;
  };

  /// Check whether the given type references the archetype.
  auto isRecursiveConcreteType = [&](PotentialArchetype *archetype,
                                     bool isSuperclass) {
    SmallPtrSet<PotentialArchetype *, 4> visited;
    SmallVector<PotentialArchetype *, 4> stack;
    stack.push_back(archetype);
    visited.insert(archetype);

    // Check whether the specific type introduces recursion.
    auto checkTypeRecursion = [&](Type type) {
      if (!type->hasTypeParameter()) return false;

      return type.findIf([&](Type type) {
        if (type->isTypeParameter()) {
          if (auto referencedPA =
                resolveArchetype(type, ArchetypeResolutionKind::AlreadyKnown)) {
            referencedPA = referencedPA->getRepresentative();
            if (referencedPA == archetype) return true;

            if (visited.insert(referencedPA).second)
              stack.push_back(referencedPA);
          }
        }

        return false;
      });
    };

    while (!stack.empty()) {
      auto pa = stack.back();
      stack.pop_back();

      // If we're checking superclasses, do so now.
      if (isSuperclass) {
        if (auto superclass = pa->getSuperclass()) {
          if (checkTypeRecursion(superclass)) return true;
        }
      }

      // Otherwise, look for the potential archetypes referenced by
      // same-type constraints.
      for (auto referencedPA : getConcreteReferencedPAs(pa)) {
        // If we found a reference to the original archetype, it's recursive.
        if (referencedPA == archetype) return true;

        if (visited.insert(referencedPA).second)
          stack.push_back(referencedPA);
      }
    }

    return false;
  };

  // Check for recursive or conflicting same-type bindings and superclass
  // constraints.
  visitPotentialArchetypes([&](PotentialArchetype *archetype) {
    if (archetype != archetype->getRepresentative()) return;

    auto equivClass = archetype->getOrCreateEquivalenceClass();
    if (equivClass->concreteType) {
      // Check for recursive same-type bindings.
      if (isRecursiveConcreteType(archetype, /*isSuperclass=*/false)) {
        if (auto constraint =
              equivClass->findAnyConcreteConstraintAsWritten()) {
          Diags.diagnose(constraint->source->getLoc(),
                         diag::recursive_same_type_constraint,
                         archetype->getDependentType(genericParams,
                                                     /*allowUnresolved=*/true),
                         constraint->value);
        }

        archetype->RecursiveConcreteType = true;
      } else {
        checkConcreteTypeConstraints(genericParams, archetype);
      }
    }

    // Check for recursive superclass bindings.
    if (equivClass->superclass) {
      if (isRecursiveConcreteType(archetype, /*isSuperclass=*/true)) {
        if (auto source = equivClass->findAnySuperclassConstraintAsWritten()) {
          Diags.diagnose(source->source->getLoc(),
                         diag::recursive_superclass_constraint,
                         source->archetype->getDependentType(
                                                     genericParams,
                                                     /*allowUnresolved=*/true),
                         equivClass->superclass);
        }

        archetype->RecursiveSuperclassType = true;
      } else {
        checkSuperclassConstraints(genericParams, archetype);
      }
    }

    checkConformanceConstraints(genericParams, archetype);
    checkLayoutConstraints(genericParams, archetype);
    checkSameTypeConstraints(genericParams, archetype);
  });

  // Check for generic parameters which have been made concrete or equated
  // with each other.
  if (!allowConcreteGenericParams) {
    SmallPtrSet<PotentialArchetype *, 4> visited;
    
    unsigned depth = 0;
    for (const auto &gp : Impl->GenericParams)
      depth = std::max(depth, gp->getDepth());

    for (const auto pa : Impl->PotentialArchetypes) {
      auto rep = pa->getRepresentative();

      if (pa->getRootGenericParamKey().Depth < depth)
        continue;

      if (!visited.insert(rep).second)
        continue;

      // Don't allow a generic parameter to be equivalent to a concrete type,
      // because then we don't actually have a parameter.
      auto equivClass = rep->getOrCreateEquivalenceClass();
      if (equivClass->concreteType) {
        if (auto constraint = equivClass->findAnyConcreteConstraintAsWritten())
          Diags.diagnose(constraint->source->getLoc(),
                         diag::requires_generic_param_made_equal_to_concrete,
                         rep->getDependentType(genericParams,
                                               /*allowUnresolved=*/true));
        continue;
      }

      // Don't allow two generic parameters to be equivalent, because then we
      // don't actually have two parameters.
      for (auto other : rep->getEquivalenceClassMembers()) {
        // If it isn't a generic parameter, skip it.
        if (other == pa || !other->isGenericParam()) continue;

        // Try to find an exact constraint that matches 'other'.
        auto repConstraint =
          findRepresentativeConstraint<PotentialArchetype *>(
            pa->getSameTypeConstraints(),
            [other](const Constraint<PotentialArchetype *> &constraint) {
              return constraint.value == other;
            });


         // Otherwise, just take any old constraint.
        if (!repConstraint) {
          repConstraint =
            findRepresentativeConstraint<PotentialArchetype *>(
              pa->getSameTypeConstraints(),
              [other](const Constraint<PotentialArchetype *> &constraint) {
                return true;
              });
        }

        if (repConstraint && repConstraint->source->getLoc().isValid()) {
          Diags.diagnose(repConstraint->source->getLoc(),
                         diag::requires_generic_params_made_equal,
                         pa->getDependentType(genericParams, true),
                         other->getDependentType(genericParams, true));
        }
        break;
      }
    }
  }

  // If any nested types remain unresolved, produce diagnostics.
  if (Impl->NumUnresolvedNestedTypes > 0) {
    visitPotentialArchetypes([&](PotentialArchetype *pa) {
      // We only care about nested types that haven't been resolved.
      if (pa->getParent() == nullptr || pa->getResolvedAssociatedType() ||
          pa->getTypeAliasDecl() ||
          /* FIXME: Should be able to handle this earlier */pa->getSuperclass())
        return;

      // Try to typo correct to a nested type name.
      Identifier correction = typoCorrectNestedType(pa);
      if (correction.empty()) {
        pa->setInvalid();
        return;
      }

      // Note that this is being renamed.
      pa->saveNameForRenaming();
      Impl->RenamedNestedTypes.push_back(pa);
      
      // Resolve the associated type and merge the potential archetypes.
      auto replacement = pa->getParent()->getNestedType(correction, *this);
      pa->resolveAssociatedType(replacement->getResolvedAssociatedType(),
                                *this);
      addSameTypeRequirement(pa, replacement,
                             RequirementSource::forNestedTypeNameMatch(pa),
                             UnresolvedHandlingKind::GenerateConstraints);
    });
  }
}

bool GenericSignatureBuilder::diagnoseRemainingRenames(
                              SourceLoc loc,
                              ArrayRef<GenericTypeParamType *> genericParams) {
  bool invalid = false;

  for (auto pa : Impl->RenamedNestedTypes) {
    if (pa->alreadyDiagnosedRename()) continue;

    Diags.diagnose(loc, diag::invalid_member_type_suggest,
                   pa->getParent()->getDependentType(genericParams,
                                                     /*allowUnresolved=*/true),
                   pa->getOriginalName(), pa->getNestedName());
    invalid = true;
  }

  return invalid;
}

/// Turn a requirement right-hand side into an unresolved type.
static GenericSignatureBuilder::UnresolvedType asUnresolvedType(
                                GenericSignatureBuilder::RequirementRHS rhs) {
  if (auto pa = rhs.dyn_cast<PotentialArchetype *>())
    return GenericSignatureBuilder::UnresolvedType(pa);

  return GenericSignatureBuilder::UnresolvedType(rhs.get<Type>());
}

void GenericSignatureBuilder::processDelayedRequirements() {
  bool anySolved = !Impl->DelayedRequirements.empty();
  while (anySolved) {
    // Steal the delayed requirements so we can reprocess them.
    anySolved = false;
    auto delayed = std::move(Impl->DelayedRequirements);
    Impl->DelayedRequirements.clear();

    // Process delayed requirements.
    for (const auto &req : delayed) {
      // Reprocess the delayed requirement.
      ConstraintResult reqResult;
      switch (req.kind) {
      case RequirementKind::Conformance:
      case RequirementKind::Superclass:
        reqResult = addTypeRequirement(
                       req.lhs, asUnresolvedType(req.rhs), req.source,
                       UnresolvedHandlingKind::ReturnUnresolved);
        break;

      case RequirementKind::Layout:
        reqResult = addLayoutRequirement(
                           req.lhs, req.rhs.get<LayoutConstraint>(), req.source,
                           UnresolvedHandlingKind::ReturnUnresolved);
        break;

      case RequirementKind::SameType:
        reqResult = addSameTypeRequirement(
                               req.lhs, asUnresolvedType(req.rhs), req.source,
                               UnresolvedHandlingKind::ReturnUnresolved);
        break;
      }

      // Update our state based on what happened.
      switch (reqResult) {
      case ConstraintResult::Concrete:
      case ConstraintResult::Conflicting:
        anySolved = true;
        break;

      case ConstraintResult::Resolved:
        anySolved = true;
        break;

      case ConstraintResult::Unresolved:
        // Add the requirement back.
        Impl->DelayedRequirements.push_back(req);
        break;
      }
    }
  }
}

template<typename T>
Constraint<T> GenericSignatureBuilder::checkConstraintList(
                           ArrayRef<GenericTypeParamType *> genericParams,
                           std::vector<Constraint<T>> &constraints,
                           llvm::function_ref<bool(const Constraint<T> &)>
                             isSuitableRepresentative,
                           llvm::function_ref<ConstraintRelation(const T&)>
                             checkConstraint,
                           Optional<Diag<unsigned, Type, T, T>>
                             conflictingDiag,
                           Diag<Type, T> redundancyDiag,
                           Diag<unsigned, Type, T> otherNoteDiag) {
  return checkConstraintList<T, T>(genericParams, constraints,
                                   isSuitableRepresentative, checkConstraint,
                                   conflictingDiag, redundancyDiag,
                                   otherNoteDiag,
                                   [](const T& value) { return value; },
                                   /*removeSelfDerived=*/true);
}

namespace {
  /// Remove self-derived sources from the given vector of constraints.
  ///
  /// \returns true if any derived-via-concrete constraints were found.
  template<typename T>
  bool removeSelfDerived(std::vector<Constraint<T>> &constraints,
                         bool dropDerivedViaConcrete = true) {
    bool anyDerivedViaConcrete = false;
    // Remove self-derived constraints.
    Optional<Constraint<T>> remainingConcrete;
    constraints.erase(
      std::remove_if(constraints.begin(), constraints.end(),
                     [&](const Constraint<T> &constraint) {
                       bool derivedViaConcrete;
                       if (constraint.source->isSelfDerivedSource(
                                constraint.archetype, derivedViaConcrete))
                         return true;

                       if (!derivedViaConcrete)
                         return false;

                       anyDerivedViaConcrete = true;

                       if (!dropDerivedViaConcrete)
                         return false;

                       // Drop derived-via-concrete requirements.
                       if (!remainingConcrete)
                         remainingConcrete = constraint;

                       return true;
                     }),
      constraints.end());

    if (constraints.empty() && remainingConcrete)
      constraints.push_back(*remainingConcrete);

    assert(!constraints.empty() && "All constraints were self-derived!");
    return anyDerivedViaConcrete;
  }
} // end anonymous namespace

template<typename T, typename DiagT>
Constraint<T> GenericSignatureBuilder::checkConstraintList(
                           ArrayRef<GenericTypeParamType *> genericParams,
                           std::vector<Constraint<T>> &constraints,
                           llvm::function_ref<bool(const Constraint<T> &)>
                             isSuitableRepresentative,
                           llvm::function_ref<ConstraintRelation(const T&)>
                             checkConstraint,
                           Optional<Diag<unsigned, Type, DiagT, DiagT>>
                             conflictingDiag,
                           Diag<Type, DiagT> redundancyDiag,
                           Diag<unsigned, Type, DiagT> otherNoteDiag,
                           llvm::function_ref<DiagT(const T&)> diagValue,
                           bool removeSelfDerived) {
  assert(!constraints.empty() && "No constraints?");
  if (removeSelfDerived) {
    ::removeSelfDerived(constraints);
  }

  // Sort the constraints, so we get a deterministic ordering of diagnostics.
  llvm::array_pod_sort(constraints.begin(), constraints.end());

  // Find a representative constraint.
  auto representativeConstraint =
    findRepresentativeConstraint<T>(constraints, isSuitableRepresentative);

  // Local function to provide a note describing the representative constraint.
  auto noteRepresentativeConstraint = [&] {
    if (representativeConstraint->source->getLoc().isInvalid()) return;

    Diags.diagnose(representativeConstraint->source->getLoc(),
                   otherNoteDiag,
                   representativeConstraint->source->classifyDiagKind(),
                   representativeConstraint->archetype->
                     getDependentType(genericParams, /*allowUnresolved=*/true),
                   diagValue(representativeConstraint->value));
  };

  // Go through the concrete constraints looking for redundancies.
  bool diagnosedConflictingRepresentative = false;
  for (const auto &constraint : constraints) {
    // Leave the representative alone.
    if (constraint == *representativeConstraint) continue;

    switch (checkConstraint(constraint.value)) {
    case ConstraintRelation::Unrelated:
      continue;

    case ConstraintRelation::Conflicting: {
      // Figure out what kind of subject we have; it will affect the
      // diagnostic.
      auto getSubjectType =
        [&](PotentialArchetype *pa) -> std::pair<unsigned, Type> {
          auto subjectType = pa->getDependentType(genericParams, true);
          unsigned kind;
          if (auto gp = subjectType->getAs<GenericTypeParamType>()) {
            if (gp->getDecl() &&
                isa<ProtocolDecl>(gp->getDecl()->getDeclContext())) {
              kind = 1;
              subjectType = cast<ProtocolDecl>(gp->getDecl()->getDeclContext())
                              ->getDeclaredInterfaceType();
            } else {
              kind = 0;
            }
          } else {
            kind = 2;
          }

          return std::make_pair(kind, subjectType);
        };


      // The requirement conflicts. If this constraint has a location, complain
      // about it.
      if (constraint.source->getLoc().isValid()) {
        auto subject = getSubjectType(constraint.archetype);
        Diags.diagnose(constraint.source->getLoc(), *conflictingDiag,
                       subject.first, subject.second,
                       diagValue(constraint.value),
                       diagValue(representativeConstraint->value));

        noteRepresentativeConstraint();
        break;
      }

      // If the representative itself conflicts and we haven't diagnosed it yet,
      // do so now.
      if (!diagnosedConflictingRepresentative &&
          representativeConstraint->source->getLoc().isValid()) {
        auto subject = getSubjectType(representativeConstraint->archetype);
        Diags.diagnose(representativeConstraint->source->getLoc(),
                       *conflictingDiag,
                       subject.first, subject.second,
                       diagValue(representativeConstraint->value),
                       diagValue(constraint.value));

        diagnosedConflictingRepresentative = true;
        break;
      }
      break;
    }

    case ConstraintRelation::Redundant:
      // If this requirement is not derived or inferred (but has a useful
      // location) complain that it is redundant.
      if (!constraint.source->isDerivedRequirement() &&
          !constraint.source->isInferredRequirement() &&
          constraint.source->getLoc().isValid()) {
        Diags.diagnose(constraint.source->getLoc(),
                       redundancyDiag,
                       constraint.archetype->getDependentType(
                         genericParams, /*allowUnresolved=*/true),
                       diagValue(constraint.value));

        noteRepresentativeConstraint();
      }
      break;
    }
  }

  return *representativeConstraint;
}

void GenericSignatureBuilder::checkConformanceConstraints(
                          ArrayRef<GenericTypeParamType *> genericParams,
                          PotentialArchetype *pa) {
  auto equivClass = pa->getEquivalenceClassIfPresent();
  if (!equivClass || equivClass->conformsTo.empty())
    return;

  for (auto &entry : equivClass->conformsTo) {
    // Remove self-derived constraints.
    assert(!entry.second.empty() && "No constraints to work with?");
    Optional<Constraint<ProtocolDecl *>> remainingConcrete;
    entry.second.erase(
      std::remove_if(entry.second.begin(), entry.second.end(),
                     [&](const Constraint<ProtocolDecl *> &constraint) {
                       bool derivedViaConcrete;
                       if (constraint.source->isSelfDerivedConformance(
                                constraint.archetype, entry.first,
                                derivedViaConcrete))
                         return true;

                       if (!derivedViaConcrete)
                         return false;

                       // Drop derived-via-concrete requirements.
                       if (!remainingConcrete)
                         remainingConcrete = constraint;
                       return true;
                     }),
      entry.second.end());

    // If we only had concrete conformances, put one back.
    if (entry.second.empty() && remainingConcrete)
      entry.second.push_back(*remainingConcrete);

    assert(!entry.second.empty() && "All constraints were self-derived!");

    checkConstraintList<ProtocolDecl *, ProtocolDecl *>(
      genericParams, entry.second,
      [](const Constraint<ProtocolDecl *> &constraint) {
        return true;
      },
      [&](ProtocolDecl *proto) {
        assert(proto == entry.first && "Mixed up protocol constraints");
        return ConstraintRelation::Redundant;
      },
      None,
      diag::redundant_conformance_constraint,
      diag::redundant_conformance_here,
      [](ProtocolDecl *proto) { return proto; },
      /*removeSelfDerived=*/false);
  }
}

/// Perform a depth-first search from the given potential archetype through
/// the *implicit* same-type constraints.
///
/// \param pa The potential archetype to visit.
/// \param paToComponent A mapping from each potential archetype to its
/// component number.
/// \param component The component number we're currently visiting.
///
/// \returns the best archetype anchor seen so far.
static PotentialArchetype *sameTypeDFS(PotentialArchetype *pa,
                        unsigned component,
                        llvm::SmallDenseMap<PotentialArchetype *, unsigned>
                          &paToComponent) {
  PotentialArchetype *anchor = pa;

  // If we've already visited this potential archetype, we're done.
  if (!paToComponent.insert({pa, component}).second) return anchor;

  // Visit its adjacent potential archetypes.
  for (const auto &constraint : pa->getSameTypeConstraints()) {
    // Skip non-derived constraints.
    if (!constraint.source->isDerivedRequirement()) continue;

    auto newAnchor = sameTypeDFS(constraint.value, component, paToComponent);

    // If this type is better than the anchor, use it for the anchor.
    if (compareDependentTypes(&newAnchor, &anchor) < 0)
      anchor = newAnchor;
  }

  return anchor;
}

namespace swift {
  bool operator<(const DerivedSameTypeComponent &lhs,
                 const DerivedSameTypeComponent &rhs) {
    return compareDependentTypes(&lhs.anchor, &rhs.anchor) < 0;
  }
} // namespace swift

/// Retrieve the "local" archetype anchor for the given potential archetype,
/// which rebuilds this potential archetype using the archetype anchors of
/// the parent types.
static PotentialArchetype *getLocalAnchor(PotentialArchetype *pa,
                                          GenericSignatureBuilder &builder) {
  auto parent = pa->getParent();
  if (!parent) return pa;

  auto parentAnchor = getLocalAnchor(parent, builder);
  return parentAnchor->getNestedArchetypeAnchor(pa->getNestedName(), builder);
}

/// Computes the ordered set of archetype anchors required to form a minimum
/// spanning tree among the connected components formed by only the derived
/// same-type requirements within the equivalence class of \c rep.
///
/// The equivalence class of the given representative potential archetype
/// (\c rep) contains all potential archetypes that are made equivalent by
/// the known set of same-type constraints, which includes both directly-
/// stated same-type constraints (e.g., \c T.A == T.B) as well as same-type
/// constraints that are implied either because the names coincide (e.g.,
/// \c T[.P1].A == T[.P2].A) or due to a requirement in a protocol.
///
/// The equivalence class of the given representative potential archetype
/// (\c rep) is formed from a graph whose vertices are the potential archetypes
/// and whose edges are the same-type constraints. These edges include both
/// directly-stated same-type constraints (e.g., \c T.A == T.B) as well as
/// same-type constraints that are implied either because the names coincide
/// (e.g., \c T[.P1].A == T[.P2].A) or due to a requirement in a protocol.
/// The equivalence class forms a single connected component.
///
/// Within that graph is a subgraph that includes only those edges that are
/// implied (and, therefore, excluding those edges that were explicitly stated).
/// The connected components within that subgraph describe the potential
/// archetypes that would be equivalence even with all of the (explicit)
/// same-type constraints removed.
///
/// The entire equivalence class can be restored by introducing edges between
/// the connected components. This function computes a minimal, canonicalized
/// set of edges (same-type constraints) needed to describe the equivalence
/// class, which is suitable for the generation of the canonical generic
/// signature.
///
/// The resulting set of "edges" is returned as a set of vertices, one per
/// connected component (of the subgraph). Each is the anchor for that
/// connected component (as determined by \c compareDependentTypes()), and the
/// set itself is ordered by \c compareDependentTypes(). The actual set of
/// canonical edges connects vertex i to vertex i+1 for i in 0..<size-1.
static void computeDerivedSameTypeComponents(
              PotentialArchetype *rep,
              llvm::SmallDenseMap<PotentialArchetype *, unsigned> &componentOf){
  // Perform a depth-first search to identify the components.
  auto equivClass = rep->getOrCreateEquivalenceClass();
  auto &components = equivClass->derivedSameTypeComponents;
  for (auto pa : rep->getEquivalenceClassMembers()) {
    // If we've already seen this potential archetype, there's nothing else to
    // do.
    if (componentOf.count(pa) != 0) continue;

    // Find all of the potential archetypes within this connected component.
    auto anchor = sameTypeDFS(pa, components.size(), componentOf);

    // Record the anchor.
    components.push_back({anchor, nullptr});
  }

  // If there is a concrete type, figure out the best concrete type anchor
  // per component.
  for (const auto &concrete : equivClass->concreteTypeConstraints) {
    // Dig out the component associated with constraint.
    assert(componentOf.count(concrete.archetype) > 0);
    auto &component = components[componentOf[concrete.archetype]];

    // FIXME: Skip self-derived sources. This means our attempts to "stage"
    // construction of self-derived sources really don't work, because we
    // discover more information later, so we need a more on-line or
    // iterative approach.
    bool derivedViaConcrete;
    if (concrete.source->isSelfDerivedSource(concrete.archetype,
                                             derivedViaConcrete))
      continue;

    // If it has a better source than we'd seen before for this component,
    // keep it.
    auto &bestConcreteTypeSource = component.concreteTypeSource;
    if (!bestConcreteTypeSource ||
        concrete.source->compare(bestConcreteTypeSource) < 0)
      bestConcreteTypeSource = concrete.source;
  }

  // Sort the components.
  llvm::array_pod_sort(components.begin(), components.end());
}

namespace {
  /// An edge in the same-type constraint graph that spans two different
  /// components.
  struct IntercomponentEdge {
    unsigned source;
    unsigned target;
    Constraint<PotentialArchetype *> constraint;

    IntercomponentEdge(unsigned source, unsigned target,
                       const Constraint<PotentialArchetype *> &constraint)
      : source(source), target(target), constraint(constraint)
    {
      assert(source != target && "Not an intercomponent edge");
      if (this->source > this->target) std::swap(this->source, this->target);
    }

    friend bool operator<(const IntercomponentEdge &lhs,
                          const IntercomponentEdge &rhs) {
      if (lhs.source != rhs.source)
        return lhs.source < rhs.source;
      if (lhs.target != rhs.target)
        return lhs.target < rhs.target;

      // Prefer non-inferred requirement sources.
      bool lhsIsInferred = lhs.constraint.source->isInferredRequirement();
      bool rhsIsInferred = rhs.constraint.source->isInferredRequirement();
      if (lhsIsInferred != rhsIsInferred)
        return rhsIsInferred;;

      return lhs.constraint < rhs.constraint;
    }
  };
} // end anonymous namespace

void GenericSignatureBuilder::checkSameTypeConstraints(
                          ArrayRef<GenericTypeParamType *> genericParams,
                          PotentialArchetype *pa) {
  auto equivClass = pa->getEquivalenceClassIfPresent();
  if (!equivClass || !equivClass->derivedSameTypeComponents.empty())
    return;

  // Make sure that we've build the archetype anchors for each potential
  // archetype in this equivalence class. This is important to do for *all*
  // potential archetypes because some non-archetype anchors will nonetheless
  // be used in the canonicalized requirements.
  for (auto pa : pa->getEquivalenceClassMembers()) {
    (void)getLocalAnchor(pa, *this);
  }
  equivClass = pa->getEquivalenceClassIfPresent();
  assert(equivClass && "Equivalence class disappeared?");

  bool anyDerivedViaConcrete = false;
  for (auto &entry : equivClass->sameTypeConstraints) {
    auto &constraints = entry.second;

    // Remove self-derived constraints.
    if (removeSelfDerived(constraints, /*dropDerivedViaConcrete=*/false))
      anyDerivedViaConcrete = true;

    // Sort the constraints, so we get a deterministic ordering of diagnostics.
    llvm::array_pod_sort(constraints.begin(), constraints.end());
  }

  // Compute the components in the subgraph of the same-type constraint graph
  // that includes only derived constraints.
  llvm::SmallDenseMap<PotentialArchetype *, unsigned> componentOf;
  computeDerivedSameTypeComponents(pa, componentOf);

  // Go through all of the same-type constraints, collecting all of the
  // non-derived constraints to put them into bins: intra-component and
  // inter-component.

  // Intra-component edges are stored per-component, so we can perform
  // diagnostics within each component.
  unsigned numComponents = equivClass->derivedSameTypeComponents.size();
  std::vector<std::vector<Constraint<PotentialArchetype *>>>
    intracomponentEdges(numComponents,
                        std::vector<Constraint<PotentialArchetype *>>());

  // Intercomponent edges are stored as one big list, which tracks the
  // source/target components.
  std::vector<IntercomponentEdge> intercomponentEdges;
  for (auto &entry : equivClass->sameTypeConstraints) {
    auto &constraints = entry.second;
    for (const auto &constraint : constraints) {
      // If the source/destination are identical, complain.
      if (constraint.archetype == constraint.value) {
        if (!constraint.source->isDerivedRequirement() &&
            !constraint.source->isInferredRequirement() &&
            constraint.source->getLoc().isValid()) {
          Diags.diagnose(constraint.source->getLoc(),
                         diag::redundant_same_type_constraint,
                         constraint.archetype->getDependentType(
                                                          genericParams, true),
                         constraint.value->getDependentType(
                                                          genericParams, true));
        }

        continue;
      }

      // Only keep constraints where the source is "first" in the ordering;
      // this lets us eliminate the duplication coming from us adding back
      // edges.
      // FIXME: Alternatively, we could track back edges differently in the
      // constraint.
      if (compareDependentTypes(&constraint.archetype, &constraint.value) > 0)
        continue;

      // Determine which component each of the source/destination fall into.
      assert(componentOf.count(constraint.archetype) > 0 &&
             "unknown potential archetype?");
      unsigned firstComponent = componentOf[constraint.archetype];
      assert(componentOf.count(constraint.value) > 0 &&
             "unknown potential archetype?");
      unsigned secondComponent = componentOf[constraint.value];

      // If both vertices are within the same component, this is an
      // intra-component edge. Record it as such.
      if (firstComponent == secondComponent) {
        intracomponentEdges[firstComponent].push_back(constraint);
        continue;
      }

      // Otherwise, it's an intercomponent edge, which is never derived.
      assert(!constraint.source->isDerivedRequirement() &&
             "Must not be derived");

      // Ignore inferred requirements; we don't want to diagnose them.
      intercomponentEdges.push_back(
        IntercomponentEdge(firstComponent, secondComponent, constraint));
    }
  }

  // If there were any derived-via-concrete constraints, drop them now before
  // we emit other diagnostics.
  if (anyDerivedViaConcrete) {
    for (auto &entry : equivClass->sameTypeConstraints) {
      auto &constraints = entry.second;

      // Remove derived-via-concrete constraints.
      (void)removeSelfDerived(constraints);
        anyDerivedViaConcrete = true;
    }
  }

  // Walk through each of the components, checking the intracomponent edges.
  // This will diagnose any explicitly-specified requirements within a
  // component, all of which are redundant.
  for (auto &constraints : intracomponentEdges) {
    if (constraints.empty()) continue;

    checkConstraintList<PotentialArchetype *, Type>(
      genericParams, constraints,
      [](const Constraint<PotentialArchetype *> &) { return true; },
      [](PotentialArchetype *) {
        return ConstraintRelation::Redundant;
      },
      None,
      diag::redundant_same_type_constraint,
      diag::previous_same_type_constraint,
      [&](PotentialArchetype *pa) {
        return pa->getDependentType(genericParams, true);
      },
      /*removeSelfDerived=*/false);
  }

  // Diagnose redundant same-type constraints across components. First,
  // sort the edges so that edges that between the same component pairs
  // occur next to each other.
  llvm::array_pod_sort(intercomponentEdges.begin(), intercomponentEdges.end());

  // Diagnose and erase any redundant edges between the same two components.
  intercomponentEdges.erase(
    std::unique(
      intercomponentEdges.begin(), intercomponentEdges.end(),
      [&](const IntercomponentEdge &lhs,
          const IntercomponentEdge &rhs) {
        // If either the source or target is different, we have
        // different elements.
        if (lhs.source != rhs.source || lhs.target != rhs.target)
          return false;

        // We have two edges connected the same components. If both
        // have locations, diagnose them.
        if (lhs.constraint.source->getLoc().isInvalid() ||
            rhs.constraint.source->getLoc().isInvalid())
          return true;

        // If the constraint source is inferred, don't diagnose it.
        if (lhs.constraint.source->isInferredRequirement())
          return true;

        Diags.diagnose(lhs.constraint.source->getLoc(),
                       diag::redundant_same_type_constraint,
                       lhs.constraint.archetype->getDependentType(
                                                          genericParams, true),
                       lhs.constraint.value->getDependentType(
                                                          genericParams, true));
        Diags.diagnose(rhs.constraint.source->getLoc(),
                       diag::previous_same_type_constraint,
                       rhs.constraint.source->classifyDiagKind(),
                       rhs.constraint.archetype->getDependentType(
                                                          genericParams, true),
                       rhs.constraint.value->getDependentType(
                                                          genericParams, true));
        return true;
      }),
    intercomponentEdges.end());

  // If we have more intercomponent edges than are needed to form a spanning
  // tree, complain about redundancies. Note that the edges we have must
  // connect all of the components, or else we wouldn't have an equivalence
  // class.
  if (intercomponentEdges.size() > numComponents - 1) {
    std::vector<bool> connected(numComponents, false);
    const auto &firstEdge = intercomponentEdges.front();
    for (const auto &edge : intercomponentEdges) {
      // If both the source and target are already connected, this edge is
      // not part of the spanning tree.
      if (connected[edge.source] && connected[edge.target]) {
        if (edge.constraint.source->getLoc().isValid() &&
            !edge.constraint.source->isInferredRequirement() &&
            firstEdge.constraint.source->getLoc().isValid()) {
          Diags.diagnose(edge.constraint.source->getLoc(),
                         diag::redundant_same_type_constraint,
                         edge.constraint.archetype->getDependentType(
                                                          genericParams, true),
                         edge.constraint.value->getDependentType(
                                                          genericParams, true));

          Diags.diagnose(firstEdge.constraint.source->getLoc(),
                         diag::previous_same_type_constraint,
                         firstEdge.constraint.source->classifyDiagKind(),
                         firstEdge.constraint.archetype->getDependentType(
                                                          genericParams, true),
                         firstEdge.constraint.value->getDependentType(
                                                          genericParams, true));
        }

        continue;
      }

      // Put the source and target into the spanning tree.
      connected[edge.source] = true;
      connected[edge.target] = true;
    }
  }
}

void GenericSignatureBuilder::checkConcreteTypeConstraints(
                                 ArrayRef<GenericTypeParamType *> genericParams,
                                 PotentialArchetype *representative) {
  auto equivClass = representative->getOrCreateEquivalenceClass();
  assert(equivClass->concreteType && "No concrete type to check");

  checkConstraintList<Type>(
    genericParams, equivClass->concreteTypeConstraints,
    [](const ConcreteConstraint &constraint) {
      return true;
    },
    [&](Type concreteType) {
      // If the concrete type is equivalent, the constraint is redundant.
      // FIXME: Should check this constraint after substituting in the
      // archetype anchors for each dependent type.
      if (concreteType->isEqual(equivClass->concreteType))
        return ConstraintRelation::Redundant;

      // Call this unrelated.
      return ConstraintRelation::Unrelated;
    },
    None,
    diag::redundant_same_type_to_concrete,
    diag::same_type_redundancy_here);
}

void GenericSignatureBuilder::checkSuperclassConstraints(
                                 ArrayRef<GenericTypeParamType *> genericParams,
                                 PotentialArchetype *representative) {
  auto equivClass = representative->getOrCreateEquivalenceClass();
  assert(equivClass->superclass && "No superclass constraint?");

  // FIXME: We should be substituting in the canonical type in context so
  // we can resolve superclass requirements, e.g., if you had:
  //
  //   class Foo<T>
  //   class Bar: Foo<Int>
  //
  //   func foo<T, U where U: Bar, U: Foo<T>>(...) { ... }
  //
  // then the second `U: Foo<T>` constraint introduces a `T == Int`
  // constraint, and we will need to perform that substitution for this final
  // check.

  auto representativeConstraint =
    checkConstraintList<Type>(
      genericParams, equivClass->superclassConstraints,
      [&](const ConcreteConstraint &constraint) {
        return constraint.value->isEqual(equivClass->superclass);
      },
      [&](Type superclass) {
        // If this class is a superclass of the "best"
        if (superclass->isExactSuperclassOf(equivClass->superclass))
          return ConstraintRelation::Redundant;

        // Otherwise, it conflicts.
        return ConstraintRelation::Conflicting;
      },
      diag::requires_superclass_conflict,
      diag::redundant_superclass_constraint,
      diag::superclass_redundancy_here);

  // If we have a concrete type, check it.
  // FIXME: Substitute into the concrete type.
  if (equivClass->concreteType) {
    // Make sure the concrete type fulfills the superclass requirement.
    if (!equivClass->superclass->isExactSuperclassOf(equivClass->concreteType)) {
      if (auto existing = equivClass->findAnyConcreteConstraintAsWritten(
                            representativeConstraint.archetype)) {
        Diags.diagnose(existing->source->getLoc(), diag::type_does_not_inherit,
                       existing->archetype->getDependentType(
                                                   genericParams,
                                                   /*allowUnresolved=*/true),
                       existing->value, equivClass->superclass);

        // FIXME: Note the representative constraint.
      } else if (representativeConstraint.source->getLoc().isValid()) {
        Diags.diagnose(representativeConstraint.source->getLoc(),
                       diag::type_does_not_inherit,
                       representativeConstraint.archetype->getDependentType(
                                                    genericParams,
                                                    /*allowUnresolved=*/true),
                       equivClass->concreteType, equivClass->superclass);
      }
    } else if (representativeConstraint.source->getLoc().isValid()) {
      // It does fulfill the requirement; diagnose the redundancy.
      Diags.diagnose(representativeConstraint.source->getLoc(),
                     diag::redundant_superclass_constraint,
                     representativeConstraint.archetype->getDependentType(
                                                  genericParams,
                                                  /*allowUnresolved=*/true),
                     representativeConstraint.value);

      if (auto existing = equivClass->findAnyConcreteConstraintAsWritten(
                            representativeConstraint.archetype)) {
        Diags.diagnose(existing->source->getLoc(),
                       diag::same_type_redundancy_here,
                       existing->source->classifyDiagKind(),
                       existing->archetype->getDependentType(
                                                   genericParams,
                                                   /*allowUnresolved=*/true),
                       existing->value);
      }
    }
  }
}

void GenericSignatureBuilder::checkLayoutConstraints(
                                ArrayRef<GenericTypeParamType *> genericParams,
                                PotentialArchetype *pa) {
  auto equivClass = pa->getEquivalenceClassIfPresent();
  if (!equivClass || !equivClass->layout) return;

  checkConstraintList<LayoutConstraint>(
    genericParams, equivClass->layoutConstraints,
    [&](const Constraint<LayoutConstraint> &constraint) {
      return constraint.value == equivClass->layout;
    },
    [&](LayoutConstraint layout) {
      // If the layout constraints are mergable, i.e. compatible,
      // it is a redundancy.
      if (layout.merge(equivClass->layout)->isKnownLayout())
        return ConstraintRelation::Redundant;

      return ConstraintRelation::Conflicting;
    },
    diag::conflicting_layout_constraints,
    diag::redundant_layout_constraint,
    diag::previous_layout_constraint);
}

template<typename F>
void GenericSignatureBuilder::visitPotentialArchetypes(F f) {
  // Stack containing all of the potential archetypes to visit.
  SmallVector<PotentialArchetype *, 4> stack;
  llvm::SmallPtrSet<PotentialArchetype *, 4> visited;

  // Add top-level potential archetypes to the stack.
  for (const auto pa : Impl->PotentialArchetypes) {
    if (visited.insert(pa).second)
      stack.push_back(pa);
  }

  // Visit all of the potential archetypes.
  while (!stack.empty()) {
    PotentialArchetype *pa = stack.back();
    stack.pop_back();
    f(pa);

    // Visit the archetype anchor.
    if (auto anchor = pa->getArchetypeAnchor(*this)) {
      if (visited.insert(anchor).second) {
        stack.push_back(anchor);
      }
    }

    // Visit everything else in this equivalence class.
    for (auto equivPA : pa->getEquivalenceClassMembers()) {
      if (visited.insert(equivPA).second) {
        stack.push_back(equivPA);
      }
    }

    // Visit nested potential archetypes.
    for (const auto &nested : pa->getNestedTypes()) {
      for (auto nestedPA : nested.second) {
        if (visited.insert(nestedPA).second) {
          stack.push_back(nestedPA);
        }
      }
    }
  }
}

namespace {
  /// Retrieve the best requirement source from a set of constraints.
  template<typename T>
  const RequirementSource *getBestConstraintSource(
                                        ArrayRef<Constraint<T>> constraints) {
    auto bestSource = constraints.front().source;
    for (const auto &constraint : constraints) {
      if (constraint.source->compare(bestSource) < 0)
        bestSource = constraint.source;
    }
    return bestSource;
  }
} // end anonymous namespace

void GenericSignatureBuilder::enumerateRequirements(llvm::function_ref<
                     void (RequirementKind kind,
                           PotentialArchetype *archetype,
                           GenericSignatureBuilder::RequirementRHS constraint,
                           const RequirementSource *source)> f) {
  // Collect all archetypes.
  SmallVector<PotentialArchetype *, 8> archetypes;
  visitPotentialArchetypes([&](PotentialArchetype *archetype) {
    archetypes.push_back(archetype);
  });

  // Remove any invalid potential archetypes or archetypes whose parents are
  // concrete; they have no requirements.
  archetypes.erase(
    std::remove_if(archetypes.begin(), archetypes.end(),
      [&](PotentialArchetype *archetype) -> bool {
        // Invalid archetypes are never representatives in well-formed or
        // corrected signature, so we don't need to visit them.
        if (archetype->isInvalid())
          return true;

        // Keep it.
        return false;
      }),
    archetypes.end());

  // Sort the archetypes in canonical order.
  llvm::array_pod_sort(archetypes.begin(), archetypes.end(),
                       compareDependentTypes);

  for (auto *archetype : archetypes) {
    // Check whether this archetype is one of the anchors within its
    // connected component. If so, we may need to emit a same-type constraint.
    //
    // FIXME: O(n) in the number of implied connected components within the
    // equivalence class. The equivalence class should be small, but...
    auto rep = archetype->getRepresentative();
    auto equivClass = rep->getOrCreateEquivalenceClass();

    // If we didn't compute the derived same-type components yet, do so now.
    if (equivClass->derivedSameTypeComponents.empty())
      checkSameTypeConstraints(Impl->GenericParams, rep);

    assert(!equivClass->derivedSameTypeComponents.empty() &&
           "Didn't compute derived same-type components?");
    auto knownAnchor =
      std::find_if(equivClass->derivedSameTypeComponents.begin(),
                   equivClass->derivedSameTypeComponents.end(),
                   [&](const DerivedSameTypeComponent &component) {
                     return component.anchor == archetype;
                   });
    std::function<void()> deferredSameTypeRequirement;

    if (knownAnchor != equivClass->derivedSameTypeComponents.end()) {
      // If this equivalence class is bound to a concrete type, equate the
      // anchor with a concrete type.
      if (Type concreteType = rep->getConcreteType()) {
        // If the parent of this anchor is also a concrete type, don't
        // create a requirement.
        if (!archetype->isGenericParam() &&
            archetype->getParent()->isConcreteType())
          continue;

        auto source =
          knownAnchor->concreteTypeSource
            ? knownAnchor->concreteTypeSource
            : RequirementSource::forAbstract(archetype);

        f(RequirementKind::SameType, archetype, concreteType, source);
        continue;
      }

      // If we're at the last anchor in the component, do nothing;
      auto nextAnchor = knownAnchor;
      ++nextAnchor;
      if (nextAnchor != equivClass->derivedSameTypeComponents.end()) {
        // Form a same-type constraint from this anchor within the component
        // to the next.
        // FIXME: Distinguish between explicit and inferred here?
        auto otherPA = nextAnchor->anchor;
        deferredSameTypeRequirement = [&f, archetype, otherPA] {
          f(RequirementKind::SameType, archetype, otherPA,
            RequirementSource::forAbstract(archetype));
        };
      }
    }
    SWIFT_DEFER {
      if (deferredSameTypeRequirement) deferredSameTypeRequirement();
    };

    // If this is not the archetype anchor, we're done.
    if (archetype != archetype->getArchetypeAnchor(*this))
      continue;

    // If we have a superclass, produce a superclass requirement
    if (equivClass->superclass) {
      f(RequirementKind::Superclass, archetype, equivClass->superclass,
        getBestConstraintSource<Type>(equivClass->superclassConstraints));
    }

    // If we have a layout constraint, produce a layout requirement.
    if (equivClass->layout) {
      f(RequirementKind::Layout, archetype, equivClass->layout,
        getBestConstraintSource<LayoutConstraint>(
                                              equivClass->layoutConstraints));
    }

    // Enumerate conformance requirements.
    SmallVector<ProtocolDecl *, 4> protocols;
    DenseMap<ProtocolDecl *, const RequirementSource *> protocolSources;
    if (equivClass) {
      for (const auto &conforms : equivClass->conformsTo) {
        protocols.push_back(conforms.first);
        assert(protocolSources.count(conforms.first) == 0 && 
               "redundant protocol requirement?");

        protocolSources.insert(
          {conforms.first,
           getBestConstraintSource<ProtocolDecl *>(conforms.second)});
      }
    }

    // Sort the protocols in canonical order.
    llvm::array_pod_sort(protocols.begin(), protocols.end(), 
                         ProtocolType::compareProtocols);

    // Enumerate the conformance requirements.
    for (auto proto : protocols) {
      assert(protocolSources.count(proto) == 1 && "Missing conformance?");
      f(RequirementKind::Conformance, archetype, 
        proto->getDeclaredInterfaceType(),
        protocolSources.find(proto)->second);
    }
  };
}

void GenericSignatureBuilder::dump() {
  dump(llvm::errs());
}

void GenericSignatureBuilder::dump(llvm::raw_ostream &out) {
  out << "Requirements:";
  enumerateRequirements([&](RequirementKind kind,
                            PotentialArchetype *archetype,
                            GenericSignatureBuilder::RequirementRHS constraint,
                            const RequirementSource *source) {
    switch (kind) {
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
      out << "\n  ";
      out << archetype->getDebugName() << " : " 
          << constraint.get<Type>().getString() << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    case RequirementKind::Layout:
      out << "\n  ";
      out << archetype->getDebugName() << " : "
          << constraint.get<LayoutConstraint>().getString() << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    case RequirementKind::SameType:
      out << "\n  ";
      out << archetype->getDebugName() << " == " ;
      if (auto secondType = constraint.dyn_cast<Type>()) {
        out << secondType.getString();
      } else {
        out << constraint.get<PotentialArchetype *>()->getDebugName();
      }
      out << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    }
  });
  out << "\n";

  out << "Potential archetypes:\n";
  for (auto pa : Impl->PotentialArchetypes) {
    pa->dump(out, &Context.SourceMgr, 2);
  }
  out << "\n";
}

void GenericSignatureBuilder::addGenericSignature(GenericSignature *sig) {
  if (!sig) return;

  for (auto param : sig->getGenericParams())
    addGenericParameter(param);

  // Add the requirements, queuing up same-type requirements until the end.
  // FIXME: Queuing up same-type requirements is a hack that works around
  // problems when referencing associated types. These issues primarily
  // occur when building canonical generic environments
  SmallVector<Requirement, 4> sameTypeRequirements;
  for (auto &reqt : sig->getRequirements()) {
    if (reqt.getKind() == RequirementKind::SameType)
      sameTypeRequirements.push_back(reqt);
    else
      addRequirement(reqt, FloatingRequirementSource::forAbstract(), nullptr);
  }

  // Handle same-type requirements.
  for (auto &reqt : sameTypeRequirements) {
    addRequirement(reqt, FloatingRequirementSource::forAbstract(), nullptr);
  }
}

/// Collect the set of requirements placed on the given generic parameters and
/// their associated types.
static void collectRequirements(GenericSignatureBuilder &builder,
                                ArrayRef<GenericTypeParamType *> params,
                                SmallVectorImpl<Requirement> &requirements) {
  builder.enumerateRequirements([&](RequirementKind kind,
          GenericSignatureBuilder::PotentialArchetype *archetype,
          GenericSignatureBuilder::RequirementRHS type,
          const RequirementSource *source) {
    // Filter out derived requirements... except for concrete-type requirements
    // on generic parameters. The exception is due to the canonicalization of
    // generic signatures, which never eliminates generic parameters even when
    // they have been mapped to a concrete type.
    if (source->isDerivedRequirement() &&
        !(kind == RequirementKind::SameType &&
          archetype->isGenericParam() &&
          type.is<Type>()))
      return;

    auto depTy = archetype->getDependentType(params,
                                             /*allowUnresolved=*/false);

    if (depTy->hasError())
      return;

    Type repTy;
    if (auto concreteTy = type.dyn_cast<Type>()) {
      // Maybe we were equated to a concrete type...
      repTy = concreteTy;

      // Drop requirements involving concrete types containing
      // unresolved associated types.
      if (repTy.findIf([](Type t) -> bool {
            if (auto *depTy = dyn_cast<DependentMemberType>(t.getPointer()))
              if (depTy->getAssocType() == nullptr)
                return true;
            return false;
          })) {
        return;
      }
    } else if (auto layoutConstraint = type.dyn_cast<LayoutConstraint>()) {
      requirements.push_back(Requirement(kind, depTy, layoutConstraint));
      return;
    } else {
      // ...or to a dependent type.
      repTy = type.get<GenericSignatureBuilder::PotentialArchetype *>()
          ->getDependentType(params, /*allowUnresolved=*/false);
    }

    if (repTy->hasError())
      return;

    requirements.push_back(Requirement(kind, depTy, repTy));
  });
}

GenericSignature *GenericSignatureBuilder::getGenericSignature() {
  assert(Impl->finalized && "Must finalize builder first");

  // Collect the requirements placed on the generic parameter types.
  SmallVector<Requirement, 4> requirements;
  collectRequirements(*this, Impl->GenericParams, requirements);

  auto sig = GenericSignature::get(Impl->GenericParams, requirements);
  return sig;
}
