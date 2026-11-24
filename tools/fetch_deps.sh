#!/bin/sh
# Download the third-party inputs into third_party/ (not kept in git).
# Run automatically by `make`; versions are pinned and checked so builds are reproducible.
#
#   bearssl/     BearSSL (MIT)                    TLS client; its brssl tool converts the CA bundle
#   qrcodegen/   qrcodegen (MIT)                  QR code for the LAN auth page
#   stb/         stb_image, stb_truetype (PD)     album art JPEGs; rasterising the UI fonts
#   cacert.pem   Mozilla CA bundle (MPL 2.0)      -> gen/trust_anchors.c
#   dejavu/      DejaVu Sans (Bitstream Vera)     -> gen/font_data.c
#
# Usage: tools/fetch_deps.sh [dest dir]   (default: third_party/)
set -e
DEST=${1:-$(dirname "$0")/../third_party}
mkdir -p "$DEST"
cd "$DEST"

BEARSSL_URL=https://www.bearssl.org/git/BearSSL
BEARSSL_REV=7bea48e5e850ab4cafbe68d3765cdaba13a86d6f

QRCODE_REV=9a217d443937f60cc56c1458c39a211d7f2d3af5
QRCODE_RAW=https://raw.githubusercontent.com/nayuki/QR-Code-generator/$QRCODE_REV/c

STB_REV=6b5799bae8a1fe3792691ef4b72ae26434c3e799
STB_RAW=https://raw.githubusercontent.com/nothings/stb/$STB_REV

# Mozilla's root store as extracted by the curl project (dated snapshot).
CACERT_URL=https://curl.se/ca/cacert-2026-11-05.pem

DEJAVU_VER=2.37
DEJAVU_URL=https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-$DEJAVU_VER.tar.bz2
DEJAVU_SHA=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7

fetch() { # url dest
    echo "  fetch $2" >&2
    curl -sSfL --retry 2 -m 120 -o "$2.tmp" "$1" && mv "$2.tmp" "$2"
}

if [ ! -f bearssl/inc/bearssl.h ]; then
    echo "  clone BearSSL @ $(echo $BEARSSL_REV | cut -c1-7)" >&2
    rm -rf bearssl.tmp
    git clone -q "$BEARSSL_URL" bearssl.tmp
    git -C bearssl.tmp -c advice.detachedHead=false checkout -q "$BEARSSL_REV"
    rm -rf bearssl.tmp/.git bearssl
    mv bearssl.tmp bearssl
fi

mkdir -p qrcodegen stb dejavu
[ -f qrcodegen/qrcodegen.c ] || fetch "$QRCODE_RAW/qrcodegen.c" qrcodegen/qrcodegen.c
[ -f qrcodegen/qrcodegen.h ] || fetch "$QRCODE_RAW/qrcodegen.h" qrcodegen/qrcodegen.h
[ -f stb/stb_image.h ] || fetch "$STB_RAW/stb_image.h" stb/stb_image.h
[ -f stb/stb_truetype.h ] || fetch "$STB_RAW/stb_truetype.h" stb/stb_truetype.h
[ -f cacert.pem ] || fetch "$CACERT_URL" cacert.pem

if [ ! -f dejavu/DejaVuSans.ttf ] || [ ! -f dejavu/DejaVuSans-Bold.ttf ]; then
    fetch "$DEJAVU_URL" dejavu.tar.bz2
    echo "$DEJAVU_SHA  dejavu.tar.bz2" | sha256sum -c --quiet
    d=dejavu-fonts-ttf-$DEJAVU_VER
    tar xjf dejavu.tar.bz2 $d/ttf/DejaVuSans.ttf $d/ttf/DejaVuSans-Bold.ttf $d/LICENSE
    mv $d/ttf/DejaVuSans.ttf $d/ttf/DejaVuSans-Bold.ttf $d/LICENSE dejavu/
    rm -rf $d dejavu.tar.bz2
fi

# Everything downloaded is checked against known hashes (BearSSL is pinned by commit).
sha256sum -c --quiet <<'SUMS'
6a2b9cc65176f2345dde260c74b6d352627e8a0a6385d086ae0e9c5d0913c70c  qrcodegen/qrcodegen.c
9eb73b8ba0037c86a25979385fd6d21fbfbb89e56853d05ccdcb2d2bb8e00da3  qrcodegen/qrcodegen.h
dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020  stb/stb_image.h
428cfe93520f2aed5e413a220b0db5bba5b17e2ebe5a64e4a8efa9eeeb67ec5f  stb/stb_truetype.h
427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285  cacert.pem
SUMS
