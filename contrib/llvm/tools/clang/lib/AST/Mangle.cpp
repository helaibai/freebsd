//===--- Mangle.cpp - Mangle C++ Names --------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implements generic name mangling support for blocks and Objective-C.
//
//===----------------------------------------------------------------------===//
#include "clang/AST/Attr.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/Basic/ABI.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#define MANGLE_CHECKER 0

#if MANGLE_CHECKER
#include <cxxabi.h>
#endif

using namespace clang;

// FIXME: For blocks we currently mimic GCC's mangling scheme, which leaves
// much to be desired. Come up with a better mangling scheme.

static void mangleFunctionBlock(MangleContext &Context,
                                StringRef Outer,
                                const BlockDecl *BD,
                                raw_ostream &Out) {
  unsigned discriminator = Context.getBlockId(BD, true);
  if (discriminator == 0)
    Out << "__" << Outer << "_block_invoke";
  else
    Out << "__" << Outer << "_block_invoke_" << discriminator+1; 
}

void MangleContext::anchor() { }

enum StdOrFastCC {
  SOF_OTHER,
  SOF_FAST,
  SOF_STD
};

static bool isExternC(const NamedDecl *ND) {
  if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(ND))
    return FD->isExternC();
  return cast<VarDecl>(ND)->isExternC();
}

static StdOrFastCC getStdOrFastCallMangling(const ASTContext &Context,
                                            const NamedDecl *ND) {
  const TargetInfo &TI = Context.getTargetInfo();
  const llvm::Triple &Triple = TI.getTriple();
  if (!Triple.isOSWindows() || Triple.getArch() != llvm::Triple::x86)
    return SOF_OTHER;

  if (Context.getLangOpts().CPlusPlus && !isExternC(ND) &&
      TI.getCXXABI() == TargetCXXABI::Microsoft)
    return SOF_OTHER;

  const FunctionDecl *FD = dyn_cast<FunctionDecl>(ND);
  if (!FD)
    return SOF_OTHER;
  QualType T = FD->getType();

  const FunctionType *FT = T->castAs<FunctionType>();

  CallingConv CC = FT->getCallConv();
  switch (CC) {
  default:
    return SOF_OTHER;
  case CC_X86FastCall:
    return SOF_FAST;
  case CC_X86StdCall:
    return SOF_STD;
  }
}

bool MangleContext::shouldMangleDeclName(const NamedDecl *D) {
  const ASTContext &ASTContext = getASTContext();

  StdOrFastCC CC = getStdOrFastCallMangling(ASTContext, D);
  if (CC != SOF_OTHER)
    return true;

  // In C, functions with no attributes never need to be mangled. Fastpath them.
  if (!getASTContext().getLangOpts().CPlusPlus && !D->hasAttrs())
    return false;

  // Any decl can be declared with __asm("foo") on it, and this takes precedence
  // over all other naming in the .o file.
  if (D->hasAttr<AsmLabelAttr>())
    return true;

  return shouldMangleCXXName(D);
}

void MangleContext::mangleName(const NamedDecl *D, raw_ostream &Out) {
  // Any decl can be declared with __asm("foo") on it, and this takes precedence
  // over all other naming in the .o file.
  if (const AsmLabelAttr *ALA = D->getAttr<AsmLabelAttr>()) {
    // If we have an asm name, then we use it as the mangling.

    // Adding the prefix can cause problems when one file has a "foo" and
    // another has a "\01foo". That is known to happen on ELF with the
    // tricks normally used for producing aliases (PR9177). Fortunately the
    // llvm mangler on ELF is a nop, so we can just avoid adding the \01
    // marker.  We also avoid adding the marker if this is an alias for an
    // LLVM intrinsic.
    StringRef UserLabelPrefix =
        getASTContext().getTargetInfo().getUserLabelPrefix();
    if (!UserLabelPrefix.empty() && !ALA->getLabel().startswith("llvm."))
      Out << '\01'; // LLVM IR Marker for __asm("foo")

    Out << ALA->getLabel();
    return;
  }

  const ASTContext &ASTContext = getASTContext();
  StdOrFastCC CC = getStdOrFastCallMangling(ASTContext, D);
  bool MCXX = shouldMangleCXXName(D);
  const TargetInfo &TI = Context.getTargetInfo();
  if (CC == SOF_OTHER || (MCXX && TI.getCXXABI() == TargetCXXABI::Microsoft)) {
    mangleCXXName(D, Out);
    return;
  }

  Out << '\01';
  if (CC == SOF_STD)
    Out << '_';
  else
    Out << '@';

  if (!MCXX)
    Out << D->getIdentifier()->getName();
  else
    mangleCXXName(D, Out);

  const FunctionDecl *FD = cast<FunctionDecl>(D);
  const FunctionType *FT = FD->getType()->castAs<FunctionType>();
  const FunctionProtoType *Proto = dyn_cast<FunctionProtoType>(FT);
  Out << '@';
  if (!Proto) {
    Out << '0';
    return;
  }
  assert(!Proto->isVariadic());
  unsigned ArgWords = 0;
  if (const CXXMethodDecl *MD = dyn_cast<CXXMethodDecl>(FD))
    if (!MD->isStatic())
      ++ArgWords;
  for (const auto &AT : Proto->param_types())
    // Size should be aligned to DWORD boundary
    ArgWords += llvm::RoundUpToAlignment(ASTContext.getTypeSize(AT), 32) / 32;
  Out << 4 * ArgWords;
}

