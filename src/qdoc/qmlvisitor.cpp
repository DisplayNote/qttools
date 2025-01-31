/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the tools applications of the Qt Toolkit.
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

#include "qmlvisitor.h"

#include "codechunk.h"
#include "codeparser.h"
#include "node.h"
#include "qdocdatabase.h"
#include "tokenizer.h"

#include <QtCore/qdebug.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qglobal.h>
#include <QtCore/qstringlist.h>

#ifndef QT_NO_DECLARATIVE
#include <private/qqmljsast_p.h>
#include <private/qqmljsastfwd_p.h>
#include <private/qqmljsengine_p.h>
#endif

QT_BEGIN_NAMESPACE

#ifndef QT_NO_DECLARATIVE
/*!
  The constructor stores all the parameters in local data members.
 */
QmlDocVisitor::QmlDocVisitor(const QString &filePath,
                             const QString &code,
                             QQmlJS::Engine *engine,
                             const QSet<QString> &commands,
                             const QSet<QString> &topics)
    : nestingLevel(0)
{
    lastEndOffset = 0;
    this->filePath_ = filePath;
    this->name = QFileInfo(filePath).baseName();
    document = code;
    this->engine = engine;
    this->commands_ = commands;
    this->topics_ = topics;
    current = QDocDatabase::qdocDB()->primaryTreeRoot();
}

/*!
  The destructor does nothing.
 */
QmlDocVisitor::~QmlDocVisitor()
{
    // nothing.
}

/*!
  Returns the location of the nearest comment above the \a offset.
 */
QQmlJS::AST::SourceLocation QmlDocVisitor::precedingComment(quint32 offset) const
{
    const auto comments = engine->comments();
    for (auto it = comments.rbegin(), end = comments.rend(); it != end; ++it) {
        QQmlJS::AST::SourceLocation loc = *it;

        if (loc.begin() <= lastEndOffset) {
            // Return if we reach the end of the preceding structure.
            break;
        }
        else if (usedComments.contains(loc.begin())) {
            // Return if we encounter a previously used comment.
            break;
        }
        else if (loc.begin() > lastEndOffset && loc.end() < offset) {
            // Only examine multiline comments in order to avoid snippet markers.
            if (document.at(loc.offset - 1) == QLatin1Char('*')) {
                QString comment = document.mid(loc.offset, loc.length);
                if (comment.startsWith(QLatin1Char('!')) || comment.startsWith(QLatin1Char('*'))) {
                    return loc;
                }
            }
        }
    }

    return QQmlJS::AST::SourceLocation();
}

class QmlSignatureParser
{
  public:
    QmlSignatureParser(FunctionNode *func, const QString &signature, const Location &loc);
    void readToken() { tok_ = tokenizer_->getToken(); }
    QString lexeme() { return tokenizer_->lexeme(); }
    QString previousLexeme() { return tokenizer_->previousLexeme(); }

    bool match(int target);
    bool matchTypeAndName(CodeChunk *type, QString *var);
    bool matchParameter();
    bool matchFunctionDecl();

  private:
    QString signature_;
    QStringList names_;
    QString funcName_;
    Tokenizer *tokenizer_;
    int tok_;
    FunctionNode *func_;
    const Location &location_;
};

/*!
  Finds the nearest unused qdoc comment above the QML entity
  represented by the \a node and processes the qdoc commands
  in that comment. The processed documentation is stored in
  the \a node.

  If a qdoc comment is found for \a location, true is returned.
  If a comment is not found there, false is returned.
 */
