﻿/* This file is part of the Krita project
 *
 * Copyright (C) 2014 Boudewijn Rempt <boud@kogmbh.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "KisMainWindow.h" // XXX: remove
#include <QMessageBox> // XXX: remove

#include <KisMimeDatabase.h>

#include <KoCanvasBase.h>
#include <KoColor.h>
#include <KoColorProfile.h>
#include <KoColorSpaceEngine.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoDocumentInfoDlg.h>
#include <KoDocumentInfo.h>
#include <KoDpi.h>
#include <KoUnit.h>
#include <KoFileDialog.h>
#include <KoID.h>
#include <KoOdfReadStore.h>
#include <KoProgressProxy.h>
#include <KoProgressUpdater.h>
#include <KoSelection.h>
#include <KoShape.h>
#include <KoShapeController.h>
#include <KoStore.h>
#include <KoUpdater.h>
#include <KoXmlWriter.h>
#include <KoXmlReader.h>
#include <KoStoreDevice.h>

#include <klocalizedstring.h>
#include <kis_debug.h>
#include <kdesktopfile.h>
#include <kconfiggroup.h>
#include <QTemporaryFile>
#include <kbackup.h>

#include <QTextBrowser>
#include <QApplication>
#include <QBuffer>
#include <QDesktopServices>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QRect>
#include <QScopedPointer>
#include <QSize>
#include <QStringList>
#include <QtGlobal>
#include <QTimer>
#include <QWidget>

// Krita Image
#include <kis_config.h>
#include <flake/kis_shape_layer.h>
#include <kis_debug.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_layer.h>
#include <kis_name_server.h>
#include <kis_paint_layer.h>
#include <kis_painter.h>
#include <kis_selection.h>
#include <kis_fill_painter.h>
#include <kis_document_undo_store.h>
#include <kis_painting_assistants_decoration.h>
#include <kis_idle_watcher.h>
#include <kis_signal_auto_connection.h>
#include <kis_debug.h>
#include <kis_canvas_widget_base.h>

// Local
#include "KisViewManager.h"
#include "kis_clipboard.h"
#include "widgets/kis_custom_image_widget.h"
#include "canvas/kis_canvas2.h"
#include "flake/kis_shape_controller.h"
#include "kis_statusbar.h"
#include "widgets/kis_progress_widget.h"
#include "kis_canvas_resource_provider.h"
#include "kis_resource_server_provider.h"
#include "kis_node_manager.h"
#include "KisPart.h"
#include "KisApplication.h"
#include "KisDocument.h"
#include "KisImportExportManager.h"
#include "KisPart.h"
#include "KisView.h"
#include "kis_grid_config.h"
#include "kis_guides_config.h"
#include "kis_image_barrier_lock_adapter.h"
#include <mutex>


// Define the protocol used here for embedded documents' URL
// This used to "store" but QUrl didn't like it,
// so let's simply make it "tar" !
#define STORE_PROTOCOL "tar"
// The internal path is a hack to make QUrl happy and for document children
#define INTERNAL_PROTOCOL "intern"
#define INTERNAL_PREFIX "intern:/"
// Warning, keep it sync in koStore.cc

#include <unistd.h>

using namespace std;

/**********************************************************
 *
 * KisDocument
 *
 **********************************************************/

namespace {

class DocumentProgressProxy : public KoProgressProxy {
public:
    KisMainWindow *m_mainWindow;
    DocumentProgressProxy(KisMainWindow *mainWindow)
        : m_mainWindow(mainWindow)
    {
    }

    ~DocumentProgressProxy() override {
        // signal that the job is done
        setValue(-1);
    }

    int maximum() const override {
        return 100;
    }

    void setValue(int value) override {
        if (m_mainWindow) {
            m_mainWindow->slotProgress(value);
        }
    }

    void setRange(int /*minimum*/, int /*maximum*/) override {

    }

    void setFormat(const QString &/*format*/) override {

    }
};
}

//static
QString KisDocument::newObjectName()
{
    static int s_docIFNumber = 0;
    QString name; name.setNum(s_docIFNumber++); name.prepend("document_");
    return name;
}


class UndoStack : public KUndo2Stack
{
public:
    UndoStack(KisDocument *doc)
        : m_doc(doc)
    {
    }

    void setIndex(int idx) override {
        KisImageWSP image = this->image();
        image->requestStrokeCancellation();
        if(image->tryBarrierLock()) {
            KUndo2Stack::setIndex(idx);
            image->unlock();
        }
    }

    void notifySetIndexChangedOneCommand() override {
        KisImageWSP image = this->image();
        image->unlock();

        /**
         * Some very weird commands may emit blocking signals to
         * the GUI (e.g. KisGuiContextCommand). Here is the best thing
         * we can do to avoid the deadlock
         */
        while(!image->tryBarrierLock()) {
            QApplication::processEvents();
        }
    }

    void undo() override {
        KisImageWSP image = this->image();
        image->requestUndoDuringStroke();

        if (image->tryUndoUnfinishedLod0Stroke() == UNDO_OK) {
            return;
        }

        if(image->tryBarrierLock()) {
            KUndo2Stack::undo();
            image->unlock();
        }
    }

    void redo() override {
        KisImageWSP image = this->image();
        if(image->tryBarrierLock()) {
            KUndo2Stack::redo();
            image->unlock();
        }
    }

private:
    KisImageWSP image() {
        KisImageWSP currentImage = m_doc->image();
        Q_ASSERT(currentImage);
        return currentImage;
    }

private:
    KisDocument *m_doc;
};

class Q_DECL_HIDDEN KisDocument::Private
{
public:
    Private() :
        docInfo(0),
        progressUpdater(0),
        progressProxy(0),
        importExportManager(0),
        isImporting(false),
        isExporting(false),
        password(QString()),
        modifiedAfterAutosave(false),
        isAutosaving(false),
        backupFile(true),
        doNotSaveExtDoc(false),
        undoStack(0),
        m_saveOk(false),
        m_waitForSave(false),
        m_duringSaveAs(false),
        m_bAutoDetectedMime(false),
        modified(false),
        readwrite(true),
        disregardAutosaveFailure(false),
        nserver(0),
        macroNestDepth(0),
        imageIdleWatcher(2000 /*ms*/),
        suppressProgress(false),
        fileProgressProxy(0),
        savingLock(&savingMutex)
    {
        if (QLocale().measurementSystem() == QLocale::ImperialSystem) {
            unit = KoUnit::Inch;
        } else {
            unit = KoUnit::Centimeter;
        }
    }