void MangleContext::mangleGlobalBlock(const BlockDecl *BD,
                                      const NamedDecl *ID,
                                      raw_ostream &Out) {
  unsigned discriminator = getBlockId(BD, false);
  if (ID) {
    if (shouldMangleDeclName(ID))
      mangleName(ID, Out);
    else {
      Out << ID->getIdentifier()->getName();
    }
  }
  if (discriminator == 0)
    Out << "_block_invoke";
  else
    Out << "_block_invoke_" << discriminator+1;
}

void MangleContext::mangleCtorBlock(const CXXConstructorDecl *CD,
                                    CXXCtorType CT, const BlockDecl *BD,
                                    raw_ostream &ResStream) {
  SmallString<64> Buffer;
  llvm::raw_svector_ostream Out(Buffer);
  mangleCXXCtor(CD, CT, Out);
  Out.flush();
  mangleFunctionBlock(*this, Buffer, BD, ResStream);
}

void MangleContext::mangleDtorBlock(const CXXDestructorDecl *DD,
                                    CXXDtorType DT, const BlockDecl *BD,
                                    raw_ostream &ResStream) {
  SmallString<64> Buffer;
  llvm::raw_svector_ostream Out(Buffer);
  mangleCXXDtor(DD, DT, Out);
  Out.flush();
  mangleFunctionBlock(*this, Buffer, BD, ResStream);
}

void MangleContext::mangleBlock(const DeclContext *DC, const BlockDecl *BD,
                                raw_ostream &Out) {
  assert(!isa<CXXConstructorDecl>(DC) && !isa<CXXDestructorDecl>(DC));

  SmallString<64> Buffer;
  llvm::raw_svector_ostream Stream(Buffer);
  if (const ObjCMethodDecl *Method = dyn_cast<ObjCMethodDecl>(DC)) {
    mangleObjCMethodName(Method, Stream);
  } else {
    const NamedDecl *ND = cast<NamedDecl>(DC);
    if (!shouldMangleDeclName(ND) && ND->getIdentifier())
      Stream << ND->getIdentifier()->getName();
    else {
      // FIXME: We were doing a mangleUnqualifiedName() before, but that's
      // a private member of a class that will soon itself be private to the
      // Itanium C++ ABI object. What should we do now? Right now, I'm just
      // calling the mangleName() method on the MangleContext; is there a
      // better way?
      mangleName(ND, Stream);
    }
  }
  Stream.flush();
  mangleFunctionBlock(*this, Buffer, BD, Out);
}

void MangleContext::mangleObjCMethodName(const ObjCMethodDecl *MD,
                                         raw_ostream &Out) {
  SmallString<64> Name;
  llvm::raw_svector_ostream OS(Name);
  
  const ObjCContainerDecl *CD =
  dyn_cast<ObjCContainerDecl>(MD->getDeclContext());
  assert (CD && "Missing container decl in GetNameForMethod");
  OS << (MD->isInstanceMethod() ? '-' : '+') << '[' << CD->getName();
  if (const ObjCCategoryImplDecl *CID = dyn_cast<ObjCCategoryImplDecl>(CD))
    OS << '(' << *CID << ')';
  OS << ' ';
  MD->getSelector().print(OS);
  OS << ']';
  
  Out << OS.str().size() << OS.str();
}