bool QmlDocVisitor::applyDocumentation(QQmlJS::AST::SourceLocation location, Node *node)
{
    QQmlJS::AST::SourceLocation loc = precedingComment(location.begin());

    if (loc.isValid()) {
        QString source = document.mid(loc.offset, loc.length);
        Location start(filePath_);
        start.setLineNo(loc.startLine);
        start.setColumnNo(loc.startColumn);
        Location finish(filePath_);
        finish.setLineNo(loc.startLine);
        finish.setColumnNo(loc.startColumn);

        Doc doc(start, finish, source.mid(1), commands_, topics_);
        const TopicList &topicsUsed = doc.topicsUsed();
        NodeList nodes;
        Node *nodePassedIn = node;
        Aggregate *parent = nodePassedIn->parent();
        node->setDoc(doc);
        nodes.append(node);
        if (topicsUsed.size() > 0) {
            for (int i=0; i<topicsUsed.size(); ++i) {
                QString topic = topicsUsed.at(i).topic;
                if (!topic.startsWith(QLatin1String("qml")) && !topic.startsWith(QLatin1String("js")))
                    continue; // maybe a qdoc warning here? mws 18/07/18
                QString args = topicsUsed.at(i).args;
                if (topic == COMMAND_JSTYPE) {
                    node->changeType(Node::QmlType, Node::JsType);
                } else if (topic.endsWith(QLatin1String("property"))) {
                    QmlPropArgs qpa;
                    if (splitQmlPropertyArg(doc, args, qpa)) {
                        if (qpa.name_ == nodePassedIn->name()) {
                            if (nodePassedIn->isAlias())
                                nodePassedIn->setDataType(qpa.type_);
                        }
                        else {
                            bool isAttached = topic.contains(QLatin1String("attached"));
                            QmlPropertyNode *n = parent->hasQmlProperty(qpa.name_, isAttached);
                            if (n == nullptr)
                                n = new QmlPropertyNode(parent, qpa.name_, qpa.type_, isAttached);
                            n->setLocation(doc.location());
                            n->setDoc(doc);
                            n->markReadOnly(nodePassedIn->isReadOnly());
                            if (nodePassedIn->isDefault())
                                n->markDefault();
                            if (isAttached)
                                n->markReadOnly(0);
                            if ((topic == COMMAND_JSPROPERTY) || (topic == COMMAND_JSATTACHEDPROPERTY))
                                n->changeType(Node::QmlProperty, Node::JsProperty);
                            nodes.append(n);
                        }
                    }
                    else
                        qDebug() << "  FAILED TO PARSE QML OR JS PROPERTY:" << topic << args;
                } else if (topic.endsWith(QLatin1String("method"))) {
                    if (node->isFunction()) {
                        FunctionNode *fn = static_cast<FunctionNode *>(node);
                        QmlSignatureParser qsp(fn, args, doc.location());
                        if (topic == COMMAND_JSMETHOD || topic == COMMAND_JSATTACHEDMETHOD)
                            fn->changeMetaness(FunctionNode::QmlMethod, FunctionNode::JsMethod);
                    }
                }
            }
        }
        for (int i=0; i<nodes.size(); ++i)
            applyMetacommands(loc, nodes.at(i), doc);
        usedComments.insert(loc.offset);
        if (doc.isEmpty()) {
            return false;
        }
        return true;
    }
    Location codeLoc(filePath_);
    codeLoc.setLineNo(location.startLine);
    node->setLocation(codeLoc);
    return false;
}

QmlSignatureParser::QmlSignatureParser(FunctionNode *func, const QString &signature, const Location &loc)
    : signature_(signature), func_(func), location_(loc)
{
    QByteArray latin1 = signature.toLatin1();
    Tokenizer stringTokenizer(location_, latin1);
    stringTokenizer.setParsingFnOrMacro(true);
    tokenizer_ = &stringTokenizer;
    readToken();
    matchFunctionDecl();
}

/*!
  If the current token matches \a target, read the next
  token and return true. Otherwise, don't read the next
  token, and return false.
 */
bool QmlSignatureParser::match(int target)
{
    if (tok_ == target) {
        readToken();
        return true;
    }
    return false;
}

/*!
  Parse a QML data type into \a type and an optional
  variable name into \a var.
 */
