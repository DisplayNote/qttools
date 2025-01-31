/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Linguist of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/
#include "clangtoolastreader.h"

#include <QtCore/qregularexpression.h>

#include <clang/Lex/MacroArgs.h>
#include <clang/Basic/TokenKinds.h>

QT_BEGIN_NAMESPACE

namespace LupdatePrivate
{
    QString contextForFunctionDecl(clang::FunctionDecl *func, const std::string &funcName)
    {
        std::string context;
#if (LUPDATE_CLANG_VERSION >= LUPDATE_CLANG_VERSION_CHECK(10,0,0))
        {
            llvm::raw_string_ostream tmp(context);
            func->printQualifiedName(tmp);
        }
#else
        context = func->getQualifiedNameAsString();
#endif
        return QString::fromStdString(context.substr(0, context.find("::" + funcName, 0)));
    }

    enum QuoteCompulsary
    {
        None = 0x01,
        Left = 0x02,                // Left quote is mandatory
        Right = 0x04,               // Right quote is mandatory
        LeftAndRight = Left | Right // Both quotes are mandatory
    };

    /*
        Removes the quotes around the lupdate extra, ID meta data, magic and
        ID prefix comments and source string literals.
        Depending on the given compulsory option, quotes can be unbalanced and
        still some text is returned. This is to mimic the old lupdate behavior.
    */
    QString cleanQuote(llvm::StringRef s, QuoteCompulsary quote)
    {
        if (s.empty())
            return {};
        s = s.trim();
        if (!s.consume_front("\"") && ((quote & Left) != 0))
            return {};
        if (!s.consume_back("\"") && ((quote & Right) != 0))
            return {};
        return QString::fromStdString(s);
    }

    /*
        Removes the quotes and a possible existing string literal prefix
        for a given string literal coming from the source code. Do not use
        to clean the quotes around the lupdate translator specific comments.
    */
    QString cleanQuote(const std::string &token)
    {
        if (token.empty())
            return {};

        const QString string = QString::fromStdString(token).trimmed();
        const int index = string.indexOf(QLatin1Char('"'));
        if (index <= 0)
            return LupdatePrivate::cleanQuote(token, QuoteCompulsary::LeftAndRight);

        QRegularExpressionMatch result;
        if (string.at(index - 1) == QLatin1Char('R')) {
            static const QRegularExpression rawStringLiteral {
                QStringLiteral(
                    "(?:\\bu8|\\b[LuU])??R\\\"([^\\(\\)\\\\ ]{0,16})\\((?<characters>.*)\\)\\1\\\""
                ), QRegularExpression::DotMatchesEverythingOption };
            result = rawStringLiteral.match(string);
        } else {
            static const QRegularExpression stringLiteral {
                QStringLiteral(
                    "(?:\\bu8|\\b[LuU])+?\\\"(?<characters>[^\\\"\\\\]*(?:\\\\.[^\\\"\\\\]*)*)\\\""
                )
            };
            result = stringLiteral.match(string);
        }
        if (result.hasMatch())
            return result.captured(QStringLiteral("characters"));
        return string;
    }

    static bool capture(const QRegularExpression &exp, const QString &line, QString *i, QString *c)
    {
        i->clear(), c->clear();
        auto result = exp.match(line);
        if (!result.hasMatch())
            return false;

        *i = result.captured(QLatin1String("identifier"));
        *c = result.captured(QStringLiteral("comment")).trimmed();

        if (*i == QLatin1String("%"))
            *c = LupdatePrivate::cleanQuote(c->toStdString(), QuoteCompulsary::Left);

        return !c->isEmpty();
    }

    bool hasQuote(llvm::StringRef source)
    {
        return source.contains("\"");
    }

