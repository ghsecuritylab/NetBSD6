#	$NetBSD: Makefile,v 1.34 2012/11/15 19:49:12 jkunz Exp $

.include <bsd.own.mk>

SUBDIR=	acpica am-utils bind byacc cron dhcpcd elftosb fetch file flex less \
	libarchive libevent liblzf libpcap mdocml ntp openresolv tcpdump \
	tmux top tre wpa

.if (${MKATF} != "no")
SUBDIR+= atf
.endif
.if (${MKCRYPTO} != "no")
SUBDIR+= pkg_install ../../crypto/external/bsd
.endif
# IP Filter
.if (${MKIPFILTER} != "no")
SUBDIR+=ipf
.endif
.if (${MKISCSI} != "no")
SUBDIR+= iscsi 
.endif
.if (${MKLDAP} != "no")
SUBDIR+= openldap
.endif
.if (${MKLLVM} != "no")
SUBDIR+= llvm
.endif
.if (${MKPCC} != "no")
SUBDIR+= pcc
.endif

.include <bsd.subdir.mk>
