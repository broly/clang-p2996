//===--- ParseReflect.cpp - C++2c Reflection Parsing (P2996) --------------===//
//
// Copyright 2024 Bloomberg Finance L.P.
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements parsing for reflection facilities.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/LocInfoType.h"
#include "clang/Basic/DiagnosticParse.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/Parser.h"
#include "clang/Parse/RAIIObjectsForParser.h"
#include "clang/Sema/EnterExpressionEvaluationContext.h"
#include "clang/Sema/Ownership.h"
#include "clang/Sema/ParsedAttr.h"
using namespace clang;

ExprResult Parser::ParseCXXReflectExpression(SourceLocation OpLoc) {
  SourceLocation OperandLoc = Tok.getLocation();

  Sema::ConstevalOnlyRecorder RecordConstevalOnly(Actions);
  EnterExpressionEvaluationContext EvalContext(
        Actions, Sema::ExpressionEvaluationContext::ReflectionContext);

  // Parse a leading nested-name-specifier, e.g.,
  //
  CXXScopeSpec SS;
  if (ParseOptionalCXXScopeSpecifier(SS, /*ObjectType=*/nullptr,
                                     /*ObjectHasErrors=*/false,
                                     /*EnteringContext=*/false)) {
    SkipUntil(tok::semi, StopAtSemi | StopBeforeMatch);
    return ExprError();
  }

  // Start the tentative parse: This will be reverted if the operand is found
  // to be a type (or rather: a type whose name is more complicated than a
  // single identifier).
  //
  TentativeParsingAction TentativeAction(*this);

  // Next, check for an unqualified-id.
  if (Tok.isOneOf(tok::identifier, tok::kw_operator, tok::kw_template,
                  tok::tilde, tok::annot_template_id)) {
    // Try parsing the operand name as an 'unqualified-id'.

    SourceLocation TemplateKWLoc;
    UnqualifiedId UnqualName;
    if (!ParseUnqualifiedId(SS, ParsedType{}, /*ObjectHadError=*/false,
                            /*EnteringContext=*/false,
                            /*AllowDestructorName=*/true,
                            /*AllowConstructorName=*/false,
                            /*AllowDeductionGuide=*/false,
                            SS.isSet() ? &TemplateKWLoc : nullptr,
                            UnqualName)) {
      bool AssumeType = false;
      if (UnqualName.getKind() == UnqualifiedIdKind::IK_TemplateId &&
          UnqualName.TemplateId->Kind == TNK_Type_template)
        AssumeType = true;
      else if (Tok.isOneOf(tok::l_square, tok::l_paren, tok::star, tok::amp,
                           tok::ampamp, tok::kw_const, tok::kw_volatile,
                           tok::kw_restrict))
        AssumeType = true;

      if (!AssumeType) {
        TentativeAction.Commit();
        return RecordConstevalOnly.RecordAndReturn(
                Actions.ActOnCXXReflectExpr(OpLoc, TemplateKWLoc, SS,
                                            UnqualName));
      }
    }
  } else if (SS.isValid() &&
             SS.getScopeRep()->getKind() == NestedNameSpecifier::Global) {
    // Check for '^::'.
    TentativeAction.Commit();

    Decl *TUDecl = Actions.getASTContext().getTranslationUnitDecl();
    return RecordConstevalOnly.RecordAndReturn(
            Actions.ActOnCXXReflectExpr(OpLoc, SourceLocation(), TUDecl));
  }
  TentativeAction.Revert();

  // Check for attribute
  {
    size_t last = Attrs.size();
    if (MaybeParseCXX11Attributes(Attrs)) {
      size_t newLast = Attrs.size();

      // Reflect expression of empty attribute list is ill formed
      if (last == newLast) {
        Diag(OperandLoc, diag::p3385_trace_empty_attributes_list);
        return ExprError();
      }
      // Reflect expression of multiple attributes is ill formed
      if (newLast - last > 1) {
        Diag(OperandLoc, diag::p3385_err_attributes_list) << (newLast - last);
        return ExprError();
      }
      // Reflects expression of unsupported attribute is ill formed
      auto * attribute = &Attrs.back();
      bool isReflectable = isAttributeWithReflectableVariant(attribute->getParsedKind());
      if (!isReflectable) {
        Diag(OpLoc, diag::p3385_warn_unsupported_attribute) << attribute->getAttrName()->getName();
        return ExprError();
      }
      return Actions.ActOnCXXReflectExpr(OpLoc, attribute);
    }
  }

  if (SS.isSet() &&
      TryAnnotateTypeOrScopeTokenAfterScopeSpec(SS, true,
                                                ImplicitTypenameContext::No)) {
    SkipUntil(tok::semi, StopAtSemi | StopBeforeMatch);
    return ExprError();
  }

  // Anything else must be a type-id (e.g., 'const int', 'Cls(*)(int)'.
  if (isCXXTypeId(TentativeCXXTypeIdContext::AsReflectionOperand)) {
    TypeResult TR = ParseTypeName(nullptr, DeclaratorContext::ReflectOperator);
    if (TR.isInvalid())
      return ExprError();

    std::string refKind;
    if (QualType QT = cast<LocInfoType>(TR.get().get())->getType();
        QT->isLValueReferenceType()) {
      refKind = "&";
    } else if (QT->isRValueReferenceType()) {
      refKind = "&&";
    } else if (auto *FPT = dyn_cast<FunctionProtoType>(QT)) {
      if (FPT->getRefQualifier() == RQ_LValue)
        refKind = "&";
      else if (FPT->getRefQualifier() == RQ_RValue)
        refKind = "&&";
    }

    if (!refKind.empty() &&
        !Tok.isOneOf(tok::r_paren, tok::greater, tok::greatergreater,
                     tok::comma, tok::r_brace, tok::r_square, tok::r_splice,
                     tok::semi, tok::ellipsis, tok::colon, tok::question)) {
      TypeLoc TL = cast<LocInfoType>(TR.get().get())
          ->getTypeSourceInfo()->getTypeLoc();

      Diag(OperandLoc, diag::warn_meant_parenthesize_reflection)
        << refKind << TL.getSourceRange();
    }

    return RecordConstevalOnly.RecordAndReturn(
            Actions.ActOnCXXReflectExpr(OpLoc, TR));
  }

  Diag(OperandLoc, diag::err_cannot_reflect_operand);
  return ExprError();
}

