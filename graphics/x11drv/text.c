/*
 * X11 graphics driver text functions
 *
 * Copyright 1993,1994 Alexandre Julliard
 */

#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include "windows.h"
#include "dc.h"
#include "gdi.h"
/*#include "callback.h"*/
#include "heap.h"
#include "x11drv.h"
#include "stddebug.h"
/* #define DEBUG_TEXT */
#include "debug.h"
#include "xmalloc.h"

#define SWAP_INT(a,b)  { int t = a; a = b; b = t; }


extern int CLIPPING_IntersectClipRect( DC * dc, short left, short top,
                                       short right, short bottom, UINT16 flags);

/***********************************************************************
 *           X11DRV_ExtTextOut
 */
BOOL32
X11DRV_ExtTextOut( DC *dc, INT32 x, INT32 y, UINT32 flags,
                   const RECT32 *lprect, LPCSTR str, UINT32 count,
                   const INT32 *lpDx )
{
    HRGN32	hRgnClip = 0;
    int 	dir, ascent, descent, i;
    XCharStruct info;
    XFontStruct *font;
    RECT32 	rect;

    if (!DC_SetupGCForText( dc )) return TRUE;
    font = dc->u.x.font.fstruct;

    dprintf_text(stddeb,"ExtTextOut: hdc=%04x %d,%d '%*.*s', %d  flags=%d\n",
                 dc->hSelf, x, y, count, count, str, count, flags);
    if (lprect != NULL) dprintf_text(stddeb, "\trect=(%d,%d- %d,%d)\n",
                                     lprect->left, lprect->top,
                                     lprect->right, lprect->bottom );

      /* Setup coordinates */

    if (dc->w.textAlign & TA_UPDATECP)
    {
	x = dc->w.CursPosX;
	y = dc->w.CursPosY;
    }

    if (flags & (ETO_OPAQUE | ETO_CLIPPED))  /* there's a rectangle */
    {
        if (!lprect)  /* not always */
        {
            SIZE32 sz;
            if (flags & ETO_CLIPPED)  /* Can't clip with no rectangle */
	      return FALSE;
	    if (!X11DRV_GetTextExtentPoint( dc, str, count, &sz ))
	      return FALSE;
	    rect.left   = XLPTODP( dc, x );
	    rect.right  = XLPTODP( dc, x+sz.cx );
	    rect.top    = YLPTODP( dc, y );
	    rect.bottom = YLPTODP( dc, y+sz.cy );
	}
	else
	{
	    rect.left   = XLPTODP( dc, lprect->left );
	    rect.right  = XLPTODP( dc, lprect->right );
	    rect.top    = YLPTODP( dc, lprect->top );
	    rect.bottom = YLPTODP( dc, lprect->bottom );
	}
	if (rect.right < rect.left) SWAP_INT( rect.left, rect.right );
	if (rect.bottom < rect.top) SWAP_INT( rect.top, rect.bottom );
    }

    x = XLPTODP( dc, x );
    y = YLPTODP( dc, y );

    dprintf_text(stddeb,"\treal coord: x=%i, y=%i, rect=(%d,%d-%d,%d)\n",
			  x, y, rect.left, rect.top, rect.right, rect.bottom);

      /* Draw the rectangle */

    if (flags & ETO_OPAQUE)
    {
        XSetForeground( display, dc->u.x.gc, dc->w.backgroundPixel );
        XFillRectangle( display, dc->u.x.drawable, dc->u.x.gc,
                        dc->w.DCOrgX + rect.left, dc->w.DCOrgY + rect.top,
                        rect.right-rect.left, rect.bottom-rect.top );
    }
    if (!count) return TRUE;  /* Nothing more to do */

      /* Compute text starting position */

    XTextExtents( font, str, count, &dir, &ascent, &descent, &info );

    if (lpDx) /* have explicit character cell x offsets */
    {
	/* sum lpDx array and add the width of last character */

        info.width = XTextWidth( font, str + count - 1, 1) + dc->w.charExtra;
        if (str[count-1] == (char)dc->u.x.font.metrics.tmBreakChar)
            info.width += dc->w.breakExtra;

        for (i = 0; i < count; i++) info.width += lpDx[i];
    }
    else
       info.width += count*dc->w.charExtra + dc->w.breakExtra*dc->w.breakCount;

    switch( dc->w.textAlign & (TA_LEFT | TA_RIGHT | TA_CENTER) )
    {
      case TA_LEFT:
 	  if (dc->w.textAlign & TA_UPDATECP)
	      dc->w.CursPosX = XDPTOLP( dc, x + info.width );
	  break;
      case TA_RIGHT:
	  x -= info.width;
	  if (dc->w.textAlign & TA_UPDATECP) dc->w.CursPosX = XDPTOLP( dc, x );
	  break;
      case TA_CENTER:
	  x -= info.width / 2;
	  break;
    }

    switch( dc->w.textAlign & (TA_TOP | TA_BOTTOM | TA_BASELINE) )
    {
      case TA_TOP:
	  y += font->ascent;
	  break;
      case TA_BOTTOM:
	  y -= font->descent;
	  break;
      case TA_BASELINE:
	  break;
    }

      /* Set the clip region */

    if (flags & ETO_CLIPPED)
    {
        hRgnClip = dc->w.hClipRgn;
        CLIPPING_IntersectClipRect( dc, rect.left, rect.top, rect.right,
                                    rect.bottom, CLIP_INTERSECT|CLIP_KEEPRGN );
    }

      /* Draw the text background if necessary */

    if (dc->w.backgroundMode != TRANSPARENT)
    {
          /* If rectangle is opaque and clipped, do nothing */
        if (!(flags & ETO_CLIPPED) || !(flags & ETO_OPAQUE))
        {
              /* Only draw if rectangle is not opaque or if some */
              /* text is outside the rectangle */
            if (!(flags & ETO_OPAQUE) ||
                (x < rect.left) ||
                (x + info.width >= rect.right) ||
                (y-font->ascent < rect.top) ||
                (y+font->descent >= rect.bottom))
            {
                XSetForeground( display, dc->u.x.gc, dc->w.backgroundPixel );
                XFillRectangle( display, dc->u.x.drawable, dc->u.x.gc,
                                dc->w.DCOrgX + x,
                                dc->w.DCOrgY + y - font->ascent,
                                info.width,
                                font->ascent + font->descent );
            }
        }
    }
    
    /* Draw the text (count > 0 verified) */

    XSetForeground( display, dc->u.x.gc, dc->w.textPixel );
    if (!dc->w.charExtra && !dc->w.breakExtra && !lpDx)
    {
        XDrawString( display, dc->u.x.drawable, dc->u.x.gc, 
                     dc->w.DCOrgX + x, dc->w.DCOrgY + y, str, count );
    }
    else  /* Now the fun begins... */
    {
        XTextItem *items, *pitem;
	int delta;

	/* allocate max items */

        pitem = items = xmalloc( count * sizeof(XTextItem) );
        delta = i = 0;
        while (i < count)
        {
	    /* initialize text item with accumulated delta */

            pitem->chars  = (char *)str + i;
	    pitem->delta  = delta; 
            pitem->nchars = 0;
            pitem->font   = None;
            delta = 0;

	    /* stuff characters into the same XTextItem until new delta 
	     * becomes  non-zero */

	    do
            {
                if (lpDx) delta += lpDx[i] - XTextWidth( font, str + i, 1);
                else
                {
                    delta += dc->w.charExtra;
                    if (str[i] == (char)dc->u.x.font.metrics.tmBreakChar)
                        delta += dc->w.breakExtra;
                }
                pitem->nchars++;
            } 
	    while ((++i < count) && !delta);
            pitem++;
        }

        XDrawText( display, dc->u.x.drawable, dc->u.x.gc,
                   dc->w.DCOrgX + x, dc->w.DCOrgY + y, items, pitem - items );
        free( items );
    }

      /* Draw underline and strike-out if needed */

    if (dc->u.x.font.metrics.tmUnderlined)
    {
	long linePos, lineWidth;       
	if (!XGetFontProperty( font, XA_UNDERLINE_POSITION, &linePos ))
	    linePos = font->descent-1;
	if (!XGetFontProperty( font, XA_UNDERLINE_THICKNESS, &lineWidth ))
	    lineWidth = 0;
	else if (lineWidth == 1) lineWidth = 0;
	XSetLineAttributes( display, dc->u.x.gc, lineWidth,
			    LineSolid, CapRound, JoinBevel ); 
        XDrawLine( display, dc->u.x.drawable, dc->u.x.gc,
		   dc->w.DCOrgX + x, dc->w.DCOrgY + y + linePos,
		   dc->w.DCOrgX + x + info.width, dc->w.DCOrgY + y + linePos );
    }
    if (dc->u.x.font.metrics.tmStruckOut)
    {
	long lineAscent, lineDescent;
	if (!XGetFontProperty( font, XA_STRIKEOUT_ASCENT, &lineAscent ))
	    lineAscent = font->ascent / 3;
	if (!XGetFontProperty( font, XA_STRIKEOUT_DESCENT, &lineDescent ))
	    lineDescent = -lineAscent;
	XSetLineAttributes( display, dc->u.x.gc, lineAscent + lineDescent,
			    LineSolid, CapRound, JoinBevel ); 
	XDrawLine( display, dc->u.x.drawable, dc->u.x.gc,
		   dc->w.DCOrgX + x, dc->w.DCOrgY + y - lineAscent,
		   dc->w.DCOrgX + x + info.width, dc->w.DCOrgY + y - lineAscent );
    }

    if (flags & ETO_CLIPPED) SelectClipRgn32( dc->hSelf, hRgnClip );
    return TRUE;
}
