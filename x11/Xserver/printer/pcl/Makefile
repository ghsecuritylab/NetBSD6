#	$NetBSD: Makefile,v 1.3 2003/09/30 06:41:30 lukem Exp $

.include "../../Makefile.serverlib"

.include <bsd.own.mk>

LIB=		pcl
.PATH:		${X11SRCDIR.xc}/programs/Xserver/Xprint/pcl

SRCS=		PclArc.c PclArea.c PclAttVal.c PclAttr.c PclColor.c \
		PclCursor.c PclFonts.c PclGC.c PclInit.c PclLine.c \
		PclMisc.c PclPixel.c PclPixmap.c PclPolygon.c \
		PclPrint.c PclSFonts.c PclSpans.c PclText.c PclWindow.c

CPPFLAGS+=	-I${DESTDIR}${X11INCDIR}/X11 \
		-I${X11SRCDIR.xc}/programs/Xserver/Xprint \
		-I${X11SRCDIR.xc}/programs/Xserver/include \
		-I${X11SRCDIR.xc}/include/fonts \
		-I${X11SRCDIR.xc}/programs/Xserver/mi \
		-I${X11SRCDIR.xc}/programs/Xserver/mfb \
		-I${X11SRCDIR.xc}/programs/Xserver/cfb

CPPFLAGS+=	-DPSZ=8 -DXP_PCL_COLOR -UXFree86LOADER -D_XP_PRINT_SERVER_

.include <bsd.x11.mk>
.include <bsd.lib.mk>