    bool trFunctionPresent(llvm::StringRef text)
    {
        if (text.contains(llvm::StringRef("qtTrId(")))
            return true;
        if (text.contains(llvm::StringRef("tr(")))
            return true;
        if (text.contains(llvm::StringRef("trUtf8(")))
            return true;
        if (text.contains(llvm::StringRef("translate(")))
            return true;
        if (text.contains(llvm::StringRef("Q_DECLARE_TR_FUNCTIONS(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TR_N_NOOP(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TRID_N_NOOP(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TRANSLATE_N_NOOP(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TRANSLATE_N_NOOP3(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TR_NOOP(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TRID_NOOP(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TRANSLATE_NOOP(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TRANSLATE_NOOP3(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TR_NOOP_UTF8(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TRANSLATE_NOOP_UTF8(")))
            return true;
        if (text.contains(llvm::StringRef("QT_TRANSLATE_NOOP3_UTF8(")))
            return true;
        return false;
    }
}

/*
    The visit call expression function is called automatically after the
    visitor TraverseAST function is called. This is the function where the
    "tr", "trUtf8", "qtIdTr", "translate" functions are picked up in the AST.
    Previously mentioned functions are always part of a CallExpression.
*/
bool LupdateVisitor::VisitCallExpr(clang::CallExpr *callExpression)
{
    const auto fullLocation = m_context->getFullLoc(callExpression->getBeginLoc());
    if (fullLocation.isInvalid())
        return true;

    // Checking that the CallExpression is from the input file we're interested in
    std::string fileName;
    if (const auto fileEntry = fullLocation.getFileEntry())
        fileName = fileEntry->getName();
    if (fileName != m_inputFile)
        return true;

    clang::FunctionDecl *func = callExpression->getDirectCallee();
    if (!func)
        return true;
    clang::QualType q = callExpression->getType();
    if (!q.getTypePtrOrNull())
        return true;
    const std::string funcName = func->getNameInfo().getAsString();
    // Only continue if the function a translation function (TODO: deal with alias function...)
    if (funcName != "tr" && funcName != "qtTrId" && funcName != "translate" && funcName != "trUtf8")
        return true;

    qCDebug(lcClang) << "************************** VisitCallExpr ****************";
    // Retrieving the information needed to fill the lupdate translator.
    // Function independent retrieve
    TranslationRelatedStore store;
    store.callType = QStringLiteral("ASTRead_CallExpr");
    store.funcName = QString::fromStdString(funcName);
    store.lupdateLocationFile = QString::fromStdString(fileName);
    store.lupdateLocationLine = fullLocation.getSpellingLineNumber();
    store.contextRetrieved = LupdatePrivate::contextForFunctionDecl(func, funcName);

    qCDebug(lcClang) << "CallType          : ASTRead_CallExpr";
    qCDebug(lcClang) << "Function name     : " << store.funcName;
    qCDebug(lcClang) << "File location     : " << store.lupdateLocationFile;
    qCDebug(lcClang) << "Line              : " << store.lupdateLocationLine;
    qCDebug(lcClang) << "Context retrieved : " << store.contextRetrieved;

    // Here we gonna need to retrieve the comments around the function call
    // //: //* //~ Things like that
    const std::vector<QString> rawComments = rawCommentsForCallExpr(callExpression);
    for (const auto &rawComment : rawComments) {
        setInfoFromRawComment(rawComment, &store);
        qCDebug(lcClang) << "Raw comments     :" << rawComment;
    }

    clang::LangOptions langOpts;
    langOpts.CPlusPlus = true;
    clang::PrintingPolicy policy(langOpts);
    std::vector<std::string> arguments(callExpression->getNumArgs(), "");
    for (unsigned int i = 0; i < callExpression->getNumArgs(); i++) {
        auto arg = callExpression->getArg(i);
        llvm::raw_string_ostream temp(arguments[i]);
        arg->printPretty(temp, nullptr, policy);
    }

    // Function dependent retrieve!
    switch (trFunctionAliasManager.trFunctionByName(QString::fromStdString(funcName))) {
    case TrFunctionAliasManager::Function_tr:
    case TrFunctionAliasManager::Function_trUtf8:
        if (arguments.size() != 3 || !LupdatePrivate::hasQuote(arguments[0]))
            return true;
        store.lupdateSource = LupdatePrivate::cleanQuote(arguments[0]);
        store.lupdateComment = LupdatePrivate::cleanQuote(arguments[1]);
        store.lupdatePlural = QString::fromStdString(arguments[2]);
        qCDebug(lcClang) << "Source      : " << store.lupdateSource;
        qCDebug(lcClang) << "Comment     : " << store.lupdateComment;
        qCDebug(lcClang) << "Plural      : " << store.lupdatePlural;
        break;
    case TrFunctionAliasManager::Function_translate:
        if (arguments.size() != 4 || !LupdatePrivate::hasQuote(arguments[0])
            || !LupdatePrivate::hasQuote(arguments[1])) {
            return true;
        }
        store.contextArg = LupdatePrivate::cleanQuote(arguments[0]);
        store.lupdateSource = LupdatePrivate::cleanQuote(arguments[1]);
        store.lupdateComment = LupdatePrivate::cleanQuote(arguments[2]);
        store.lupdatePlural = QString::fromStdString(arguments[3]);
        qCDebug(lcClang) << "Context Arg : " << store.contextArg;
        qCDebug(lcClang) << "Source      : " << store.lupdateSource;
        qCDebug(lcClang) << "Comment     : " << store.lupdateComment;
        qCDebug(lcClang) << "Plural      : " << store.lupdatePlural;
        break;
    case TrFunctionAliasManager::Function_qtTrId:
        if (arguments.size() != 2 || !LupdatePrivate::hasQuote(arguments[0]))
            return true;
        store.lupdateId = LupdatePrivate::cleanQuote(arguments[0]);
        store.lupdatePlural = QString::fromStdString(arguments[1]);
        qCDebug(lcClang) << "ID          : " << store.lupdateId;
        qCDebug(lcClang) << "Plural      : " << store.lupdatePlural;
        break;
    }
    m_translationStoresFromAST.push_back(store);
    return true;
}

