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

#include "qdoctagfiles.h"

#include "atom.h"
#include "doc.h"
#include "htmlgenerator.h"
#include "location.h"
#include "node.h"
#include "qdocdatabase.h"
#include "text.h"

#include <QtCore/qdebug.h>

#include <limits.h>

QT_BEGIN_NAMESPACE

/*!
  \class QDocTagFiles

  This class handles the generation of the qdoc tag file.
 */

QDocTagFiles *QDocTagFiles::qdocTagFiles_ = nullptr;

/*!
  Constructs the singleton. \a qdb is the pointer to the
  qdoc database that is used when reading and writing the
  index files.
 */
QDocTagFiles::QDocTagFiles()
{
    qdb_ = QDocDatabase::qdocDB();
}

/*!
  Destroys the singleton QDocTagFiles.
 */
QDocTagFiles::~QDocTagFiles()
{
    qdb_ = nullptr;
}

/*!
  Creates the singleton. Allows only one instance of the class
  to be created. Returns a pointer to the singleton.
 */
QDocTagFiles *QDocTagFiles::qdocTagFiles()
{
   if (qdocTagFiles_ == nullptr)
      qdocTagFiles_ = new QDocTagFiles;
   return qdocTagFiles_;
}

/*!
  Destroys the singleton.
 */
void QDocTagFiles::destroyQDocTagFiles()
{
    if (qdocTagFiles_ != nullptr) {
        delete qdocTagFiles_;
        qdocTagFiles_ = nullptr;
    }
}

/*!
  Generate the tag file section with the given \a writer for the \a parent
  node.
 */
void QDocTagFiles::generateTagFileCompounds(QXmlStreamWriter &writer, const Aggregate *parent)
{
    const auto &nonFunctionList = const_cast<Aggregate *>(parent)->nonfunctionList();
    for (const auto *node : nonFunctionList) {
        if (!node->url().isEmpty() || node->isPrivate())
            continue;

        QString kind;
        switch (node->nodeType()) {
        case Node::Namespace:
            kind = "namespace";
            break;
        case Node::Class:
        case Node::Struct:
        case Node::Union:
        case Node::QmlType:
        case Node::JsType:
            kind = "class";
            break;
        default:
            continue;
        }
        const Aggregate *aggregate = static_cast<const Aggregate *>(node);

        QString access = "public";
        if (node->isProtected())
            access = "protected";

        QString objName = node->name();

        // Special case: only the root node should have an empty name.
        if (objName.isEmpty() && node != qdb_->primaryTreeRoot())
            continue;

        // *** Write the starting tag for the element here. ***
        writer.writeStartElement("compound");
        writer.writeAttribute("kind", kind);

        if (node->isClassNode()) {
            writer.writeTextElement("name", node->fullDocumentName());
            writer.writeTextElement("filename", gen_->fullDocumentLocation(node, false));

            // Classes contain information about their base classes.
            const ClassNode *classNode = static_cast<const ClassNode *>(node);
            const QList<RelatedClass> bases = classNode->baseClasses();
            for (const auto &related : bases) {
                ClassNode *n = related.node_;
                if (n)
                    writer.writeTextElement("base", n->name());
            }

            // Recurse to write all members.
            generateTagFileMembers(writer, aggregate);
            writer.writeEndElement();

            // Recurse to write all compounds.
            generateTagFileCompounds(writer, aggregate);
        } else {
            writer.writeTextElement("name", node->fullDocumentName());
            writer.writeTextElement("filename", gen_->fullDocumentLocation(node, false));

            // Recurse to write all members.
            generateTagFileMembers(writer, aggregate);
            writer.writeEndElement();

            // Recurse to write all compounds.
            generateTagFileCompounds(writer, aggregate);
        }
    }
}

/*!
  Writes all the members of the \a parent node with the \a writer.
  The node represents a C++ class, namespace, etc.
 */
