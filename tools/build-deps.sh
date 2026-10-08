#!/usr/bin/env bash
set -euo pipefail
mkdir -p /tmp/orbit-deps
cd /tmp/orbit-deps
fetch() {
  if ! test -f "$2"; then curl --silent --show-error --fail --location --retry 3 "$1" -o "$2"; fi
  printf '%s  %s\n' "$3" "$2" | sha256sum -c -
  tar xf "$2"
}
fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz openssl.tar.gz c53a47e5e441c930c3928cf7bf6fb00e5d129b630e0aa873b08258656e7345ec
fetch https://curl.se/download/curl-8.18.0.tar.xz curl.tar.xz 40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946
fetch https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz microhttpd.tar.gz a89e09fc9b4de34dde19f4fcb4faaa1ce10299b9908db1132bbfa1de47882b94
source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
export CPPFLAGS="-I$PS5_SYSROOT/user/homebrew/include"
export LDFLAGS="-L$PS5_SYSROOT/user/homebrew/lib"
cd /tmp/orbit-deps/openssl-3.5.2
./Configure BSD-x86_64 no-tests no-apps no-shared --prefix="$PREFIX"
make -j4 build_sw
make install_sw DESTDIR="$PS5_SYSROOT"
cd /tmp/orbit-deps/curl-8.18.0
./configure --prefix="$PREFIX" --host=x86_64-pc-freebsd --enable-static --disable-shared --with-openssl="$PS5_SYSROOT/user/homebrew" --without-zlib --without-brotli --without-zstd --without-libpsl --without-libidn2 --without-libssh2 --disable-ldap --disable-ldaps --disable-docs --disable-manual --disable-ftp --disable-file --disable-dict --disable-gopher --disable-imap --disable-mqtt --disable-pop3 --disable-rtsp --disable-smb --disable-smtp --disable-telnet --disable-tftp
make -j4
make install DESTDIR="$PS5_SYSROOT"
cd /tmp/orbit-deps/libmicrohttpd-1.0.1
./configure --prefix="$PREFIX" --host=x86_64-pc-freebsd --enable-static --disable-shared --disable-doc --disable-curl --disable-examples --disable-https
make -j4
make install DESTDIR="$PS5_SYSROOT"
