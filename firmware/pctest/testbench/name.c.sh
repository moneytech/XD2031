#!/bin/sh
# http://en.wikipedia.org/wiki/Here_document

TESTFILE=name
CFLAGS="-Wall -std=c99"
INCLUDE="-I.. -I../.. -I../../../common"

cc -D PCTEST $INCLUDE $CFLAGS ../../$TESTFILE.c ../mains/$TESTFILE.c ../../cmdnames.c -o ../bin/$TESTFILE || exit 1

../bin/$TESTFILE << "EOF"
# To set the parsehint parameter, give a
!PARSEHINT_COMMAND
# or
!PARSEHINT_LOAD
# without any trailing or leading spaces.
# On unknown commands or known ones with (trailing) whitespaces
# you should get a syntax error.
!NOT-A-COMMAND  
#
#
# LOAD"TEST",8
TEST
# LOAD"0:TEST",8
0:TEST
# LOAD":TEST",8
:TEST
# LOAD"FTP:TEST",8
FTP:TEST
# Filename beginning with digit
!PARSEHINT_LOAD
1TEST
!PARSEHINT_COMMAND
1TEST
!PARSEHINT_COMMAND
# RENAME
# Please note though no drive is given for OLD, the parser reports
# the same drive as in NEW, which makes perfect sense for RENAME
R:NEW=OLD
# Some CD variations
CD 0:NAME
CD:NAME
CD NAME
CD FTP:NAME
!PARSEHINT_LOAD
NAME,SEQ,WRITE
NAME,S,W
NAME,R,U
NAME,R,P
NAME,N
RELFILE,L,?
RELFILE,L,?@
EOF
exit 0