bool QmlSignatureParser::matchTypeAndName(CodeChunk *type, QString *var)
{
    /*
      This code is really hard to follow... sorry. The loop is there to match
      Alpha::Beta::Gamma::...::Omega.
     */
    for (;;) {
        bool virgin = true;

        if (tok_ != Tok_Ident) {
            while (match(Tok_signed) ||
                   match(Tok_unsigned) ||
                   match(Tok_short) ||
                   match(Tok_long) ||
                   match(Tok_int64)) {
                type->append(previousLexeme());
                virgin = false;
            }
        }

        if (virgin) {
            if (match(Tok_Ident)) {
                type->append(previousLexeme());
            }
            else if (match(Tok_void) ||
                     match(Tok_int) ||
                     match(Tok_char) ||
                     match(Tok_double) ||
                     match(Tok_Ellipsis))
                type->append(previousLexeme());
            else
                return false;
        }
        else if (match(Tok_int) ||
                 match(Tok_char) ||
                 match(Tok_double)) {
            type->append(previousLexeme());
        }

        if (match(Tok_Gulbrandsen))
            type->append(previousLexeme());
        else
            break;
    }

    while (match(Tok_Ampersand) ||
           match(Tok_Aster) ||
           match(Tok_const) ||
           match(Tok_Caret))
        type->append(previousLexeme());

    /*
      The usual case: Look for an optional identifier, then for
      some array brackets.
     */
    type->appendHotspot();

    if ((var != nullptr) && match(Tok_Ident))
        *var = previousLexeme();

    if (tok_ == Tok_LeftBracket) {
        int bracketDepth0 = tokenizer_->bracketDepth();
        while ((tokenizer_->bracketDepth() >= bracketDepth0 && tok_ != Tok_Eoi) ||
               tok_ == Tok_RightBracket) {
            type->append(lexeme());
            readToken();
        }
    }
    return true;
}

bool QmlSignatureParser::matchParameter()
{
    QString name;
    CodeChunk type;
    CodeChunk defaultValue;

    bool result = matchTypeAndName(&type, &name);
    if (name.isEmpty()) {
        name = type.toString();
        type.clear();
    }

    if (!result)
        return false;
    if (match(Tok_Equal)) {
        int parenDepth0 = tokenizer_->parenDepth();
        while (tokenizer_->parenDepth() >= parenDepth0 &&
               (tok_ != Tok_Comma ||
                tokenizer_->parenDepth() > parenDepth0) &&
               tok_ != Tok_Eoi) {
            defaultValue.append(lexeme());
            readToken();
        }
    }
    func_->parameters().append(type.toString(), name, defaultValue.toString());
    return true;
}

bool QmlSignatureParser::matchFunctionDecl()
{
    CodeChunk returnType;

    int firstBlank = signature_.indexOf(QChar(' '));
    int leftParen = signature_.indexOf(QChar('('));
    if ((firstBlank > 0) && (leftParen - firstBlank) > 1) {
        if (!matchTypeAndName(&returnType, nullptr))
            return false;
    }

    while (match(Tok_Ident)) {
        names_.append(previousLexeme());
        if (!match(Tok_Gulbrandsen)) {
            funcName_ = previousLexeme();
            names_.pop_back();
            break;
        }
    }

    if (tok_ != Tok_LeftParen)
        return false;
    /*
      Parsing the parameters should be moved into class Parameters,
      but it can wait. mws 14/12/2018
     */
    readToken();

    func_->setLocation(location_);
    func_->setReturnType(returnType.toString());

    if (tok_ != Tok_RightParen) {
        func_->parameters().clear();
        do {
            if (!matchParameter())
                return false;
        } while (match(Tok_Comma));
    }
    if (!match(Tok_RightParen))
        return false;
    return true;
}

/*!
  A QML property argument has the form...

  <type> <component>::<name>
  <type> <QML-module>::<component>::<name>

  This function splits the argument into one of those
  two forms. The three part form is the old form, which
  was used before the creation of QtQuick 2 and Qt
  Components. A <QML-module> is the QML equivalent of a
  C++ namespace. So this function splits \a arg on "::"
  and stores the parts in the \e {type}, \e {module},
  \e {component}, and \a {name}, fields of \a qpa. If it
  is successful, it returns \c true. If not enough parts
  are found, a qdoc warning is emitted and false is
  returned.
 */
