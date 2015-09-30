/* This file is part of the KDE project
   Copyright (C) 2005-2006 Peter Simonsson <psn@linux.se>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#ifndef KIS_RECENT_DOCUMENTS_PANE_H
#define KIS_RECENT_DOCUMENTS_PANE_H

#include "KisDetailsPane.h"

class KFileItem;
class QPixmap;
class KJob;

class KisRecentDocumentsPanePrivate;

/**
 * This widget is the recent doc part of the template opening widget.
 * The parent widget is initial widget in the document space of each Calligra component.
 * This widget shows a list of recent documents and can show their details or open it.
 */
class KisRecentDocumentsPane : public KisDetailsPane
{
    Q_OBJECT
public:
    /**
     * Constructor.
     * @param parent the parent widget
     * @param header string used as header text in the listview
     */
    KisRecentDocumentsPane(QWidget* parent, const QString& header);
    ~KisRecentDocumentsPane();

protected Q_SLOTS:
    void selectionChanged(const QModelIndex& index);
    void openFile();
    void openFile(const QModelIndex& index);

    void previewResult(KJob* job);
    void updatePreview(const KFileItem& fileItem, const QPixmap& preview);
    void updateIcon(const KFileItem& fileItem, const QPixmap& pixmap);

private:
    KisRecentDocumentsPanePrivate * const d;
};

#endif