    ~Private() {
        // Don't delete m_d->shapeController because it's in a QObject hierarchy.
        delete nserver;
    }

    KoDocumentInfo *docInfo;

    KoProgressUpdater *progressUpdater;
    KoProgressProxy *progressProxy;

    KoUnit unit;

    KisImportExportManager *importExportManager; // The filter-manager to use when loading/saving [for the options]

    QByteArray mimeType; // The actual mimetype of the document
    QByteArray outputMimeType; // The mimetype to use when saving

    bool isImporting;
    bool isExporting; // File --> Import/Export vs File --> Open/Save
    QString password; // The password used to encrypt an encrypted document

    QTimer autoSaveTimer;
    QString lastErrorMessage; // see openFile()
    QString lastWarningMessage;
    int autoSaveDelay {300}; // in seconds, 0 to disable.
    bool modifiedAfterAutosave;
    bool isAutosaving;
    bool backupFile;
    bool doNotSaveExtDoc; // makes it possible to save only internally stored child documents

    KUndo2Stack *undoStack;

    KisGuidesConfig guidesConfig;

    QUrl m_originalURL; // for saveAs
    QString m_originalFilePath; // for saveAs
    bool m_saveOk;
    bool m_waitForSave;
    bool m_duringSaveAs;
    bool m_bAutoDetectedMime; // whether the mimetype in the arguments was detected by the part itself
    QUrl m_url; // local url - the one displayed to the user.
    QString m_file; // Local file - the only one the part implementation should deal with.
    QEventLoop m_eventLoop;
    QMutex savingMutex;

    bool modified;
    bool readwrite;

    QDateTime firstMod;
    QDateTime lastMod;

    bool disregardAutosaveFailure;

    KisNameServer *nserver;
    qint32 macroNestDepth;

    KisImageSP image;
    KisImageSP savingImage;

    KisNodeSP preActivatedNode;
    KisShapeController* shapeController;
    KoShapeController* koShapeController;
    KisIdleWatcher imageIdleWatcher;
    QScopedPointer<KisSignalAutoConnection> imageIdleConnection;

    bool suppressProgress;
    KoProgressProxy* fileProgressProxy;

    QList<KisPaintingAssistantSP> assistants;
    KisGridConfig gridConfig;

    StdLockableWrapper<QMutex> savingLock;

    void setImageAndInitIdleWatcher(KisImageSP _image) {
        image = _image;

        imageIdleWatcher.setTrackedImage(image);

        if (image) {
            imageIdleConnection.reset(
                        new KisSignalAutoConnection(
                            &imageIdleWatcher, SIGNAL(startedIdleMode()),
                            image.data(), SLOT(explicitRegenerateLevelOfDetail())));
        }
    }

    class SafeSavingLocker;
};

class KisDocument::Private::SafeSavingLocker {
public:
    SafeSavingLocker(KisDocument::Private *_d, KisDocument *document)
        : d(_d)
        , m_document(document)
        , m_locked(false)
        , m_imageLock(d->image, true)
    {
        const int realAutoSaveInterval = KisConfig().autoSaveInterval();
        const int emergencyAutoSaveInterval = 10; // sec

        /**
         * Initial try to lock both objects. Locking the image guards
         * us from any image composition threads running in the
         * background, while savingMutex guards us from entering the
         * saving code twice by autosave and main threads.
         *
         * Since we are trying to lock multiple objects, so we should
         * do it in a safe manner.
         */
        m_locked = std::try_lock(m_imageLock, d->savingLock) < 0;

        if (!m_locked) {
            if (d->isAutosaving) {
                d->disregardAutosaveFailure = true;
                if (realAutoSaveInterval) {
                    m_document->setAutoSaveDelay(emergencyAutoSaveInterval);
                }
            } else {
                d->image->requestStrokeEnd();
                QApplication::processEvents();

                // one more try...
                m_locked = std::try_lock(m_imageLock, d->savingLock) < 0;
            }
        }

        if (m_locked) {
            d->disregardAutosaveFailure = false;
        }
    }

    ~SafeSavingLocker() {
         if (m_locked) {
             m_imageLock.unlock();
             d->savingLock.unlock();

             const int realAutoSaveInterval = KisConfig().autoSaveInterval();
             m_document->setAutoSaveDelay(realAutoSaveInterval);
         }
     }

    bool successfullyLocked() const {
        return m_locked;
    }

private:
    KisDocument::Private *d;
    KisDocument *m_document;
    bool m_locked;

    KisImageBarrierLockAdapter m_imageLock;
};

KisDocument::KisDocument()
    : d(new Private())
{
    d->undoStack = new UndoStack(this);
    d->undoStack->setParent(this);

    d->importExportManager = new KisImportExportManager(this);
    d->importExportManager->setProgresUpdater(d->progressUpdater);

    connect(&d->autoSaveTimer, SIGNAL(timeout()), this, SLOT(slotAutoSave()));
    KisConfig cfg;
    setAutoSaveDelay(cfg.autoSaveInterval());

    setObjectName(newObjectName());

    d->docInfo = new KoDocumentInfo(this);

    d->firstMod = QDateTime::currentDateTime();
    d->lastMod = QDateTime::currentDateTime();


    // preload the krita resources
    KisResourceServerProvider::instance();

    d->nserver = new KisNameServer(1);

    d->shapeController = new KisShapeController(this, d->nserver);
    d->koShapeController = new KoShapeController(0, d->shapeController);

    undoStack()->setUndoLimit(KisConfig().undoStackLimit());
    connect(d->undoStack, SIGNAL(indexChanged(int)), this, SLOT(slotUndoStackIndexChanged(int)));
    setBackupFile(KisConfig().backupFile());
}