/*
    Retrieve the comments associated with the CallExpression.
*/
std::vector<QString> LupdateVisitor::rawCommentsForCallExpr(const clang::CallExpr *callExpr) const
{
    if (!m_context)
        return {};
    return rawCommentsFromSourceLocation(m_context->getFullLoc(callExpr->getBeginLoc()));
}

std::vector<QString> LupdateVisitor::rawCommentsFromSourceLocation(
    clang::SourceLocation sourceLocation) const
{
    if (!m_context)
        return {};
    if (sourceLocation.isInvalid() || !sourceLocation.isFileID()) {
        qCDebug(lcClang) << "The declaration does not map directly to a location in a file,"
            " early return.";
        return {};
    }
    auto &sourceMgr = m_context->getSourceManager();

#if (LUPDATE_CLANG_VERSION >= LUPDATE_CLANG_VERSION_CHECK(10,0,0))
    const clang::FileID file = sourceMgr.getDecomposedLoc(sourceLocation).first;
    const auto commentsInThisFile = m_context->getRawCommentList().getCommentsInFile(file);
    if (!commentsInThisFile)
        return {};

    std::vector<clang::RawComment *> tmp;
    for (auto commentInFile : *commentsInThisFile)
        tmp.emplace_back(commentInFile.second);
    clang::ArrayRef<clang::RawComment *> rawComments = tmp;
#else
    clang::ArrayRef<clang::RawComment *> rawComments = m_context->getRawCommentList().getComments();
#endif

    // If there are no comments anywhere, we won't find anything.
    if (rawComments.empty())
        return {};

    // Create a dummy raw comment with the source location of the declaration.
    clang::RawComment commentAtDeclarationLocation(sourceMgr,
        clang::SourceRange(sourceLocation), m_context->getLangOpts().CommentOpts, false);

    // Create a functor object to compare the source location of the comment and the declaration.
    const clang::BeforeThanCompare<clang::RawComment> compareSourceLocation(sourceMgr);
    //  Find the comment that occurs just after or within this declaration. Possible findings:
    //  QObject::tr(/* comment 1 */ "test"); //: comment 2   -> finds "//: comment 1"
    //  QObject::tr("test"); //: comment 1                   -> finds "//: comment 1"
    //  QObject::tr("test");
    //  //: comment 1                                        -> finds "//: comment 1"
    //  /*: comment 1 */ QObject::tr("test");                -> finds no trailing comment
    auto comment = std::lower_bound(rawComments.begin(), rawComments.end(),
        &commentAtDeclarationLocation, compareSourceLocation);

    // We did not find any comment before the declaration.
    if (comment == rawComments.begin())
        return {};

    // Decompose the location for the declaration and find the beginning of the file buffer.
    std::pair<clang::FileID, unsigned> declLocDecomp = sourceMgr.getDecomposedLoc(sourceLocation);

    // Get the text buffer from the beginning of the file up through the declaration's begin.
    bool invalid = false;
    const char *buffer = sourceMgr.getBufferData(declLocDecomp.first, &invalid).data();
    if (invalid) {
        qCDebug(lcClang).nospace() << "An error occurred fetching the source buffer of file: "
            << sourceMgr.getFilename(sourceLocation);
        return {};
    }

    std::vector<QString> retrievedRawComments;
    auto lastDecompLoc = declLocDecomp.second;
    const auto declLineNum = sourceMgr.getLineNumber(declLocDecomp.first, declLocDecomp.second);
    do {
        std::advance(comment, -1);

        // Decompose the end of the comment.
        std::pair<clang::FileID, unsigned> commentEndDecomp
            = sourceMgr.getDecomposedLoc((*comment)->getSourceRange().getEnd());

        // If the comment and the declaration aren't in the same file, then they aren't related.
        if (declLocDecomp.first != commentEndDecomp.first) {
            qCDebug(lcClang) << "Comment and the declaration aren't in the same file. Comment '"
                << (*comment)->getRawText(sourceMgr) << "' is ignored, return.";
            return retrievedRawComments;
        }

        // Current lupdate ignores comments on the same line before the declaration.
        // void Class42::hello(int something /*= 17 */, QString str = Class42::tr("eyo"))
        if (declLineNum == sourceMgr.getLineNumber(commentEndDecomp.first, commentEndDecomp.second)) {
            qCDebug(lcClang) << "Comment ends on same line as the declaration. Comment '"
                << (*comment)->getRawText(sourceMgr) << "' is ignored, continue.";
            continue;
        }

        // Extract text between the comment and declaration.
        llvm::StringRef text(buffer + commentEndDecomp.second,
            lastDecompLoc - commentEndDecomp.second);

        // There should be no other declarations or preprocessor directives between
        // comment and declaration.
        if (text.find_first_of(";}#@") != llvm::StringRef::npos) {
            qCDebug(lcClang) << "Found another declaration or preprocessor directive between"
                " comment and declaration, break.";
            break;
        }

        // There should be no other translation function between comment and declaration.
        if (LupdatePrivate::trFunctionPresent(text)) {
            qCDebug(lcClang) << "Found another translation function between comment and "
                "declaration, break.";
            break;
        }

        retrievedRawComments.emplace(retrievedRawComments.begin(),
            QString::fromStdString((*comment)->getRawText(sourceMgr)));
        lastDecompLoc = sourceMgr.getDecomposedLoc((*comment)->getSourceRange().getBegin()).second;
    } while (comment != rawComments.begin());

    return retrievedRawComments;
}

