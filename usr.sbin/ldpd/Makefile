# $NetBSD: Makefile,v 1.4 2012/11/12 18:39:00 kefren Exp $

.include <bsd.own.mk>

PROG=   ldpd
MAN=    ldpd.8

SRCS=   conffile.c \
	fsm.c \
	label.c \
	ldp_command.c \
	ldp_errors.c \
	ldp_peer.c \
	main.c \
	mpls_interface.c \
	mpls_routes.c \
	notifications.c \
	pdu.c \
	socketops.c \
	tlv.c \
	tlv_stack.c

LDADD+=	-lcrypt

.if (${USE_INET6} != "no")
CPPFLAGS+=-DINET6
.endif

.include <bsd.prog.mk>