KisDocument::~KisDocument()
{
    /**
     * Push a timebomb, which will try to release the memory after
     * the document has been deleted
     */
    KisPaintDevice::createMemoryReleaseObject()->deleteLater();

    d->autoSaveTimer.disconnect(this);
    d->autoSaveTimer.stop();

    delete d->importExportManager;

    // Despite being QObject they needs to be deleted before the image
    delete d->shapeController;

    delete d->koShapeController;

    if (d->image) {
        d->image->notifyAboutToBeDeleted();

        /**
         * WARNING: We should wait for all the internal image jobs to
         * finish before entering KisImage's destructor. The problem is,
         * while execution of KisImage::~KisImage, all the weak shared
         * pointers pointing to the image enter an inconsistent
         * state(!). The shared counter is already zero and destruction
         * has started, but the weak reference doesn't know about it,
         * because KisShared::~KisShared hasn't been executed yet. So all
         * the threads running in background and having weak pointers will
         * enter the KisImage's destructor as well.
         */

        d->image->requestStrokeCancellation();
        d->image->waitForDone();

        // clear undo commands that can still point to the image
        d->undoStack->clear();
        d->image->waitForDone();

        KisImageWSP sanityCheckPointer = d->image;
        Q_UNUSED(sanityCheckPointer);
        // The following line trigger the deletion of the image
        d->image.clear();

        // check if the image has actually been deleted
        KIS_SAFE_ASSERT_RECOVER_NOOP(!sanityCheckPointer.isValid());
    }


    delete d;
}

bool KisDocument::reload()
{
    // XXX: reimplement!
    return false;
}

bool KisDocument::exportDocument(const QUrl &_url, KisPropertiesConfigurationSP exportConfiguration)
{
    //qDebug() << "exportDocument" << _url.toDisplayString() << "is autosaving" << d->isAutosaving;
    bool ret;

    d->isExporting = true;

    //
    // Preserve a lot of state here because we need to restore it in order to
    // be able to fake a File --> Export.  Can't do this in saveFile() because,
    // for a start, KParts has already set url and m_file and because we need
    // to restore the modified flag etc. and don't want to put a load on anyone
    // reimplementing saveFile() (Note: importDocument() and exportDocument()
    // will remain non-virtual).
    //
    QUrl oldURL = url();
    QString oldFile = localFilePath();

    //qDebug() << "\toldUrl" << oldURL << "oldFile" << oldFile << "export url" << _url;

    bool wasModified = isModified();

    // save...
    ret = saveAs(_url, exportConfiguration);

    //
    // This is sooooo hacky :(
    // Hopefully we will restore enough state.
    //
    dbgUI << "Restoring KisDocument state to before export";

    // always restore url & m_file regardless of failure or success
    //qDebug() << "\tafter saveAs: url" << url() << "local file path" << localFilePath();
    setUrl(oldURL);
    setLocalFilePath(oldFile);
    //qDebug() << "\tafter restoring: url" << url() << "local file path" << localFilePath();


    // on successful export we need to restore modified etc. too
    // on failed export, mimetype/modified hasn't changed anyway
    if (ret) {
        setModified(wasModified);
   }

    d->isExporting = false;

    return ret;
}

bool KisDocument::saveAs(const QUrl &url, KisPropertiesConfigurationSP exportConfiguration)
{
    //qDebug() << "saveAs" << url;
    if (!url.isValid() || !url.isLocalFile()) {
        errKrita << "saveAs: Malformed URL " << url.url() << endl;
        return false;
    }
    d->m_duringSaveAs = true;
    d->m_originalURL = d->m_url;
    d->m_originalFilePath = d->m_file;
    d->m_url = url; // Store where to upload in saveToURL
    d->m_file = d->m_url.toLocalFile();

    bool result = save(exportConfiguration); // Save local file and upload local file

    if (!result) {
        d->m_url = d->m_originalURL;
        d->m_file = d->m_originalFilePath;
        d->m_duringSaveAs = false;
        d->m_originalURL = QUrl();
        d->m_originalFilePath.clear();
    }

    return result;
}

bool KisDocument::save(KisPropertiesConfigurationSP exportConfiguration)
{
    //qDebug() << "save" << d->m_file << d->m_url << url() << localFilePath();

    d->m_saveOk = false;

    if (d->m_file.isEmpty()) { // document was created empty
        d->m_file = d->m_url.toLocalFile();
    }

    updateEditingTime(true);

    setFileProgressProxy();
    setUrl(url());

    bool ok = saveFile(localFilePath(), exportConfiguration);

    clearFileProgressProxy();

    if (ok) {
        setModified( false );
        emit completed();
        d->m_saveOk = true;
        d->m_duringSaveAs = false;
        d->m_originalURL = QUrl();
        d->m_originalFilePath.clear();
        return true; // Nothing to do
    }
    else {
        emit canceled(QString());
    }
    return false;
}

QByteArray KisDocument::serializeToNativeByteArray()
{
    QByteArray byteArray;
    QBuffer buffer(&byteArray);

    QScopedPointer<KisImportExportFilter> filter(KisImportExportManager::filterForMimeType(nativeFormatMimeType(), KisImportExportManager::Export));
    filter->setBatchMode(true);
    filter->setMimeType(nativeFormatMimeType());

    if (!prepareLocksForSaving()) {
        return byteArray;
    }

    if (filter->convert(this, &buffer) != KisImportExportFilter::OK) {
        qWarning() << "serializeToByteArray():: Could not export to our native format";
    }

    unlockAfterSaving();

    return byteArray;
}

bool KisDocument::isInSaving() const
{
    std::unique_lock<StdLockableWrapper<QMutex>> l(d->savingLock, std::try_to_lock);
    return !l.owns_lock();
}