/*
    Read the raw comments and split them according to the prefix.
    Fill the corresponding variables in the TranslationRelatedStore.
*/
void LupdateVisitor::setInfoFromRawComment(const QString &commentString,
    TranslationRelatedStore *store)
{
    const QStringList commentLines = commentString.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    static const QRegularExpression
        cppStyle(QStringLiteral("^\\/\\/(?<identifier>[:=~%])\\s+(?<comment>.+)$"));
    static const QRegularExpression
        cStyleSingle(QStringLiteral("^\\/\\*(?<identifier>[:=~%])\\s+(?<comment>.+)\\*\\/$"));
    static const QRegularExpression
        cStyleMultiBegin(QStringLiteral("^\\/\\*(?<identifier>[:=~%])\\s+(?<comment>.*)$"));

    static const QRegularExpression isSpace(QStringLiteral("\\s+"));
    static const QRegularExpression idefix(QStringLiteral("^\\/\\*(?<identifier>[:=~%])"));

    bool save = false;
    bool sawStarPrefix = false;
    bool sourceIdentifier = false;

    QString comment, identifier;
    for (auto line : commentLines) {
        line = line.trimmed();

        if (!sawStarPrefix) {
            if (line.startsWith(QStringLiteral("//"))) {
                // Process C++ style comment.
                save = LupdatePrivate::capture(cppStyle, line, &identifier, &comment);
            } else if (line.startsWith(QLatin1String("/*")) && line.endsWith(QLatin1String("*/"))) {
                // Process C style comment on a single line.
                save = LupdatePrivate::capture(cStyleSingle, line, &identifier, &comment);
            } else if (line.startsWith(QLatin1String("/*"))) {
                sawStarPrefix = true; // Start processing a multi line C style comment.

                auto result = idefix.match(line);
                if (!result.hasMatch())
                    continue; // No identifier found.
                identifier = result.captured(QLatin1String("identifier"));

                if (line.size() > 4) // The line is not just opening, try grab the comment.
                    LupdatePrivate::capture(cStyleMultiBegin, line, &identifier, &comment);
                sourceIdentifier = (identifier == QLatin1String("%"));
            }
        } else {
            if (line.endsWith(QLatin1String("*/"))) {
                sawStarPrefix = false; // Finished processing a multi line C style comment.
                line = line.remove(QLatin1String("*/")).trimmed(); // Still there can be something.
            }

            if (sourceIdentifier) {
                line = LupdatePrivate::cleanQuote(line.toStdString(),
                    LupdatePrivate::QuoteCompulsary::Left);
            }

            if (!line.isEmpty() && !comment.isEmpty() && !sourceIdentifier)
                comment.append(QLatin1Char(' '));

            comment += line;
            save = !sawStarPrefix && !comment.isEmpty();
        }

        if (!save)
            continue;

        if (identifier == QStringLiteral(":")) {
            if (!store->lupdateExtraComment.isEmpty())
                store->lupdateExtraComment.append(QLatin1Char(' '));
            store->lupdateExtraComment += comment;
        } else if (identifier == QStringLiteral("=")) {
            if (!store->lupdateIdMetaData.isEmpty())
                store->lupdateIdMetaData.append(QLatin1Char(' '));
            store->lupdateIdMetaData = comment; // Only the last one is to be picked up.
        } else if (identifier == QStringLiteral("~")) {
            auto first = comment.section(isSpace, 0, 0);
            auto second = comment.mid(first.size()).trimmed();
            if (!second.isEmpty())
                store->lupdateAllMagicMetaData.insert(first, second);
        } else if (identifier == QLatin1String("%")) {
            store->lupdateSourceWhenId += comment;
        }

        save = false;
        comment.clear();
        identifier.clear();
    }
}