bool QmlDocVisitor::splitQmlPropertyArg(const Doc &doc,
                                        const QString &arg,
                                        QmlPropArgs& qpa)
{
    qpa.clear();
    QStringList blankSplit = arg.split(QLatin1Char(' '));
    if (blankSplit.size() > 1) {
        qpa.type_ = blankSplit[0];
        QStringList colonSplit(blankSplit[1].split("::"));
        if (colonSplit.size() == 3) {
            qpa.module_ = colonSplit[0];
            qpa.component_ = colonSplit[1];
            qpa.name_ = colonSplit[2];
            return true;
        }
        else if (colonSplit.size() == 2) {
            qpa.component_ = colonSplit[0];
            qpa.name_ = colonSplit[1];
            return true;
        }
        else if (colonSplit.size() == 1) {
            qpa.name_ = colonSplit[0];
            return true;
        }
        QString msg = "Unrecognizable QML module/component qualifier for " + arg;
        doc.location().warning(tr(msg.toLatin1().data()));
    }
    else {
        QString msg = "Missing property type for " + arg;
        doc.location().warning(tr(msg.toLatin1().data()));
    }
    return false;
}

/*!
  Applies the metacommands found in the comment.
 */
void QmlDocVisitor::applyMetacommands(QQmlJS::AST::SourceLocation,
                                      Node *node,
                                      Doc &doc)
{
    QDocDatabase *qdb = QDocDatabase::qdocDB();
    QSet<QString> metacommands = doc.metaCommandsUsed();
    if (metacommands.count() > 0) {
        metacommands.subtract(topics_);
        QSet<QString>::iterator i = metacommands.begin();
        while (i != metacommands.end()) {
            QString command = *i;
            ArgList args = doc.metaCommandArgs(command);
            if ((command == COMMAND_QMLABSTRACT) || (command == COMMAND_ABSTRACT)) {
                if (node->isQmlType() || node->isJsType()) {
                    node->setAbstract(true);
                }
            }
            else if (command == COMMAND_DEPRECATED) {
                node->setStatus(Node::Obsolete);
            }
            else if ((command == COMMAND_INQMLMODULE) || (command == COMMAND_INJSMODULE)) {
                qdb->addToQmlModule(args[0].first,node);
            }
            else if (command == COMMAND_QMLINHERITS) {
                if (node->name() == args[0].first)
                    doc.location().warning(tr("%1 tries to inherit itself").arg(args[0].first));
                else if (node->isQmlType() || node->isJsType()) {
                    QmlTypeNode *qmlType = static_cast<QmlTypeNode *>(node);
                    qmlType->setQmlBaseName(args[0].first);
                }
            }
            else if (command == COMMAND_QMLDEFAULT) {
                node->markDefault();
            }
            else if (command == COMMAND_QMLREADONLY) {
                node->markReadOnly(1);
            }
            else if ((command == COMMAND_INGROUP) && !args.isEmpty()) {
                ArgList::ConstIterator argsIter = args.constBegin();
                while (argsIter != args.constEnd()) {
                    QDocDatabase::qdocDB()->addToGroup(argsIter->first, node);
                    ++argsIter;
                }
            }
            else if (command == COMMAND_INTERNAL) {
                node->setStatus(Node::Internal);
            }
            else if (command == COMMAND_OBSOLETE) {
                node->setStatus(Node::Obsolete);
            }
            else if (command == COMMAND_PAGEKEYWORDS) {
                // Not done yet. Do we need this?
            }
            else if (command == COMMAND_PRELIMINARY) {
                node->setStatus(Node::Preliminary);
            }
            else if (command == COMMAND_SINCE) {
                QString arg = args[0].first; //.join(' ');
                node->setSince(arg);
            }
            else if (command == COMMAND_WRAPPER) {
                node->setWrapper();
            }
            else {
                doc.location().warning(tr("The \\%1 command is ignored in QML files").arg(command));
            }
            ++i;
        }
    }
}

/*!
  Reconstruct the qualified \a id using dot notation
  and return the fully qualified string.
 */
QString QmlDocVisitor::getFullyQualifiedId(QQmlJS::AST::UiQualifiedId *id)
{
    QString result;
    if (id) {
        result = id->name.toString();
        id = id->next;
        while (id != nullptr) {
            result += QChar('.') + id->name.toString();
            id = id->next;
        }
    }
    return result;
}