bool KisDocument::saveFile(const QString &filePath, KisPropertiesConfigurationSP exportConfiguration)
{
    if (!prepareLocksForSaving()) {
        return false;
    }

    // Unset the error message
    setErrorMessage("");

    // Save it to be able to restore it after a failed save
    const bool wasModified = isModified();
    bool ret = false;
    bool suppressErrorDialog = fileBatchMode();
    KisImportExportFilter::ConversionStatus status = KisImportExportFilter::OK;

    //qDebug() << "saveFile" << localFilePath() << QFileInfo(localFilePath()).exists() << !QFileInfo(localFilePath()).isWritable();

    if (QFileInfo(localFilePath()).exists() && !QFileInfo(localFilePath()).isWritable()) {
        setErrorMessage(i18n("%1 cannot be written to. Please save under a different name.", localFilePath()));
    }
    else {

        // The output format is set by KisMainWindow, and by openFile
        QByteArray outputMimeType = d->outputMimeType;

        if (outputMimeType.isEmpty()) {
            outputMimeType = d->outputMimeType = nativeFormatMimeType();
        }

        //qDebug() << "saveFile. Is Autosaving?" << isAutosaving() << "url" << filePath << d->outputMimeType;


        if (d->backupFile) {
            Q_ASSERT(url().isLocalFile());
            KBackup::backupFile(url().toLocalFile());
        }

        qApp->processEvents();

        setFileProgressUpdater(i18n("Saving Document"));

        //qDebug() << "saving to tempory file" << tempororaryFileName;
        status = d->importExportManager->exportDocument(localFilePath(), filePath, outputMimeType, !d->isExporting , exportConfiguration);

        ret = (status == KisImportExportFilter::OK);
        suppressErrorDialog = (fileBatchMode() || isAutosaving() || status == KisImportExportFilter::UserCancelled || status == KisImportExportFilter::BadConversionGraph);
        //qDebug() << "Export status was" << status;

        if (ret) {

            if (!d->isAutosaving && !d->suppressProgress) {
                QPointer<KoUpdater> updater = d->progressUpdater->startSubtask(1, "clear undo stack");
                updater->setProgress(0);
                d->undoStack->setClean();
                updater->setProgress(100);
            } else {
                d->undoStack->setClean();
            }

            if (errorMessage().isEmpty()) {
                if (!isAutosaving()) {
                    removeAutoSaveFiles();
                }
            }
            else {
                ret = false;
                qWarning() << "Error while saving:" << errorMessage();
            }
            // Restart the autosave timer
            // (we don't want to autosave again 2 seconds after a real save)
            if (!isAutosaving()) {
                setAutoSaveDelay(d->autoSaveDelay);
            }

            d->mimeType = outputMimeType;
        }
    }

    if (!ret) {
        if (!suppressErrorDialog) {

            if (errorMessage().isEmpty()) {
                setErrorMessage(KisImportExportFilter::conversionStatusString(status));
            }

            if (errorMessage().isEmpty()) {
                QMessageBox::critical(0, i18nc("@title:window", "Krita"), i18n("Could not save\n%1", filePath));
            } else {
                QMessageBox::critical(0, i18nc("@title:window", "Krita"), i18n("Could not save %1\nReason: %2", filePath, errorMessage()));
            }

        }

        // couldn't save file so this new URL is invalid
        // FIXME: we should restore the current document's true URL instead of
        // setting it to nothing otherwise anything that depends on the URL
        // being correct will not work (i.e. the document will be called
        // "Untitled" which may not be true)
        //
        // Update: now the URL is restored in KisMainWindow but really, this
        // should still be fixed in KisDocument/KParts (ditto for file).
        // We still resetURL() here since we may or may not have been called
        // by KisMainWindow - Clarence
        resetURL();

        // As we did not save, restore the "was modified" status
        setModified(wasModified);
    }

    emit sigSavingFinished();
    clearFileProgressUpdater();

    unlockAfterSaving();
    return ret;
}


QByteArray KisDocument::mimeType() const
{
    return d->mimeType;
}

void KisDocument::setMimeType(const QByteArray & mimeType)
{
    d->mimeType = mimeType;
}

void KisDocument::setOutputMimeType(const QByteArray & mimeType)
{
    d->outputMimeType = mimeType;
}

QByteArray KisDocument::outputMimeType() const
{
    return d->outputMimeType;
}

bool KisDocument::fileBatchMode() const
{
    return d->importExportManager->batchMode();
}

void KisDocument::setFileBatchMode(const bool batchMode)
{
    d->importExportManager->setBatchMode(batchMode);
}

bool KisDocument::isImporting() const
{
    return d->isImporting;
}

bool KisDocument::isExporting() const
{
    return d->isExporting;
}

void KisDocument::slotAutoSave()
{
    //qDebug() << "slotAutoSave. Modified:"  << d->modified << "modifiedAfterAutosave" << d->modified << "url" << url() << localFilePath();

    if (!d->isAutosaving && d->modified && d->modifiedAfterAutosave) {

        bool batchmode = d->importExportManager->batchMode();
        d->importExportManager->setBatchMode(true);
        qApp->setOverrideCursor(Qt::BusyCursor);
        connect(this, SIGNAL(sigProgress(int)), KisPart::instance()->currentMainwindow(), SLOT(slotProgress(int)));
        emit statusBarMessage(i18n("Autosaving..."));
        d->isAutosaving = true;
        QString autoSaveFileName = generateAutoSaveFileName(localFilePath());

        QByteArray mimetype = d->outputMimeType;
        d->outputMimeType = nativeFormatMimeType();
        bool ret = exportDocument(QUrl::fromLocalFile(autoSaveFileName));
        d->outputMimeType = mimetype;

        if (ret) {
            d->modifiedAfterAutosave = false;
            d->autoSaveTimer.stop(); // until the next change
        }
        qApp->restoreOverrideCursor();
        d->importExportManager->setBatchMode(batchmode);
        d->isAutosaving = false;

        emit clearStatusBarMessage();
        disconnect(this, SIGNAL(sigProgress(int)), KisPart::instance()->currentMainwindow(), SLOT(slotProgress(int)));

        if (!ret && !d->disregardAutosaveFailure) {
            emit statusBarMessage(i18n("Error during autosave! Partition full?"));
        }
    }
}

void KisDocument::setReadWrite(bool readwrite)
{
    d->readwrite = readwrite;
    setAutoSaveDelay(d->autoSaveDelay);

    Q_FOREACH (KisMainWindow *mainWindow, KisPart::instance()->mainWindows()) {
        mainWindow->setReadWrite(readwrite);
    }
}

void KisDocument::setAutoSaveDelay(int delay)
{
    //qDebug() << "setting autosave delay from" << d->autoSaveDelay << "to" << delay;
    d->autoSaveDelay = delay;
    if (isReadWrite() && d->autoSaveDelay > 0) {
        d->autoSaveTimer.start(d->autoSaveDelay * 1000);
    }
    else {
        d->autoSaveTimer.stop();
    }
}

KoDocumentInfo *KisDocument::documentInfo() const
{
    return d->docInfo;
}

bool KisDocument::isModified() const
{
    return d->modified;
}

QPixmap KisDocument::generatePreview(const QSize& size)
{
    KisImageSP image = d->image;
    if (d->savingImage) image = d->savingImage;

    if (image) {
        QRect bounds = image->bounds();
        QSize newSize = bounds.size();
        newSize.scale(size, Qt::KeepAspectRatio);
        QPixmap px = QPixmap::fromImage(image->convertToQImage(newSize, 0));
        if (px.size() == QSize(0,0)) {
            px = QPixmap(newSize);
            QPainter gc(&px);
            QBrush checkBrush = QBrush(KisCanvasWidgetBase::createCheckersImage(newSize.width() / 5));
            gc.fillRect(px.rect(), checkBrush);
            gc.end();
        }
        return px;
    }
    return QPixmap(size);
}

