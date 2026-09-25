/*
 * SPDX-FileCopyrightText: 2020-2023 Megan Conkle <megan.conkle@kdemail.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>

#include <QHash>
#include <QStack>
#include <QTextStream>
#include <QVector>
#include <QtGlobal>

#include <3rdparty/cmark-gfm/src/cmark-gfm.h>
#include "markdownast.h"

namespace ghostwriter
{
class MarkdownASTPrivate
{
public:
    MarkdownASTPrivate()
    {
        ;
    }

    ~MarkdownASTPrivate()
    {
        ;
    }

    MemoryArena<MarkdownNode> arena;
    MarkdownNode *root;

    // Children of nodes with many children, for binary searching by line.
    // Only built when the children are strictly ordered, non-overlapping
    // block nodes, so that a search gives the same answer as a linear scan.
    QHash<const MarkdownNode *, QVector<MarkdownNode *>> childIndex;

    void buildChildIndex();
    MarkdownNode *firstChildNear(const MarkdownNode *parent, int lineNumber) const;
};

namespace
{
constexpr int MinIndexedChildren = 8;
}

void MarkdownASTPrivate::buildChildIndex()
{
    childIndex.clear();

    if (nullptr == root) {
        return;
    }

    QStack<const MarkdownNode *> nodes;
    nodes.push(root);

    while (!nodes.isEmpty()) {
        const MarkdownNode *node = nodes.pop();
        QVector<MarkdownNode *> children;
        bool indexable = true;

        for (MarkdownNode *child = node->firstChild(); nullptr != child; child = child->next()) {
            if (child->isBlockType()) {
                nodes.push(child);
            }

            if (!child->isBlockType() || (MarkdownNode::TableCell == child->type()) || (child->endLine() <= 0)
                || (!children.isEmpty() && (children.last()->endLine() >= child->startLine()))) {
                indexable = false;
            }

            children.append(child);
        }

        if (indexable && (children.size() >= MinIndexedChildren)) {
            childIndex.insert(node, children);
        }
    }
}

MarkdownNode *MarkdownASTPrivate::firstChildNear(const MarkdownNode *parent, int lineNumber) const
{
    auto it = childIndex.constFind(parent);

    if (it == childIndex.constEnd()) {
        return parent->firstChild();
    }

    const QVector<MarkdownNode *> &children = it.value();

    // Last child starting at or before the line.  Earlier children end
    // before it starts, so a linear scan would have skipped them anyway.
    auto upper = std::upper_bound(children.cbegin(), children.cend(), lineNumber, [](int line, const MarkdownNode *node) {
        return line < node->startLine();
    });

    if (upper == children.cbegin()) {
        return children.first();
    }

    return *(upper - 1);
}

MarkdownAST::MarkdownAST()
    : d_ptr(new MarkdownASTPrivate())
{
    Q_D(MarkdownAST);
    
    d->root = nullptr;
}

MarkdownAST::MarkdownAST(cmark_node *root)
    : d_ptr(new MarkdownASTPrivate())
{    
    setRoot(root);
}

MarkdownAST::~MarkdownAST()
{
    Q_D(MarkdownAST);
    
    d->arena.freeAll();
    d->root = nullptr;
}

MarkdownNode *MarkdownAST::root()
{
    Q_D(MarkdownAST);
    
    return d->root;
}

void MarkdownAST::setRoot(cmark_node *root)
{
    Q_D(MarkdownAST);
    
    d->arena.freeAll();
    d->childIndex.clear();

    if (nullptr == root) {
        d->root = nullptr;
        return;
    }

    d->root = d->arena.allocate();

    // Clone the node into memory that isn't allocated to
    // cmark-gfm's arena memory.
    QStack<cmark_node *> fromNodes;
    QStack<MarkdownNode *> toNodes;

    d->root->setDataFrom(root);
    fromNodes.push(root);
    toNodes.push(d->root);

    while (!fromNodes.isEmpty()) {
        cmark_node *source = fromNodes.pop();
        MarkdownNode *dest = toNodes.pop();

        // Prep children nodes for cloning.
        MarkdownNode *destParent = dest;
        source = cmark_node_first_child(source);

        while (NULL != source) {
            fromNodes.push(source);
            dest = d->arena.allocate();
            dest->setDataFrom(source);
            destParent->appendChild(dest);
            toNodes.push(dest);
            source = cmark_node_next(source);
        }
    }

    d->buildChildIndex();
}

MarkdownNode *MarkdownAST::findBlockAtLine(int lineNumber) const
{
    Q_D(const MarkdownAST);
    
    if ((nullptr == d->root) || (MarkdownNode::Invalid == d->root->type())) {
        return nullptr;
    }

    MarkdownNode *candidate = nullptr;
    MarkdownNode *current = d->firstChildNear(d->root, lineNumber);

    while
    (
        (nullptr != current)
        && (current->isBlockType())
        && (MarkdownNode::TableCell != current->type())
    ) {
        if
        (
            (current->startLine() <= lineNumber)
            &&
            (
                (lineNumber <= current->endLine())
                || (0 == current->endLine())
            )
        ) {
            candidate = current;

            switch (current->type()) {
            case MarkdownNode::ListItem:
            case MarkdownNode::TaskListItem:
                return candidate;
            case MarkdownNode::Heading: {
                int lineCount = current->endLine() - current->startLine() + 1;

                if (
                    (lineCount > 2) &&
                    (lineNumber == current->endLine())) {
                    current = current->next();
                } else {
                    current = d->firstChildNear(current, lineNumber);
                }
                break;
            }
            default:
                current = d->firstChildNear(current, lineNumber);
                break;
            }
        } else if (current->startLine() > lineNumber) {
            return candidate;
        } else {
            current = current->next();
        }
    }

    return candidate;
}

MarkdownNode *MarkdownAST::topLevelBlockAtLine(int lineNumber) const
{
    Q_D(const MarkdownAST);

    if ((nullptr == d->root) || (MarkdownNode::Invalid == d->root->type())) {
        return nullptr;
    }

    for (MarkdownNode *node = d->firstChildNear(d->root, lineNumber); nullptr != node; node = node->next()) {
        if (node->startLine() > lineNumber) {
            return nullptr;
        }

        if ((lineNumber <= node->endLine()) || (0 == node->endLine())) {
            return node;
        }
    }

    return nullptr;
}

QVector<MarkdownNode *> MarkdownAST::headings() const
{
    Q_D(const MarkdownAST);
    
    QVector<MarkdownNode *> headings;

    if ((nullptr == d->root) || (MarkdownNode::Invalid == d->root->type())) {
        return headings;
    }

    MarkdownNode *node = d->root->firstChild();

    while (nullptr != node) {
        if (MarkdownNode::Heading == node->type()) {
            headings.append(node);
        }

        node = node->next();
    }

    return headings;
}

void MarkdownAST::clear()
{
    Q_D(MarkdownAST);
    
    d->arena.freeAll();
    d->childIndex.clear();
    d->root = nullptr;
}

QString MarkdownAST::toString() const
{
    Q_D(const MarkdownAST);
    
    if (nullptr == d->root) {
        return "AST is empty";
    }

    QString text;
    QTextStream stream(&text);
    QStack<MarkdownNode *> nodes;
    QStack<QString> indentation;

    nodes.push(d->root);
    indentation.push("");

    while (!nodes.empty()) {
        MarkdownNode *node = nodes.pop();
        QString indent = indentation.pop();


#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
        stream << indent << "->" << node->toString() << Qt::endl;
#else
        stream << indent << "->" << node->toString() << endl;
#endif

        MarkdownNode *child = node->lastChild();
        indent += "   ";

        while (nullptr != child) {
            nodes.push(child);
            indentation.push(indent);
            child = child->previous();
        }
    }

    return text;
}
} // namespace ghostwriter
