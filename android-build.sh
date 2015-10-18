#!/bin/sh

NCPU=4
ARCH_LIST="armeabi x86 mips"

[ -e libtool-2.4.6.tar.gz ] || wget http://ftpmirror.gnu.org/libtool/libtool-2.4.6.tar.gz || exit 1
[ -e 12916e229c769da4929f6df7f038ab51cf0cb067.tar.gz ] || wget https://github.com/json-c/json-c/archive/12916e229c769da4929f6df7f038ab51cf0cb067.tar.gz || exit 1
[ -e libsndfile-1.0.25.tar.gz ] || wget http://www.mega-nerd.com/libsndfile/files/libsndfile-1.0.25.tar.gz || exit 1

[ -e configure ] || {
	env NOCONFIGURE=1 ./bootstrap.sh || exit 1
}

for ARCH in $ARCH_LIST; do

	case $ARCH in
		x86) TOOLCHAIN=i686-linux-android;;
		mips) TOOLCHAIN=mipsel-linux-android;;
		*) TOOLCHAIN=arm-linux-androideabi;;
	esac

	mkdir -p $ARCH
	cd $ARCH

	[ -e libtool-2.4.6/install/lib/libltdl.a ] || {
		rm -rf libtool-2.4.6
		tar xvf ../libtool-2.4.6.tar.gz || exit 1
		cd libtool-2.4.6
		mkdir -p install
		../../setCrossEnvironment-$ARCH.sh ./configure --host=$TOOLCHAIN --prefix=`pwd`/install --disable-shared || exit 1
		make -j$NCPU || exit 1
		make install || exit 1
		cd ..
	} || exit 1

	[ -e json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/lib/libjson-c.a ] || {
		rm -rf json-c-12916e229c769da4929f6df7f038ab51cf0cb067
		tar xvf ../12916e229c769da4929f6df7f038ab51cf0cb067.tar.gz || exit 1
		cd json-c-12916e229c769da4929f6df7f038ab51cf0cb067
		mkdir -p install
		env ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes \
		../../setCrossEnvironment-$ARCH.sh ./autogen.sh \
			--host=$TOOLCHAIN \
			--prefix=`pwd`/install \
			--disable-shared \
			--with-gnu-ld \
			|| exit 1
		make -j$NCPU || exit 1
		make install || exit 1
		cd ..
	} || exit 1

	[ -e libsndfile-1.0.25/install/lib/libsndfile.a ] || {
		rm -rf libsndfile-1.0.25
		tar xvf ../libsndfile-1.0.25.tar.gz || exit 1
		cd libsndfile-1.0.25
		mkdir -p install
		cp -f ../../android-config.sub Cfg/config.sub
		cp -f ../../android-config.guess Cfg/config.guess
		autoreconf
		../../setCrossEnvironment-$ARCH.sh ./configure \
			--host=$TOOLCHAIN \
			--prefix=`pwd`/install \
			--disable-shared \
			--disable-external-libs \
			--disable-alsa \
			|| exit 1
		echo 'int main () {}' > programs/sndfile-play.c
		make -j$NCPU || exit 1
		make install || exit 1
		cd ..
	} || exit 1

	mkdir -p install

	[ -e Makefile ] || {
		env \
		CFLAGS="-I`pwd`/libtool-2.4.6/install/include \
			-I`pwd`/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/include/json-c \
			-I`pwd`/libsndfile-1.0.25/install/include" \
		LDFLAGS="-L`pwd`/libtool-2.4.6/install/lib \
			-L`pwd`/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/lib \
			-L`pwd`/libsndfile-1.0.25/install/lib -pie" \
		LIBJSON_CFLAGS=-I`pwd`/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/include \
		LIBJSON_LIBS="-L`pwd`/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/lib -ljson-c" \
		LIBSNDFILE_CFLAGS=-I`pwd`/libsndfile-1.0.25/install/include \
		LIBSNDFILE_LIBS="-L`pwd`/libsndfile-1.0.25/install/lib -lsndfile" \
		../setCrossEnvironment-$ARCH.sh \
		  ../configure            \
		  --prefix=`pwd`/install  \
		  --host=$TOOLCHAIN       \
		  --disable-nls           \
		  --disable-rpath         \
		  --disable-neon-opt      \
		  --enable-static         \
		  --disable-shared        \
		  --disable-x11           \
		  --disable-tests         \
		  --disable-oss-output    \
		  --disable-oss-wrapper   \
		  --disable-coreaudio-output \
		  --disable-alsa          \
		  --disable-esound        \
		  --disable-solaris       \
		  --disable-glib2         \
		  --disable-gtk3          \
		  --disable-gconf         \
		  --disable-avahi         \
		  --disable-jack          \
		  --disable-asyncns       \
		  --disable-tcpwrap       \
		  --disable-lirc          \
		  --disable-dbus          \
		  --disable-bluez4        \
		  --disable-bluez5        \
		  --disable-udev          \
		  --disable-hal-compat    \
		  --disable-openssl       \
		  --disable-xen           \
		  --disable-systemd-daemon \
		  --disable-systemd-login \
		  --disable-systemd-journal \
		  --disable-manpages      \
		  --enable-static-bins    \
		  --enable-force-preopen  \
		  --without-caps          \
		|| exit 1
	} || exit 1

	make -j$NCPU V=1 || exit 1
	make install || exit 1
	../setCrossEnvironment-$ARCH.sh sh -c '$STRIP --strip-unneeded install/bin/pulseaudio' || exit 1
	cd ..
done