/*
    Fill the Translator with the retrieved information after traversing the AST.
*/
void LupdateVisitor::fillTranslator()
{
    for (const auto &store : qAsConst(m_translationStoresFromAST))
        fillTranslator(store);
    // Here also need to fill the translator with the information retrieved from the PreProcessor
}

void LupdateVisitor::fillTranslator(TranslationRelatedStore store)
{
    bool forcePlural = false;
    switch (trFunctionAliasManager.trFunctionByName(store.funcName)) {
    case TrFunctionAliasManager::Function_Q_DECLARE_TR_FUNCTIONS:
        // If there is a Q_DECLARE_TR_FUNCTION the context given takes priority
        // over the retrieved context.
        // The retrieved context for Q_DECLARE_TR_FUNCTION (where the macro was)
        // has to fit the start of the retrieved context of the tr function or
        // NOOP macro if there is already a argument giving the context, it has
        // priority.
        //handleDeclareTrFunctions(); // TODO: Implement.
        break;
    case TrFunctionAliasManager::Function_QT_TR_N_NOOP:
        forcePlural = true;
        Q_FALLTHROUGH();
    case TrFunctionAliasManager::Function_tr:
    case TrFunctionAliasManager::Function_trUtf8:
    case TrFunctionAliasManager::Function_QT_TR_NOOP:
    case TrFunctionAliasManager::Function_QT_TR_NOOP_UTF8:
        handleTr(store, forcePlural);
        break;
    case TrFunctionAliasManager::Function_QT_TRANSLATE_N_NOOP:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_N_NOOP3:
        forcePlural = true;
        Q_FALLTHROUGH();
    case TrFunctionAliasManager::Function_translate:
    case TrFunctionAliasManager::Function_findMessage:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_NOOP:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_NOOP_UTF8:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_NOOP3:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_NOOP3_UTF8:
        handleTranslate(store, forcePlural);
        break;
    case TrFunctionAliasManager::Function_QT_TRID_N_NOOP:
        forcePlural = true;
        Q_FALLTHROUGH();
    case TrFunctionAliasManager::Function_qtTrId:
    case TrFunctionAliasManager::Function_QT_TRID_NOOP:
        handleTrId(store, forcePlural);
        break;
    }
}