ExprResult Parser::ParseCXXMetafunctionExpression() {
  assert(Tok.is(tok::kw___metafunction) && "expected '___metafunction'");
  SourceLocation KwLoc = ConsumeToken();

  // Balance any number of arguments in parens.
  BalancedDelimiterTracker Parens(*this, tok::l_paren);
  if (Parens.expectAndConsume())
    return ExprError();

  SmallVector<Expr *, 2> Args;
  do {
    ExprResult Expr = ParseConstantExpression();
    if (Expr.isInvalid()) {
      Parens.skipToEnd();
      return ExprError();
    }
    Args.push_back(Expr.get());
  } while (TryConsumeToken(tok::comma));

  if (Parens.consumeClose())
    return ExprError();

  SourceLocation LPLoc = Parens.getOpenLocation();
  SourceLocation RPLoc = Parens.getCloseLocation();
  return Actions.ActOnCXXMetafunction(KwLoc, LPLoc, Args, RPLoc);
}

bool Parser::ParseSpliceSpecifier(bool TryParseSpecialization) {
  assert(Tok.is(tok::l_splice) && "expected '[:'");

  BalancedDelimiterTracker SpliceTokens(*this, tok::l_splice);
  if (SpliceTokens.expectAndConsume())
    return true;

  ExprResult ER = ParseConstantExpression();
  if (ER.isInvalid() || ER.get()->containsErrors()) {
    SpliceTokens.skipToEnd();
    return true;
  }
  Expr *Operand = ER.get();

  Token end = Tok;
  if (SpliceTokens.consumeClose())
    return true;

  SourceLocation LSplice = SpliceTokens.getOpenLocation();
  SourceLocation RSplice = SpliceTokens.getCloseLocation();

  SpliceResult SR;
  if (TryParseSpecialization && Tok.is(tok::less)) {
    SourceLocation LAngleLoc, RAngleLoc;

    // 'TArgs' must outlive the 'ASTTemplateArgsPtr' aliasing its storage, and
    // therefore the call to 'ActOnSpliceSpecifier' which reads through it.
    TemplateArgList TArgs;
    if (ParseTemplateIdAfterTemplateName(/*ConsumeLastToken=*/false, LAngleLoc,
                                         TArgs, RAngleLoc,
                                         /*Template=*/nullptr))
      return true;

    ASTTemplateArgsPtr TArgsPtr(TArgs.data(), TArgs.size());
    end = Tok;
    ConsumeToken();

    SR = Actions.ActOnSpliceSpecifier(LSplice, Operand, RSplice, LAngleLoc,
                                      TArgsPtr, RAngleLoc);
  } else {
    SR = Actions.ActOnSpliceSpecifier(LSplice, Operand, RSplice);
  }
  if (SR.isInvalid())
    return true;
  SpliceSpecifier *Splice = SR.get();

  UnconsumeToken(end);
  Tok.setKind(tok::annot_splice);
  setSpliceAnnotation(Tok, Splice);
  Tok.setLocation(Splice->getBeginLoc());
  Tok.setAnnotationEndLoc(Splice->getEndLoc());
  PP.AnnotateCachedTokens(Tok);

  return false;
}

