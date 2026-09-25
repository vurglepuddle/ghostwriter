/*
 * SPDX-FileCopyrightText: 2026 ghostwriter contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QFile>
#include <QFileSystemModel>
#include <QItemSelectionModel>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "folderviewwidget.h"

using namespace ghostwriter;

class FolderViewTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir settingsDir;
    QTemporaryDir documentsDir;

    QString createFile(const QString &name);

private slots:
    void initTestCase();
    void loadingSelectsDocumentWithoutReopeningIt();
    void userSelectionOpensFile();
};

QString FolderViewTest::createFile(const QString &name)
{
    const QString path = documentsDir.filePath(name);
    QFile file(path);

    if (file.open(QIODevice::WriteOnly)) {
        file.write("# Test\n");
    }

    return path;
}

void FolderViewTest::initTestCase()
{
    QVERIFY(settingsDir.isValid());
    QVERIFY(documentsDir.isValid());

    QCoreApplication::setOrganizationName("ghostwriter");
    QCoreApplication::setApplicationName("folderviewtest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
}

// The view selects the open document's file itself when it loads.  That must
// not be reported as the user choosing the file, which would open the
// document a second time.
void FolderViewTest::loadingSelectsDocumentWithoutReopeningIt()
{
    const QString document = createFile("document.md");
    createFile("other.md");

    FolderViewWidget view;
    QSignalSpy fileSelected(&view, &FolderViewWidget::fileSelected);

    view.reloadFolderViewFromPath(document);

    auto *model = qobject_cast<QFileSystemModel *>(view.model());
    QVERIFY(model);
    QCOMPARE(model->filePath(view.selectionModel()->currentIndex()), document);
    QCOMPARE(fileSelected.count(), 0);
}

void FolderViewTest::userSelectionOpensFile()
{
    const QString document = createFile("current.md");
    const QString other = createFile("chosen.md");

    FolderViewWidget view;
    view.reloadFolderViewFromPath(document);

    QSignalSpy fileSelected(&view, &FolderViewWidget::fileSelected);
    auto *model = qobject_cast<QFileSystemModel *>(view.model());
    QVERIFY(model);

    view.selectionModel()->setCurrentIndex(model->index(other), QItemSelectionModel::SelectCurrent);

    QCOMPARE(fileSelected.count(), 1);
    QCOMPARE(fileSelected.at(0).at(0).toString(), other);
}

QTEST_MAIN(FolderViewTest)
#include "folderviewtest.moc"
