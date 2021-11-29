cd openssl
./config enable-tls1_3 --prefix=$PWD/build
make -j$(nproc)
make install_sw

echo "starting NGCP2"
cd ../ngtcp2
autoreconf -i
./configure PKG_CONFIG_PATH=$PWD/../openssl/build/lib/pkgconfig LDFLAGS="-Wl,-rpath,$PWD/../openssl/build/lib"
make -j$(nproc) check