/*!
  Begin the visit of the object \a definition, recording it in the
  qdoc database. Increment the object nesting level, which is used
  to test whether we are at the public API level. The public level
  is level 1.

  Note that this visit() function creates the qdoc object node as a
  QmlType. If it is actually a JsType, this fact is discovered when
  the qdoc comment is applied to the node. The node's typet is then
  changed to JsType.
 */
bool QmlDocVisitor::visit(QQmlJS::AST::UiObjectDefinition *definition)
{
    QString type = getFullyQualifiedId(definition->qualifiedTypeNameId);
    nestingLevel++;

    if (current->isNamespace()) {
        QmlTypeNode *component = nullptr;
        Node *candidate = current ->findChildNode(name, Node::QML);
        if (candidate != nullptr)
            component = static_cast<QmlTypeNode *>(candidate);
        else
            component = new QmlTypeNode(current, name);
        component->setTitle(name);
        component->setImportList(importList);
        importList.clear();
        if (applyDocumentation(definition->firstSourceLocation(), component))
            component->setQmlBaseName(type);
        current = component;
    }

    return true;
}

/*!
  End the visit of the object \a definition. In particular,
  decrement the object nesting level, which is used to test
  whether we are at the public API level. The public API
  level is level 1. It won't decrement below 0.
 */
void QmlDocVisitor::endVisit(QQmlJS::AST::UiObjectDefinition *definition)
{
    if (nestingLevel > 0) {
        --nestingLevel;
    }
    lastEndOffset = definition->lastSourceLocation().end();
}

bool QmlDocVisitor::visit(QQmlJS::AST::UiImport *import)
{
    QString name = document.mid(import->fileNameToken.offset, import->fileNameToken.length);
    if (name[0] == '\"')
        name = name.mid(1, name.length()-2);
    QString version;
    if (import->version) {
        const auto start = import->version->firstSourceLocation().begin();
        const auto end = import->version->lastSourceLocation().end();
        version = document.mid(start, end - start);
    }
    QString importId = document.mid(import->importIdToken.offset, import->importIdToken.length);
    QString importUri = getFullyQualifiedId(import->importUri);
    importList.append(ImportRec(name, version, importId, importUri));

    return true;
}

void QmlDocVisitor::endVisit(QQmlJS::AST::UiImport *definition)
{
    lastEndOffset = definition->lastSourceLocation().end();
}

bool QmlDocVisitor::visit(QQmlJS::AST::UiObjectBinding *)
{
    ++nestingLevel;
    return true;
}

void QmlDocVisitor::endVisit(QQmlJS::AST::UiObjectBinding *)
{
    --nestingLevel;
}

bool QmlDocVisitor::visit(QQmlJS::AST::UiArrayBinding *)
{
    return true;
}

void QmlDocVisitor::endVisit(QQmlJS::AST::UiArrayBinding *)
{
}

template <typename T>
QString qualifiedIdToString(T node);

template <>
QString qualifiedIdToString(QStringRef node)
{
    return node.toString();
}

template <>
QString qualifiedIdToString(QQmlJS::AST::UiQualifiedId *node)
{
    QString s;

    for (QQmlJS::AST::UiQualifiedId *it = node; it; it = it->next) {
        s.append(it->name);

        if (it->next)
            s.append(QLatin1Char('.'));
    }

    return s;
}

/*!
    Visits the public \a member declaration, which can be a
    signal or a property. It is a custom signal or property.
    Only visit the \a member if the nestingLevel is 1.
 */