QString KisDocument::generateAutoSaveFileName(const QString & path) const
{
    QString retval;

    // Using the extension allows to avoid relying on the mime magic when opening
    const QString extension (".kra");

    if (path.isEmpty()) {
        // Never saved?
#ifdef Q_OS_WIN
        // On Windows, use the temp location (https://bugs.kde.org/show_bug.cgi?id=314921)
        retval = QString("%1%2.%3-%4-%5-autosave%6").arg(QDir::tempPath()).arg(QDir::separator()).arg("krita").arg(qApp->applicationPid()).arg(objectName()).arg(extension);
#else
        // On Linux, use a temp file in $HOME then. Mark it with the pid so two instances don't overwrite each other's autosave file
        retval = QString("%1%2.%3-%4-%5-autosave%6").arg(QDir::homePath()).arg(QDir::separator()).arg("krita").arg(qApp->applicationPid()).arg(objectName()).arg(extension);
#endif
    } else {
        QFileInfo fi(path);
        QString dir = fi.absolutePath();
        QString filename = fi.fileName();
        retval = QString("%1%2.%3-autosave%4").arg(dir).arg(QDir::separator()).arg(filename).arg(extension);
    }

    //qDebug() << "generateAutoSaveFileName() for path" << path << ":" << retval;
    return retval;
}

bool KisDocument::importDocument(const QUrl &_url)
{
    bool ret;

    dbgUI << "url=" << _url.url();
    d->isImporting = true;

    // open...
    ret = openUrl(_url);

    // reset url & m_file (kindly? set by KisParts::openUrl()) to simulate a
    // File --> Import
    if (ret) {
        dbgUI << "success, resetting url";
        resetURL();
        setTitleModified();
    }

    d->isImporting = false;

    return ret;
}


