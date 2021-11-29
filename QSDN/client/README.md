UDP based OpenFlow UDP Based RYU is developed in SIOTLAB Santa Clara University. Author of this feature is Puneet Kumar(PhD Student), Computer Science Santa Clara University. Advisor : Behnam Dezfouli

This feature is purely experimental based and would require some work if it has to be scaled. Please use the OVS in this git repo as normal OVS wouldn't support UDP and hence would not work.

RYU is based on existing RYU/TCP and in addition to changing the transportation, this feature is heavily modified for greenio sockets and event handlers.

RYU: It will start a UDP server on 6653 port for ryu as controller

OVS: OVS will start as usual however, please use "ovs-vsctl set-controller udp::6653" in order to talk to RYU

# openflow_quic
OVS Related configuration:
Install python3.4:
1. sudo apt update
2. sudo apt install python3-pip
3. sudo apt install python-pip
4. sudo apt-get install build-essential checkinstall
5. sudo apt-get install libreadline-gplv2-dev libncursesw5-dev libssl-dev libsqlite3-dev tk-dev libgdbm-dev libc6-dev libbz2-dev
6. cd /usr/src
7. sudo wget https://www.python.org/ftp/python/3.4.4/Python-3.4.4.tgz
8. sudo tar xzf Python-3.4.4.tgz
9. cd Python-3.4.4
10. sudo ./configure
11. make
12. sudo make install
13. sudo apt-get install dh-autoreconf
14. sudo apt-get install libev-dev

The following packages are required to configure the build system:

pkg-config >= 0.20
autoconf
automake
autotools-dev
libtool
libngtcp2 uses cunit for its unit test frame work:

cunit >= 2.1
To build sources under the examples directory, libev is required:

libev
The client and server under examples directory require OpenSSL (master branch) as crypto backend:

next steps are same for client and server. Go to each folder and run the following 
1. cd openssl
2. ./config enable-tls1_3 --prefix=$PWD/build
3. make -j$(nproc)
4. make install_sw
5. cd client or server whichever folder you are in
6. autoreconf -i
7. ./configure PKG_CONFIG_PATH=$PWD/../openssl/build/lib/pkgconfig LDFLAGS="-Wl,-rpath,$PWD/../openssl/build/lib"
8. make -j$(nproc) check