TranslatorMessage LupdateVisitor::fillTranslatorMessage(const TranslationRelatedStore &store,
    bool forcePlural, bool isId)
{
    QString context;
    if (!isId) {
        context = ParserTool::transcode(store.contextArg.isEmpty() ? store.contextRetrieved
            : store.contextArg);
    }

    TranslatorMessage msg(context,
                          ParserTool::transcode(isId ? store.lupdateSourceWhenId
                                                     : store.lupdateSource),
                          ParserTool::transcode(store.lupdateComment),
                          QString(),
                          store.lupdateLocationFile,
                          store.lupdateLocationLine,
                          QStringList(),
                          TranslatorMessage::Type::Unfinished,
                          (forcePlural ? forcePlural : !store.lupdatePlural.isEmpty()));

    if (!store.lupdateAllMagicMetaData.empty())
        msg.setExtras(store.lupdateAllMagicMetaData);
    msg.setExtraComment(ParserTool::transcode(store.lupdateExtraComment));
    return msg;
}

void LupdateVisitor::handleTranslate(const TranslationRelatedStore &store, bool forcePlural)
{
    if (!store.lupdateSourceWhenId.isEmpty())
        qCDebug(lcClang) << "//% is ignored when using translate function\n";

    TranslatorMessage msg = fillTranslatorMessage(store, forcePlural);
    msg.setId(ParserTool::transcode(store.lupdateIdMetaData)); // //= NOT to be used with qTrId
    m_tor->append(msg);
}

void LupdateVisitor::handleTr(const TranslationRelatedStore &store, bool forcePlural)
{
    if (!store.lupdateSourceWhenId.isEmpty())
        qCDebug(lcClang) << "//% is ignored when using tr function\n";
    if (store.contextRetrieved.isEmpty() && store.contextArg.isEmpty()) {
        qCDebug(lcClang) << "tr() cannot be called without context \n";
        return;
    }

    TranslatorMessage msg = fillTranslatorMessage(store, forcePlural);
    msg.setId(ParserTool::transcode(store.lupdateIdMetaData)); // //= NOT to be used with qTrId
    m_tor->append(msg);
}

void LupdateVisitor::handleTrId(const TranslationRelatedStore &store, bool forcePlural)
{
    if (!store.lupdateIdMetaData.isEmpty())
        qCDebug(lcClang) << "//= is ignored when using qtTrId function \n";

    TranslatorMessage msg = fillTranslatorMessage(store, forcePlural, true);
    msg.setId(ParserTool::transcode(store.lupdateId));
    m_tor->append(msg);
}

void LupdateVisitor::processPreprocessorCalls()
{
    for (const auto &store : qAsConst(m_translationStoresFromPP))
        processPreprocessorCall(store);
}

void LupdateVisitor::processPreprocessorCall(TranslationRelatedStore store)
{
    const std::vector<QString> rawComments = rawCommentsFromSourceLocation(store.callLocation);
    for (const auto &rawComment : rawComments)
        setInfoFromRawComment(rawComment, &store);

    if (store.isValid()) {
        if (store.funcName.contains(QStringLiteral("Q_DECLARE_TR_FUNCTIONS")))
            m_qDeclateTrFunctionContext.push_back(store);
        else
            m_noopTranslationStores.push_back(store);
        store.printStore();
    }
}

