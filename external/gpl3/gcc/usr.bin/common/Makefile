#	$NetBSD: Makefile,v 1.9 2019/02/04 04:49:20 mrg Exp $

LIBISPRIVATE=	yes

# For ../Makefile.inc and bsd.own.mk
.include <bsd.init.mk>

LIB=		common

.include "${.CURDIR}/defs.mk"
SRCS=	${G_OBJS-libcommon:S/memory-block.o/memory-block.cc/:.o=.c}

# XXX make sure this doesn't have more than it needs
CPPFLAGS+=	-I${GCCARCH} -I${BACKENDOBJ} -I. \
		${G_ALL_CFLAGS:M-D*} ${G_INCLUDES:M-I*:N-I.*} \
		-DPREFIX=\"/usr\" \
		-DDEFAULT_TARGET_VERSION=\"${G_version}\" \
		-DDEFAULT_TARGET_MACHINE=\"${MACHINE_GNU_PLATFORM}\" \
		-DCONFIGURE_SPECS="\"\""

MKPIC:=		no
MKPICLIB:=	no

HOSTPROG_CXX=	1

COPTS.diagnostic.c=	-Wno-stack-protector
COPTS.intl.c=		-Wno-stack-protector

version.c: ${GCCARCH}/defs.mk ${TOP}/tools/gcc/gcc-version.mk

${SRCS}: ${GCCARCH}/defs.mk

.include <bsd.lib.mk>

# Force using C++ for this
HOST_CC:=	${HOST_CXX}
CC:=		${CXX}
CFLAGS:=	${CXXFLAGS}

.PATH: ${DIST}/gcc