void QDocTagFiles::generateTagFileMembers(QXmlStreamWriter &writer, const Aggregate *parent)
{
    const auto &childNodes = parent->childNodes();
    for (const auto *node : childNodes) {
        if (!node->url().isEmpty())
            continue;

        QString nodeName;
        QString kind;
        switch (node->nodeType()) {
        case Node::Enum:
            nodeName = "member";
            kind = "enum";
            break;
        case Node::Typedef:
            nodeName = "member";
            kind = "typedef";
            break;
        case Node::Property:
            nodeName = "member";
            kind = "property";
            break;
        case Node::Function:
            nodeName = "member";
            kind = "function";
            break;
        case Node::Namespace:
            nodeName = "namespace";
            break;
        case Node::Class:
        case Node::Struct:
        case Node::Union:
            nodeName = "class";
            break;
        case Node::Variable:
        default:
            continue;
        }

        QString access;
        switch (node->access()) {
        case Node::Public:
            access = "public";
            break;
        case Node::Protected:
            access = "protected";
            break;
        case Node::Private:
        default:
            continue;
        }

        QString objName = node->name();

        // Special case: only the root node should have an empty name.
        if (objName.isEmpty() && node != qdb_->primaryTreeRoot())
            continue;

        // *** Write the starting tag for the element here. ***
        writer.writeStartElement(nodeName);
        if (!kind.isEmpty())
            writer.writeAttribute("kind", kind);

        switch (node->nodeType()) {
        case Node::Class:
        case Node::Struct:
        case Node::Union:
            writer.writeCharacters(node->fullDocumentName());
            writer.writeEndElement();
            break;
        case Node::Namespace:
            writer.writeCharacters(node->fullDocumentName());
            writer.writeEndElement();
            break;
        case Node::Function:
            {
                /*
                  Function nodes contain information about
                  the type of function being described.
                */

                const FunctionNode *functionNode = static_cast<const FunctionNode *>(node);
                writer.writeAttribute("protection", access);
                writer.writeAttribute("virtualness", functionNode->virtualness());
                writer.writeAttribute("static", functionNode->isStatic() ? "yes" : "no");

                if (functionNode->isNonvirtual())
                    writer.writeTextElement("type", functionNode->returnType());
                else
                    writer.writeTextElement("type", "virtual " + functionNode->returnType());

                writer.writeTextElement("name", objName);
                QStringList pieces = gen_->fullDocumentLocation(node, false).split(QLatin1Char('#'));
                writer.writeTextElement("anchorfile", pieces[0]);
                writer.writeTextElement("anchor", pieces[1]);
                QString signature = functionNode->signature(false, false);
                signature = signature.mid(signature.indexOf(QChar('('))).trimmed();
                if (functionNode->isConst())
                    signature += " const";
                if (functionNode->isFinal())
                    signature += " final";
                if (functionNode->isOverride())
                    signature += " override";
                if (functionNode->isPureVirtual())
                    signature += " = 0";
                writer.writeTextElement("arglist", signature);
            }
            writer.writeEndElement(); // member
            break;
        case Node::Property:
            {
                const PropertyNode *propertyNode = static_cast<const PropertyNode *>(node);
                writer.writeAttribute("type", propertyNode->dataType());
                writer.writeTextElement("name", objName);
                QStringList pieces = gen_->fullDocumentLocation(node, false).split(QLatin1Char('#'));
                writer.writeTextElement("anchorfile", pieces[0]);
                writer.writeTextElement("anchor", pieces[1]);
                writer.writeTextElement("arglist", QString());
            }
            writer.writeEndElement(); // member
            break;
        case Node::Enum:
            {
                const EnumNode *enumNode = static_cast<const EnumNode *>(node);
                writer.writeTextElement("name", objName);
                QStringList pieces = gen_->fullDocumentLocation(node, false).split(QLatin1Char('#'));
                writer.writeTextElement("anchor", pieces[1]);
                writer.writeTextElement("arglist", QString());
                writer.writeEndElement(); // member

                for (int i = 0; i < enumNode->items().size(); ++i) {
                    EnumItem item = enumNode->items().value(i);
                    writer.writeStartElement("member");
                    writer.writeAttribute("name", item.name());
                    writer.writeTextElement("anchor", pieces[1]);
                    writer.writeTextElement("arglist", QString());
                    writer.writeEndElement(); // member
                }
            }
            break;
        case Node::Typedef:
            {
                const TypedefNode *typedefNode = static_cast<const TypedefNode *>(node);
                if (typedefNode->associatedEnum())
                    writer.writeAttribute("type", typedefNode->associatedEnum()->fullDocumentName());
                else
                    writer.writeAttribute("type", QString());
                writer.writeTextElement("name", objName);
                QStringList pieces = gen_->fullDocumentLocation(node, false).split(QLatin1Char('#'));
                writer.writeTextElement("anchorfile", pieces[0]);
                writer.writeTextElement("anchor", pieces[1]);
                writer.writeTextElement("arglist", QString());
            }
            writer.writeEndElement(); // member
            break;

        case Node::Variable:
        default:
            break;
        }
    }
}

/*!
  Writes a tag file named \a fileName.
 */
void QDocTagFiles::generateTagFile(const QString &fileName, Generator *g)
{
    QFile file(fileName);
    QFileInfo fileInfo(fileName);

    // If no path was specified or it doesn't exist,
    // default to the output directory
    if (fileInfo.fileName() == fileName || !fileInfo.dir().exists())
        file.setFileName(gen_->outputDir() + QLatin1Char('/') +
                         fileInfo.fileName());

    if (!file.open(QFile::WriteOnly | QFile::Text)) {
        Location::null.warning(
            QString("Failed to open %1 for writing.").arg(file.fileName()));
        return;
    }

    gen_ = g;
    QXmlStreamWriter writer(&file);
    writer.setAutoFormatting(true);
    writer.writeStartDocument();
    writer.writeStartElement("tagfile");
    generateTagFileCompounds(writer, qdb_->primaryTreeRoot());
    writer.writeEndElement(); // tagfile
    writer.writeEndDocument();
    file.close();
}

QT_END_NAMESPACE