bool KisDocument::openUrl(const QUrl &_url, KisDocument::OpenUrlFlags flags)
{
    if (!_url.isLocalFile()) {
        return false;
    }
    dbgUI << "url=" << _url.url();
    d->lastErrorMessage.clear();

    // Reimplemented, to add a check for autosave files and to improve error reporting
    if (!_url.isValid()) {
        d->lastErrorMessage = i18n("Malformed URL\n%1", _url.url());  // ## used anywhere ?
        return false;
    }

    QUrl url(_url);
    bool autosaveOpened = false;
    if (url.isLocalFile() && !fileBatchMode()) {
        QString file = url.toLocalFile();
        QString asf = generateAutoSaveFileName(file);
        if (QFile::exists(asf)) {
            KisApplication *kisApp = static_cast<KisApplication*>(qApp);
            kisApp->hideSplashScreen();
            //dbgUI <<"asf=" << asf;
            // ## TODO compare timestamps ?
            int res = QMessageBox::warning(0,
                                           i18nc("@title:window", "Krita"),
                                           i18n("An autosaved file exists for this document.\nDo you want to open the autosaved file instead?"),
                                           QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
            switch (res) {
            case QMessageBox::Yes :
                url.setPath(asf);
                autosaveOpened = true;
                break;
            case QMessageBox::No :
                QFile::remove(asf);
                break;
            default: // Cancel
                return false;
            }
        }
    }

    bool ret = openUrlInternal(url);

    if (autosaveOpened) {
        resetURL(); // Force save to act like 'Save As'
        setReadWrite(true); // enable save button
        setModified(true);
    }
    else {
        if( !(flags & OPEN_URL_FLAG_DO_NOT_ADD_TO_RECENT_FILES) ) {
            KisPart::instance()->addRecentURLToAllMainWindows(_url);
        }

        if (ret) {
            // Detect readonly local-files; remote files are assumed to be writable
            QFileInfo fi(url.toLocalFile());
            setReadWrite(fi.isWritable());
        }
    }
    return ret;
}

class DlgLoadMessages : public KoDialog {
public:
    DlgLoadMessages(const QString &title, const QString &message, const QStringList &warnings) {
        setWindowTitle(title);
        setWindowIcon(KisIconUtils::loadIcon("dialog-warning"));
        QWidget *page = new QWidget(this);
        QVBoxLayout *layout = new QVBoxLayout(page);
        QHBoxLayout *hlayout = new QHBoxLayout();
        QLabel *labelWarning= new QLabel();
        labelWarning->setPixmap(KisIconUtils::loadIcon("dialog-warning").pixmap(32, 32));
        hlayout->addWidget(labelWarning);
        hlayout->addWidget(new QLabel(message));
        layout->addLayout(hlayout);
        QTextBrowser *browser = new QTextBrowser();
        QString warning = "<html><body><p><b>";
        if (warnings.size() == 1) {
            warning += "</b> Reason:</p>";
        }
        else {
            warning += "</b> Reasons:</p>";
        }
        warning += "<p/><ul>";

        Q_FOREACH(const QString &w, warnings) {
            warning += "\n<li>" + w + "</li>";
        }
        warning += "</ul>";
        browser->setHtml(warning);
        browser->setMinimumHeight(200);
        browser->setMinimumWidth(400);
        layout->addWidget(browser);
        setMainWidget(page);
        setButtons(KoDialog::Ok);
        resize(minimumSize());
    }
};

bool KisDocument::openFile()
{
    //dbgUI <<"for" << localFilePath();
    if (!QFile::exists(localFilePath())) {
        QMessageBox::critical(0, i18nc("@title:window", "Krita"), i18n("File %1 does not exist.", localFilePath()));
        return false;
    }

    QString filename = localFilePath();
    QString typeName = mimeType();

    if (typeName.isEmpty()) {
        typeName = KisMimeDatabase::mimeTypeForFile(filename);
    }

    //qDebug() << "mimetypes 4:" << typeName;

    // Allow to open backup files, don't keep the mimetype application/x-trash.
    if (typeName == "application/x-trash") {
        QString path = filename;
        while (path.length() > 0) {
            path.chop(1);
            typeName = KisMimeDatabase::mimeTypeForFile(path);
            //qDebug() << "\t" << path << typeName;
            if (!typeName.isEmpty()) {
                break;
            }
        }
        //qDebug() << "chopped" << filename  << "to" << path << "Was trash, is" << typeName;
    }
    dbgUI << localFilePath() << "type:" << typeName;

    setFileProgressUpdater(i18n("Opening Document"));

    KisImportExportFilter::ConversionStatus status;

    status = d->importExportManager->importDocument(localFilePath(), typeName);
    if (status != KisImportExportFilter::OK) {
        QString msg = KisImportExportFilter::conversionStatusString(status);
        if (!msg.isEmpty()) {
            DlgLoadMessages dlg(i18nc("@title:window", "Krita"),
                                i18n("Could not open %2.\nReason: %1.", msg, prettyPathOrUrl()),
                                errorMessage().split("\n") + warningMessage().split("\n"));
            dlg.exec();
        }
        clearFileProgressUpdater();
        return false;
    }
    else if (!warningMessage().isEmpty()) {
        DlgLoadMessages dlg(i18nc("@title:window", "Krita"),
                            i18n("There were problems opening %1.", prettyPathOrUrl()),
                            warningMessage().split("\n"));
        dlg.exec();
        setUrl(QUrl());
    }

    setMimeTypeAfterLoading(typeName);
    emit sigLoadingFinished();

    if (!d->suppressProgress && d->progressUpdater) {
        QPointer<KoUpdater> updater = d->progressUpdater->startSubtask(1, "clear undo stack");
        updater->setProgress(0);
        undoStack()->clear();
        updater->setProgress(100);

        clearFileProgressUpdater();
    } else {
        undoStack()->clear();
    }

    return true;
}

KoProgressUpdater *KisDocument::progressUpdater() const
{
    return d->progressUpdater;
}

void KisDocument::setProgressProxy(KoProgressProxy *progressProxy)
{
    d->progressProxy = progressProxy;
}

KoProgressProxy* KisDocument::progressProxy() const
{
    if (!d->progressProxy) {
        KisMainWindow *mainWindow = 0;
        if (KisPart::instance()->mainwindowCount() > 0) {
            mainWindow = KisPart::instance()->mainWindows()[0];
        }
        d->progressProxy = new DocumentProgressProxy(mainWindow);
    }
    return d->progressProxy;
}

// shared between openFile and koMainWindow's "create new empty document" code
void KisDocument::setMimeTypeAfterLoading(const QString& mimeType)
{
    d->mimeType = mimeType.toLatin1();
    d->outputMimeType = d->mimeType;
}


bool KisDocument::loadNativeFormat(const QString & file_)
{
    return openUrl(QUrl::fromLocalFile(file_));
}

void KisDocument::setModified()
{
    d->modified = true;
}

void KisDocument::setModified(bool mod)
{
    if (mod) {
        updateEditingTime(false);
    }

    if (d->isAutosaving)   // ignore setModified calls due to autosaving
        return;

    if ( !d->readwrite && d->modified ) {
        errKrita << "Can't set a read-only document to 'modified' !" << endl;
        return;
    }

    //dbgUI<<" url:" << url.path();
    //dbgUI<<" mod="<<mod<<" MParts mod="<<KisParts::ReadWritePart::isModified()<<" isModified="<<isModified();

    if (mod && !d->modifiedAfterAutosave) {
        // First change since last autosave -> start the autosave timer
        setAutoSaveDelay(d->autoSaveDelay);
    }
    d->modifiedAfterAutosave = mod;

    if (mod == isModified())
        return;

    d->modified = mod;

    if (mod) {
        documentInfo()->updateParameters();
    }

    // This influences the title
    setTitleModified();
    emit modified(mod);
}

void KisDocument::updateEditingTime(bool forceStoreElapsed)
{
    QDateTime now = QDateTime::currentDateTime();
    int firstModDelta = d->firstMod.secsTo(now);
    int lastModDelta = d->lastMod.secsTo(now);

    if (lastModDelta > 30) {
        d->docInfo->setAboutInfo("editing-time", QString::number(d->docInfo->aboutInfo("editing-time").toInt() + d->firstMod.secsTo(d->lastMod)));
        d->firstMod = now;
    } else if (firstModDelta > 60 || forceStoreElapsed) {
        d->docInfo->setAboutInfo("editing-time", QString::number(d->docInfo->aboutInfo("editing-time").toInt() + firstModDelta));
        d->firstMod = now;
    }

    d->lastMod = now;
}

QString KisDocument::prettyPathOrUrl() const
{
    QString _url(url().toDisplayString());
#ifdef Q_OS_WIN
    if (url().isLocalFile()) {
        _url = QDir::toNativeSeparators(_url);
    }
#endif
    return _url;
}

// Get caption from document info (title(), in about page)
QString KisDocument::caption() const
{
    QString c;
    if (documentInfo()) {
        c = documentInfo()->aboutInfo("title");
    }
    const QString _url(url().fileName());
    if (!c.isEmpty() && !_url.isEmpty()) {
        c = QString("%1 - %2").arg(c).arg(_url);
    }
    else if (c.isEmpty()) {
        c = _url; // Fall back to document URL
    }
    return c;
}

void KisDocument::setTitleModified()
{
    emit titleModified(caption(), isModified());
}

QDomDocument KisDocument::createDomDocument(const QString& tagName, const QString& version) const
{
    return createDomDocument("krita", tagName, version);
}

//static
QDomDocument KisDocument::createDomDocument(const QString& appName, const QString& tagName, const QString& version)
{
    QDomImplementation impl;
    QString url = QString("http://www.calligra.org/DTD/%1-%2.dtd").arg(appName).arg(version);
    QDomDocumentType dtype = impl.createDocumentType(tagName,
                                                     QString("-//KDE//DTD %1 %2//EN").arg(appName).arg(version),
                                                     url);
    // The namespace URN doesn't need to include the version number.
    QString namespaceURN = QString("http://www.calligra.org/DTD/%1").arg(appName);
    QDomDocument doc = impl.createDocument(namespaceURN, tagName, dtype);
    doc.insertBefore(doc.createProcessingInstruction("xml", "version=\"1.0\" encoding=\"UTF-8\""), doc.documentElement());
    return doc;
}

bool KisDocument::isNativeFormat(const QByteArray& mimetype) const
{
    if (mimetype == nativeFormatMimeType())
        return true;
    return extraNativeMimeTypes().contains(mimetype);
}

void KisDocument::setErrorMessage(const QString& errMsg)
{
    d->lastErrorMessage = errMsg;
}

QString KisDocument::errorMessage() const
{
    return d->lastErrorMessage;
}

void KisDocument::setWarningMessage(const QString& warningMsg)
{
    d->lastWarningMessage = warningMsg;
}

QString KisDocument::warningMessage() const
{
    return d->lastWarningMessage;
}


void KisDocument::removeAutoSaveFiles()
{
    //qDebug() << "removeAutoSaveFiles";
    // Eliminate any auto-save file
    QString asf = generateAutoSaveFileName(localFilePath());   // the one in the current dir
    //qDebug() << "\tfilename:" << asf << "exists:" << QFile::exists(asf);
    if (QFile::exists(asf)) {
        //qDebug() << "\tremoving autosavefile" << asf;
        QFile::remove(asf);
    }
    asf = generateAutoSaveFileName(QString());   // and the one in $HOME
    //qDebug() << "Autsavefile in $home" << asf;
    if (QFile::exists(asf)) {
        //qDebug() << "\tremoving autsavefile 2" << asf;
        QFile::remove(asf);
    }
}

void KisDocument::setBackupFile(bool saveBackup)
{
    d->backupFile = saveBackup;
}

KoUnit KisDocument::unit() const
{
    return d->unit;
}

void KisDocument::setUnit(const KoUnit &unit)
{
    if (d->unit != unit) {
        d->unit = unit;
        emit unitChanged(unit);
    }
}

KUndo2Stack *KisDocument::undoStack()
{
    return d->undoStack;
}

KisImportExportManager *KisDocument::importExportManager() const
{
    return d->importExportManager;
}

void KisDocument::addCommand(KUndo2Command *command)
{
    if (command)
        d->undoStack->push(command);
}

void KisDocument::beginMacro(const KUndo2MagicString & text)
{
    d->undoStack->beginMacro(text);
}

void KisDocument::endMacro()
{
    d->undoStack->endMacro();
}

void KisDocument::slotUndoStackIndexChanged(int idx)
{
    // even if the document was already modified, call setModified to re-start autosave timer
    setModified(idx != d->undoStack->cleanIndex());
}

void KisDocument::clearUndoHistory()
{
    d->undoStack->clear();
}

KisGridConfig KisDocument::gridConfig() const
{
    return d->gridConfig;
}

void KisDocument::setGridConfig(const KisGridConfig &config)
{
    d->gridConfig = config;
}

const KisGuidesConfig& KisDocument::guidesConfig() const
{
    return d->guidesConfig;
}

void KisDocument::setGuidesConfig(const KisGuidesConfig &data)
{
    if (d->guidesConfig == data) return;

    d->guidesConfig = data;
    emit sigGuidesConfigChanged(d->guidesConfig);
}

void KisDocument::resetURL() {
    setUrl(QUrl());
    setLocalFilePath(QString());
}

KoDocumentInfoDlg *KisDocument::createDocumentInfoDialog(QWidget *parent, KoDocumentInfo *docInfo) const
{
    return new KoDocumentInfoDlg(parent, docInfo);
}

bool KisDocument::isReadWrite() const
{
    return d->readwrite;
}

QUrl KisDocument::url() const
{
    return d->m_url;
}

bool KisDocument::closeUrl(bool promptToSave)
{
    if (promptToSave) {
        if ( isReadWrite() && isModified()) {
            Q_FOREACH (KisView *view, KisPart::instance()->views()) {
                if (view && view->document() == this) {
                    if (!view->queryClose()) {
                        return false;
                    }
                }
            }
        }
    }
    // Not modified => ok and delete temp file.
    d->mimeType = QByteArray();

    // It always succeeds for a read-only part,
    // but the return value exists for reimplementations
    // (e.g. pressing cancel for a modified read-write part)
    return true;
}



void KisDocument::setUrl(const QUrl &url)
{
    d->m_url = url;
}

QString KisDocument::localFilePath() const
{
    return d->m_file;
}


void KisDocument::setLocalFilePath( const QString &localFilePath )
{
    d->m_file = localFilePath;
}

bool KisDocument::openUrlInternal(const QUrl &url)
{
    if ( !url.isValid() )
        return false;

    if (d->m_bAutoDetectedMime) {
        d->mimeType = QByteArray();
        d->m_bAutoDetectedMime = false;
    }

    QByteArray mimetype = d->mimeType;

    if ( !closeUrl() )
        return false;

    d->mimeType = mimetype;
    setUrl(url);

    d->m_file.clear();

    if (d->m_url.isLocalFile()) {
        d->m_file = d->m_url.toLocalFile();
        bool ret;
        // set the mimetype only if it was not already set (for example, by the host application)
        if (d->mimeType.isEmpty()) {
            // get the mimetype of the file
            // using findByUrl() to avoid another string -> url conversion
            QString mime = KisMimeDatabase::mimeTypeForFile(d->m_url.toLocalFile());
            d->mimeType = mime.toLocal8Bit();
            d->m_bAutoDetectedMime = true;
        }
        setFileProgressProxy();
        setUrl(d->m_url);
        ret = openFile();
        clearFileProgressProxy();

        if (ret) {
            emit completed();
        } else {
            emit canceled(QString());
        }
        return ret;
    }
    return false;
}

bool KisDocument::newImage(const QString& name,
                           qint32 width, qint32 height,
                           const KoColorSpace* cs,
                           const KoColor &bgColor, bool backgroundAsLayer,
                           int numberOfLayers,
                           const QString &description, const double imageResolution)
{
    Q_ASSERT(cs);

    KisConfig cfg;

    KisImageSP image;
    KisPaintLayerSP layer;

    if (!cs) return false;

    QApplication::setOverrideCursor(Qt::BusyCursor);

    image = new KisImage(createUndoStore(), width, height, cs, name);

    Q_CHECK_PTR(image);

    connect(image, SIGNAL(sigImageModified()), this, SLOT(setImageModified()), Qt::UniqueConnection);
    image->setResolution(imageResolution, imageResolution);

    image->assignImageProfile(cs->profile());
    documentInfo()->setAboutInfo("title", name);
    if (name != i18n("Unnamed") && !name.isEmpty()) {
        setUrl(QUrl::fromLocalFile(QDesktopServices::storageLocation(QDesktopServices::PicturesLocation) + '/' + name + ".kra"));
    }
    documentInfo()->setAboutInfo("abstract", description);

    layer = new KisPaintLayer(image.data(), image->nextLayerName(), OPACITY_OPAQUE_U8, cs);
    Q_CHECK_PTR(layer);

    if (backgroundAsLayer) {
        image->setDefaultProjectionColor(KoColor(cs));

        if (bgColor.opacityU8() == OPACITY_OPAQUE_U8) {
            layer->paintDevice()->setDefaultPixel(bgColor);
        } else {
            // Hack: with a semi-transparent background color, the projection isn't composited right if we just set the default pixel
            KisFillPainter painter;
            painter.begin(layer->paintDevice());
            painter.fillRect(0, 0, width, height, bgColor, bgColor.opacityU8());
        }
    } else {
        image->setDefaultProjectionColor(bgColor);
    }
    layer->setDirty(QRect(0, 0, width, height));

    image->addNode(layer.data(), image->rootLayer().data());
    setCurrentImage(image);

    for(int i = 1; i < numberOfLayers; ++i) {
        KisPaintLayerSP layer = new KisPaintLayer(image, image->nextLayerName(), OPACITY_OPAQUE_U8, cs);
        image->addNode(layer, image->root(), i);
        layer->setDirty(QRect(0, 0, width, height));
    }

    cfg.defImageWidth(width);
    cfg.defImageHeight(height);
    cfg.defImageResolution(imageResolution);
    cfg.defColorModel(image->colorSpace()->colorModelId().id());
    cfg.setDefaultColorDepth(image->colorSpace()->colorDepthId().id());
    cfg.defColorProfile(image->colorSpace()->profile()->name());

    QApplication::restoreOverrideCursor();

    return true;
}

KoShapeBasedDocumentBase *KisDocument::shapeController() const
{
    return d->shapeController;
}

KoShapeLayer* KisDocument::shapeForNode(KisNodeSP layer) const
{
    return d->shapeController->shapeForNode(layer);
}

vKisNodeSP KisDocument::activeNodes() const
{
    vKisNodeSP nodes;
    Q_FOREACH (KisView *v, KisPart::instance()->views()) {
        if (v->document() == this && v->viewManager()) {
            KisNodeSP activeNode = v->viewManager()->activeNode();
            if (activeNode && !nodes.contains(activeNode)) {
                if (activeNode->inherits("KisMask")) {
                    activeNode = activeNode->parent();
                }
                nodes.append(activeNode);
            }
        }
    }
    return nodes;
}

QList<KisPaintingAssistantSP> KisDocument::assistants() const
{
    return d->assistants;
}

void KisDocument::setAssistants(const QList<KisPaintingAssistantSP> value)
{
    d->assistants = value;
}

void KisDocument::setPreActivatedNode(KisNodeSP activatedNode)
{
    d->preActivatedNode = activatedNode;
}

KisNodeSP KisDocument::preActivatedNode() const
{
    return d->preActivatedNode;
}

void KisDocument::setFileProgressUpdater(const QString &text)
{
    d->suppressProgress = d->importExportManager->batchMode();

    if (!d->suppressProgress) {
        d->progressUpdater = new KoProgressUpdater(d->progressProxy, KoProgressUpdater::Unthreaded);
        d->progressUpdater->start(100, text);
        d->importExportManager->setProgresUpdater(d->progressUpdater);
        if (KisPart::instance()->currentMainwindow()) {
            connect(this, SIGNAL(sigProgress(int)), KisPart::instance()->currentMainwindow(), SLOT(slotProgress(int)));
            connect(KisPart::instance()->currentMainwindow(), SIGNAL(sigProgressCanceled()), this, SIGNAL(sigProgressCanceled()));
        }
    }
}

void KisDocument::clearFileProgressUpdater()
{
    if (!d->suppressProgress && d->progressUpdater) {
        if (KisPart::instance()->currentMainwindow()) {
            disconnect(KisPart::instance()->currentMainwindow(), SIGNAL(sigProgressCanceled()), this, SIGNAL(sigProgressCanceled()));
            disconnect(this, SIGNAL(sigProgress(int)), KisPart::instance()->currentMainwindow(), SLOT(slotProgress(int)));
        }
        delete d->progressUpdater;
        d->importExportManager->setProgresUpdater(0);
        d->progressUpdater = 0;
    }
}

void KisDocument::setFileProgressProxy()
{
    if (!d->progressProxy && !d->importExportManager->batchMode()) {
        d->fileProgressProxy = progressProxy();
    } else {
        d->fileProgressProxy = 0;
    }
}

void KisDocument::clearFileProgressProxy()
{
    if (d->fileProgressProxy) {
        setProgressProxy(0);
        delete d->fileProgressProxy;
        d->fileProgressProxy = 0;
    }
}

KisImageWSP KisDocument::image() const
{
    return d->image;
}

KisImageSP KisDocument::savingImage() const
{
    return d->savingImage;
}


void KisDocument::setCurrentImage(KisImageSP image)
{
    if (d->image) {
        // Disconnect existing sig/slot connections
        d->image->disconnect(this);
        d->shapeController->setImage(0);
        d->image = 0;
    }

    if (!image) return;

    d->setImageAndInitIdleWatcher(image);
    d->shapeController->setImage(image);
    setModified(false);
    connect(d->image, SIGNAL(sigImageModified()), this, SLOT(setImageModified()), Qt::UniqueConnection);
    d->image->initialRefreshGraph();
}

void KisDocument::setImageModified()
{
    setModified(true);
}


KisUndoStore* KisDocument::createUndoStore()
{
    return new KisDocumentUndoStore(this);
}

bool KisDocument::isAutosaving() const
{
    return d->isAutosaving;
}

bool KisDocument::prepareLocksForSaving()
{
    KisImageSP copiedImage;
    // XXX: Restore this when
    // a) cloning works correctly and
    // b) doesn't take ages because it needs to refresh its entire graph and finally,
    // c) we do use the saving image to save in the background.
    {
        Private::SafeSavingLocker locker(d, this);
        if (locker.successfullyLocked()) {

            copiedImage = d->image; //->clone(true);
        }
        else if (!isAutosaving()) {
            // even though it is a recovery operation, we should ensure we do not enter saving twice!
            std::unique_lock<StdLockableWrapper<QMutex>> l(d->savingLock, std::try_to_lock);

            if (l.owns_lock()) {
                d->lastErrorMessage = i18n("The image was still busy while saving. Your saved image might be incomplete.");
                d->image->lock();
                copiedImage = d->image; //->clone(true);
                //copiedImage->initialRefreshGraph();
                d->image->unlock();
            }
        }
    }

    bool result = false;

    // ensure we do not enter saving twice
    if (copiedImage && d->savingMutex.tryLock()) {
        d->savingImage = copiedImage;
        result = true;
    } else {
        qWarning() << "Could not lock the document for saving!";
        d->lastErrorMessage = i18n("Could not lock the image for saving.");
    }

    return result;
}

void KisDocument::unlockAfterSaving()
{
    d->savingImage = 0;
    d->savingMutex.unlock();
}