ExprResult Parser::ParseCXXSpliceAsExpr(SourceLocation TemplateKWLoc,
                                        bool AllowMemberReference) {
  assert(Tok.is(tok::annot_splice) && "expected a splice annotation");

  SpliceResult SR = getSpliceAnnotation(Tok);
  if (SR.isInvalid())
    return ExprError();
  SpliceSpecifier *Splice = SR.get();

  assert((!Splice->isSpecialization() || TemplateKWLoc.isValid()) &&
         "splice-specialization-specifier required leading 'template'");
  ConsumeAnnotationToken();

  return Actions.ActOnCXXSpliceExpression(TemplateKWLoc, Splice,
                                          AllowMemberReference);
}

TypeResult Parser::ParseCXXSpliceAsType(SourceLocation TypenameKWLoc,
                                        bool AllowDependent, bool Complain) {
  assert(Tok.is(tok::annot_splice) && "expected a splice annotation");

  SpliceResult SR = getSpliceAnnotation(Tok);
  if (SR.isInvalid())
    return TypeError();
  SpliceSpecifier *Splice = SR.get();

  TypeResult Result = Actions.ActOnCXXSpliceTypeSpecifier(TypenameKWLoc,
                                                          Splice, Complain);
  if (!Result.isInvalid())
    ConsumeAnnotationToken();

  return Result;
}

DeclResult Parser::ParseCXXSpliceAsNamespace() {
  assert(Tok.is(tok::annot_splice) && "expected annot_splice");

  SpliceResult SR = getSpliceAnnotation(Tok);
  if (SR.isInvalid())
    return DeclError();
  SpliceSpecifier *Splice = SR.get();

  assert(!Splice->isSpecialization() &&
         "splice-specialization-specifier cannot represent a namespace");
  ConsumeAnnotationToken();

  return Actions.ActOnCXXSpliceExpectingNamespace(Splice);
}

                        // ===============================
                        // Token sequences (P3294)
                        // ===============================

namespace {
enum class TokenSeqPieceKind { Text, Value, Identifier, Tokens };

struct TokenSeqPiece {
  TokenSeqPieceKind Kind;
  std::string Text;                // for 'Text'
  SmallVector<Token, 8> Operand;   // for the interpolations
};
} // namespace

