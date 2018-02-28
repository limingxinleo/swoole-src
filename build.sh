ARCH=
PHPIZE_BIN=$(command -v phpize 2>/dev/null)
PHPCONFIG_BIN=$(command -v php-config 2>/dev/null)

# Translate long options to short
for arg in "$@"; do
  shift
  case "$arg" in
    "--arch") set -- "$@" "-a" ;;
    "--phpize") set -- "$@" "-i" ;;
    "--php-config") set -- "$@" "-c" ;;
    *) set -- "$@" "$arg"
  esac
done

# Options switcher
while getopts a:i:c: opts; do
   case ${opts} in
      a) ARCH=${OPTARG} ;;
      i) PHPIZE_BIN=${OPTARG} ;;
      c) PHPCONFIG_BIN=${OPTARG} ;;
   esac
done

git submodule update --init --recursive
cd thirdparty/hiredis
make 
sudo make install
cd ../nghttp2
cmake .
make
sudo make install
sudo ldconfig
cd ../..
$PHPIZE_BIN --clean
$PHPIZE_BIN
make clean
./configure --enable-openssl --enable-sockets --enable-async-redis --enable-mysqlnd --enable-http2
make -j
make install

