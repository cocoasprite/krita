/*
 *  Copyright (c) 2003 Boudewijn Rempt <boud@valdyas.org>
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
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include <qpainter.h>

#include <kdebug.h>
#include <kaction.h>
#include <kcommand.h>
#include <klocale.h>
#include <koColor.h>

#include "kis_painter.h"
#include "kis_selection.h"
#include "kis_doc.h"
#include "kis_view.h"
#include "kis_tool_brush.h"
#include "kis_global.h"



KisToolBrush::KisToolBrush(KisView *view, KisDoc *doc)
    : super(view, doc),
      m_mousePressed( false ),
      m_polyline(3)
{
    m_view = view;
    m_doc = doc;

    setCursor( Qt::crossCursor );
}

KisToolBrush::~KisToolBrush()
{
}

void KisToolBrush::mousePress(QMouseEvent *e)
{
    m_mousePressed = true;
    m_polyline[2] = m_polyline[1] = m_polyline[0] = e->pos();
}



void KisToolBrush::mouseMove(QMouseEvent *e)
{
    if ( m_mousePressed ) {
        m_polyline[2] = m_polyline[1];
        m_polyline[1] = m_polyline[0];
        m_polyline[0] = e->pos();

        QRect r = m_polyline.boundingRect();
        r = r.normalize();

        KisImageSP img = m_view -> currentImg();
        if (img) {
            KisPaintDeviceSP device = img -> activeDevice();
            if (device) {
                KisPainter p( device );
                p.beginTransaction( "Brush" );
                p.brushpaint( m_polyline );
                p.endTransaction();
                device->anchor();
            }
        }
        img -> invalidate(r);
        m_view -> updateCanvas(r);
    }

}

void KisToolBrush::mouseRelease(QMouseEvent *e)
{
    m_mousePressed = false;
}


void KisToolBrush::tabletEvent(QTabletEvent *e)
{
    // XXX
    e->accept();
}


void KisToolBrush::setup()
{
	KToggleAction *toggle;
	toggle = new KToggleAction(i18n("&Brush"), "Brush", 0, this,
                                   SLOT(activateSelf()),
                                   m_view -> actionCollection(), "tool_brush
	toggle -> setExclusiveGroup("tools");
}