ExprResult Parser::ParseTokenSequenceExpression(SourceLocation OpLoc) {
  assert(Tok.is(tok::l_brace) && "not a token sequence");
  ConsumeAnyToken();  // '{'

  SourceManager &SM = PP.getSourceManager();
  auto IsBackslash = [&](const Token &T) {
    return T.is(tok::unknown) && T.getLength() == 1 &&
           *SM.getCharacterData(T.getLocation()) == '\\';
  };

  SmallVector<TokenSeqPiece, 8> Pieces;
  std::string CurText;
  auto FlushText = [&] {
    if (CurText.empty())
      return;
    Pieces.push_back({TokenSeqPieceKind::Text, std::move(CurText), {}});
    CurText.clear();
  };

  unsigned Depth = 0;
  while (true) {
    if (Tok.is(tok::eof)) {
      Diag(Tok, diag::err_expected) << tok::r_brace;
      return ExprError();
    }
    if (Tok.is(tok::r_brace) && Depth == 0)
      break;

    if (IsBackslash(Tok)) {
      ConsumeAnyToken();  // '\'

      TokenSeqPieceKind Kind = TokenSeqPieceKind::Value;
      if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()->isStr("id")) {
        Kind = TokenSeqPieceKind::Identifier;
        ConsumeAnyToken();
      } else if (Tok.is(tok::identifier) &&
                 Tok.getIdentifierInfo()->isStr("tokens")) {
        Kind = TokenSeqPieceKind::Tokens;
        ConsumeAnyToken();
      }
      if (Tok.isNot(tok::l_paren)) {
        Diag(Tok, diag::err_expected) << tok::l_paren;
        return ExprError();
      }
      ConsumeAnyToken();  // '('

      TokenSeqPiece Piece{Kind, {}, {}};
      unsigned OperandDepth = 0;
      while (!(Tok.is(tok::r_paren) && OperandDepth == 0)) {
        if (Tok.is(tok::eof)) {
          Diag(Tok, diag::err_expected) << tok::r_paren;
          return ExprError();
        }
        if (Tok.isOneOf(tok::l_paren, tok::l_square, tok::l_brace))
          ++OperandDepth;
        else if (Tok.isOneOf(tok::r_paren, tok::r_square, tok::r_brace))
          --OperandDepth;
        Piece.Operand.push_back(Tok);
        ConsumeAnyToken();
      }
      ConsumeAnyToken();  // ')'

      FlushText();
      Pieces.push_back(std::move(Piece));
      continue;
    }

    if (Tok.isOneOf(tok::l_paren, tok::l_square, tok::l_brace))
      ++Depth;
    else if (Tok.isOneOf(tok::r_paren, tok::r_square, tok::r_brace))
      --Depth;

    SmallString<32> Buffer;
    CurText += PP.getSpelling(Tok, Buffer);
    CurText += ' ';
    ConsumeAnyToken();
  }
  FlushText();
  ConsumeAnyToken();  // '}'

  // Rewrite the sequence as:
  //   ::std::meta::__ts_make(::std::meta::__ts_lit("..."),
  //                          ::std::meta::__ts_val(e), ...)
  SmallVector<Token, 64> Out;
  auto Punct = [&](tok::TokenKind K, unsigned Len = 1) {
    Token T;
    T.startToken();
    T.setKind(K);
    T.setLocation(OpLoc);
    T.setLength(Len);
    Out.push_back(T);
  };
  auto Ident = [&](StringRef Name) {
    Token T;
    T.startToken();
    T.setKind(tok::identifier);
    T.setLocation(OpLoc);
    T.setLength(Name.size());
    T.setIdentifierInfo(PP.getIdentifierInfo(Name));
    Out.push_back(T);
  };
  auto Qualified = [&](StringRef Name) {
    Punct(tok::coloncolon, 2);
    Ident("std");
    Punct(tok::coloncolon, 2);
    Ident("meta");
    Punct(tok::coloncolon, 2);
    Ident(Name);
  };
  auto StringLit = [&](StringRef Contents) {
    std::string Quoted = "\"";
    for (char C : Contents) {
      if (C == '"' || C == '\\')
        Quoted += '\\';
      Quoted += C;
    }
    Quoted += '"';
    Token T;
    T.startToken();
    T.setKind(tok::string_literal);
    PP.CreateString(Quoted, T, OpLoc, OpLoc);
    Out.push_back(T);
  };

  Qualified("__ts_make");
  Punct(tok::l_paren);
  bool First = true;
  for (TokenSeqPiece &P : Pieces) {
    if (!First)
      Punct(tok::comma);
    First = false;

    switch (P.Kind) {
    case TokenSeqPieceKind::Text:
      Qualified("__ts_lit");
      break;
    case TokenSeqPieceKind::Value:
      Qualified("__ts_val");
      break;
    case TokenSeqPieceKind::Identifier:
      Qualified("__ts_id");
      break;
    case TokenSeqPieceKind::Tokens:
      Qualified("__ts_tok");
      break;
    }
    Punct(tok::l_paren);
    if (P.Kind == TokenSeqPieceKind::Text)
      StringLit(P.Text);
    else
      Out.append(P.Operand.begin(), P.Operand.end());
    Punct(tok::r_paren);
  }
  Punct(tok::r_paren);

  Token End;
  End.startToken();
  End.setKind(tok::eof);
  End.setLocation(OpLoc);
  Out.push_back(End);
  // Append the current token so that it is not lost.
  Out.push_back(Tok);
  PP.EnterTokenStream(Out, /*DisableMacroExpansion=*/true,
                      /*IsReinject=*/true);
  ConsumeAnyToken();

  ExprResult Result = ParseAssignmentExpression();
  if (Tok.is(tok::eof)) {
    ConsumeAnyToken();  // restores the saved token
  } else {
    Diag(Tok, diag::err_expected) << tok::r_paren;
    while (Tok.isNot(tok::eof))
      ConsumeAnyToken();
    ConsumeAnyToken();
    return ExprError();
  }
  return Result;
}
