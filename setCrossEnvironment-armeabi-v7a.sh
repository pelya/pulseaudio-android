#!/bin/sh

IFS='
'

NDK=`which ndk-build`
NDK=`dirname $NDK`

if uname -s | grep -i "linux" > /dev/null ; then
	MYARCH=linux-$(arch)
  NDK=`readlink -f $NDK`
elif uname -s | grep -i "darwin" > /dev/null ; then
	MYARCH=darwin-x86_64
elif uname -s | grep -i "windows" > /dev/null ; then
	MYARCH=windows-x86_64
fi

#echo NDK $NDK
[ -z "$NDK_TOOLCHAIN_VERSION" ] && NDK_TOOLCHAIN_VERSION=4.9
LOCAL_PATH=`dirname $0`
if which realpath > /dev/null ; then
	LOCAL_PATH=`realpath $LOCAL_PATH`
else
	LOCAL_PATH=`cd $LOCAL_PATH && pwd`
fi
ARCH=armeabi-v7a
GCCPREFIX=armv7a-linux-androideabi
BINUTILSPREFIX=arm-linux-androideabi
APILEVEL=21


CFLAGS="
-g
-ffunction-sections
-fdata-sections
-funwind-tables
-fstack-protector-strong
-no-canonical-prefixes
-mthumb
-Wformat
-Werror=format-security
-Oz
-DNDEBUG
-fPIC
$CFLAGS"

CFLAGS="`echo $CFLAGS | tr '\n' ' '`"

LDFLAGS="
-fPIC
-g
-ffunction-sections
-fdata-sections
-Wl,--gc-sections
-funwind-tables
-fstack-protector-strong
-no-canonical-prefixes
-mthumb
-Wformat
-Werror=format-security
-Oz
-DNDEBUG
-Wl,--build-id
-Wl,--warn-shared-textrel
-Wl,--fatal-warnings
-Wl,--no-undefined
-Wl,-z,noexecstack
-Qunused-arguments
-Wl,-z,relro
-Wl,-z,now
-latomic
-lm
-ldl
$LDFLAGS"

LDFLAGS="`echo $LDFLAGS | tr '\n' ' '`"

CC="$NDK/toolchains/llvm/prebuilt/$MYARCH/bin/$GCCPREFIX$APILEVEL-clang"
CXX="$NDK/toolchains/llvm/prebuilt/$MYARCH/bin/$GCCPREFIX$APILEVEL-clang++"
CPP="$CC -E $CFLAGS"

env \
CFLAGS="$CFLAGS" \
CXXFLAGS="$CXXFLAGS $CFLAGS -frtti -fexceptions" \
LDFLAGS="$LDFLAGS" \
CC="$CC" \
CXX="$CXX" \
RANLIB="$NDK/toolchains/llvm/prebuilt/$MYARCH/bin/llvm-ranlib" \
LD="$CXX" \
AR="$NDK/toolchains/llvm/prebuilt/$MYARCH/bin/llvm-ar" \
CPP="$CPP" \
NM="$NDK/toolchains/llvm/prebuilt/$MYARCH/bin/llvm-nm" \
AS="$NDK/toolchains/llvm/prebuilt/$MYARCH/bin/$BINUTILSPREFIX-as" \
STRIP="$NDK/toolchains/llvm/prebuilt/$MYARCH/bin/llvm-strip" \
"$@"