bool QmlDocVisitor::visit(QQmlJS::AST::UiPublicMember *member)
{
    if (nestingLevel > 1) {
        return true;
    }
    switch (member->type) {
    case QQmlJS::AST::UiPublicMember::Signal:
    {
        if (current->isQmlType() || current->isJsType()) {
            QmlTypeNode *qmlType = static_cast<QmlTypeNode *>(current);
            if (qmlType) {
                FunctionNode::Metaness metaness = FunctionNode::QmlSignal;
                if (qmlType->isJsType())
                    metaness = FunctionNode::JsSignal;
                QString name = member->name.toString();
                FunctionNode *newSignal = new FunctionNode(metaness, current, name);
                Parameters &parameters = newSignal->parameters();
                for (QQmlJS::AST::UiParameterList *it = member->parameters; it; it = it->next) {
                    const QString type = qualifiedIdToString(it->type);
                    if (!type.isEmpty() && !it->name.isEmpty())
                        parameters.append(type, QString(), it->name.toString());
                }
                applyDocumentation(member->firstSourceLocation(), newSignal);
            }
        }
        break;
    }
    case QQmlJS::AST::UiPublicMember::Property:
    {
        QString type = qualifiedIdToString(member->memberType);
        QString name = member->name.toString();
        if (current->isQmlType() || current->isJsType()) {
            QmlTypeNode *qmlType = static_cast<QmlTypeNode *>(current);
            if (qmlType) {
                QString name = member->name.toString();
                QmlPropertyNode *qmlPropNode = qmlType->hasQmlProperty(name);
                if (qmlPropNode == nullptr)
                    qmlPropNode = new QmlPropertyNode(qmlType, name, type, false);
                qmlPropNode->markReadOnly(member->isReadonlyMember);
                if (member->isDefaultMember)
                    qmlPropNode->markDefault();
                applyDocumentation(member->firstSourceLocation(), qmlPropNode);
            }
        }
        break;
    }
    default:
        return false;
    }

    return true;
}

/*!
  End the visit of the \a member.
 */
void QmlDocVisitor::endVisit(QQmlJS::AST::UiPublicMember* member)
{
    lastEndOffset = member->lastSourceLocation().end();
}

bool QmlDocVisitor::visit(QQmlJS::AST::IdentifierPropertyName *)
{
    return true;
}

/*!
  Begin the visit of the function declaration \a fd, but only
  if the nesting level is 1.
 */
bool QmlDocVisitor::visit(QQmlJS::AST::FunctionDeclaration* fd)
{
    if (nestingLevel <= 1) {
        FunctionNode::Metaness metaness = FunctionNode::QmlMethod;
        if (current->isJsType())
            metaness = FunctionNode::JsMethod;
        else if (!current->isQmlType())
            return true;
        QString name = fd->name.toString();
        FunctionNode *method = new FunctionNode(metaness, current, name);
        Parameters &parameters = method->parameters();
        QQmlJS::AST::FormalParameterList *formals = fd->formals;
        if (formals) {
            QQmlJS::AST::FormalParameterList *fp = formals;
            do {
                parameters.append(QString(), QString(), fp->element->bindingIdentifier.toString());
                fp = fp->next;
            } while (fp && fp != formals);
        }
        applyDocumentation(fd->firstSourceLocation(), method);
    }
    return true;
}

/*!
  End the visit of the function declaration, \a fd.
 */
void QmlDocVisitor::endVisit(QQmlJS::AST::FunctionDeclaration* fd)
{
    lastEndOffset = fd->lastSourceLocation().end();
}

/*!
  Begin the visit of the signal handler declaration \a sb, but only
  if the nesting level is 1.

  This visit is now deprecated. It has been decided to document
  public signals. If a signal handler must be discussed in the
  documentation, that discussion must take place in the comment
  for the signal.
 */
bool QmlDocVisitor::visit(QQmlJS::AST::UiScriptBinding* )
{
    return true;
}

void QmlDocVisitor::endVisit(QQmlJS::AST::UiScriptBinding* sb)
{
    lastEndOffset = sb->lastSourceLocation().end();
}

bool QmlDocVisitor::visit(QQmlJS::AST::UiQualifiedId* )
{
    return true;
}

void QmlDocVisitor::endVisit(QQmlJS::AST::UiQualifiedId* )
{
    // nothing.
}

void QmlDocVisitor::throwRecursionDepthError()
{
    hasRecursionDepthError = true;
}

bool QmlDocVisitor::hasError() const
{
    return hasRecursionDepthError;
}

#endif

QT_END_NAMESPACE
