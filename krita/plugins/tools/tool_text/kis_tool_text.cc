/*
 *
 *  Copyright (c) 2011 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_tool_text.h"

#include <KoShapeRegistry.h>
#include <KoShapeController.h>
#include <KoColorBackground.h>
#include <KoCanvasBase.h>
#include <KoCanvasController.h>
#include <KoShape.h>

#include "kis_cursor.h"

KisToolText::KisToolText(KoCanvasBase * canvas)
        : KisToolRectangleBase(canvas, KisCursor::load("tool_rectangle_cursor.png", 6, 6))
{
    setObjectName("tool_text");
}

KisToolText::~KisToolText()
{
}

void KisToolText::finishRect(const QRectF &rect)
{
    if (rect.isNull())
        return;

    QRectF r = convertToPt(rect);
    QString shapeString = (m_optionWidget->mode() == KisTextToolOptionWidget::MODE_ARTISTIC) ? "ArtisticText" : "TextShapeID";
    KoShapeFactoryBase* textFactory = KoShapeRegistry::instance()->value(shapeString);
    if (textFactory) {
        KoShape* shape = textFactory->createDefaultShape();
        shape->setSize(r.size());
        shape->setPosition(r.topLeft());
        addShape(shape);
    }
}

QList< QWidget* > KisToolText::createOptionWidgets()
{
    m_optionWidget = new KisTextToolOptionWidget();
    QList< QWidget* > widgets;
    widgets.append(m_optionWidget);
    return widgets;
}

KisPainter::FillStyle KisToolText::fillStyle()
{
    return m_optionWidget->style();
}


#include "kis_tool_text.moc"