void LupdatePPCallbacks::MacroExpands(const clang::Token &macroNameTok,
    const clang::MacroDefinition &macroDefinition, clang::SourceRange range,
    const clang::MacroArgs *args)
{
    if (!args)
        return;
    const auto &sm = m_preprocessor.getSourceManager();
    llvm::StringRef fileName = sm.getFilename(range.getBegin());
    if (fileName != m_inputFile)
        return;

    const QString funcName = QString::fromStdString(m_preprocessor.getSpelling(macroNameTok));
    qCDebug(lcClang) << "func  Name " << funcName;
    if (!funcName.contains(QStringLiteral("NOOP"))
        && !funcName.contains(QStringLiteral("Q_DECLARE_TR_FUNCTIONS"))) {
        return;
    }

    TranslationRelatedStore store;
    store.callType = QStringLiteral("MacroExpands");
    store.funcName = funcName;
    store.lupdateLocationFile = QString::fromStdString(fileName);
    store.lupdateLocationLine = sm.getExpansionLineNumber(range.getBegin());
    store.locationCol = sm.getExpansionColumnNumber(range.getBegin());
    store.callLocation = range.getBegin();

    std::vector<QString> arguments(args->getNumMacroArguments());
    for (unsigned i = 0; i < args->getNumMacroArguments(); i++) {
        auto preExpArguments = const_cast<clang::MacroArgs*>(args)->getPreExpArgument(i,
            m_preprocessor);
        QString temp;
        for (const auto &preExpArgument : preExpArguments) {
            const auto kind = preExpArgument.getKind();
            if (kind == clang::tok::TokenKind::identifier)
                temp = QString::fromStdString(m_preprocessor.getSpelling(preExpArgument));
            else if (clang::tok::isStringLiteral(kind))
                temp += LupdatePrivate::cleanQuote(m_preprocessor.getSpelling(preExpArgument));
        }
        arguments[i] = temp;
    }
    storeMacroArguments(arguments, &store);
    if (store.isValid())
        m_translationStores.push_back(store);
}

void LupdatePPCallbacks::storeMacroArguments(const std::vector<QString> &args,
    TranslationRelatedStore *store)
{
    switch (trFunctionAliasManager.trFunctionByName(store->funcName)) {
    // only one argument: the context with no "
    case TrFunctionAliasManager::Function_Q_DECLARE_TR_FUNCTIONS:
        if (args.size() != 1)
            break;
        store->contextArg = args[0];
        break;
    // only one argument: the source
    case TrFunctionAliasManager::Function_QT_TR_N_NOOP:
        Q_FALLTHROUGH();
    case TrFunctionAliasManager::Function_QT_TR_NOOP:
    case TrFunctionAliasManager::Function_QT_TR_NOOP_UTF8:
        if (args.size() != 1)
            break;
        store->lupdateSource = args[0];
        break;
    // two arguments: the context and the source
    case TrFunctionAliasManager::Function_QT_TRANSLATE_N_NOOP:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_N_NOOP3:
        Q_FALLTHROUGH();
    case TrFunctionAliasManager::Function_QT_TRANSLATE_NOOP:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_NOOP_UTF8:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_NOOP3:
    case TrFunctionAliasManager::Function_QT_TRANSLATE_NOOP3_UTF8:
        if (args.size() != 2)
            break;
        store->contextArg = args[0];
        store->lupdateSource = args[1];
        break;
    // only one argument (?) the message Id
    case TrFunctionAliasManager::Function_QT_TRID_N_NOOP:
        Q_FALLTHROUGH();
    case TrFunctionAliasManager::Function_qtTrId:
    case TrFunctionAliasManager::Function_QT_TRID_NOOP:
        if (args.size() != 1)
            break;
        store->lupdateId = args[0];
        break;
    }
}

QT_END_NAMESPACE
