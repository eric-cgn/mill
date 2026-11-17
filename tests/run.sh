#!/bin/sh
# Build and run the host tests of the core (with layout, run for run, against layout.sums, HTTP/1.1's replies and byte ranges, the settings
# migration, and mill://config's saving, on their own), the SMB, NFS, News, LDAP, and IMAP Plug-Ins' helpers,
# the Image Formats Plug-In's decoders (PNG's on its own too), and the container cores (gcc with
# warnings as errors and sanitizers), in the same pinned Retro68 image that builds Mill,
# which has a host gcc too. The core's allocations go through the tests' own functions,
# which can fail on purpose (see test_out_of_memory).
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/out
exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src \
  ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/t \
    -Dmalloc=t_malloc -Dcalloc=t_calloc -Drealloc=t_realloc \
    tests/test_core.c src/core/*.c src/third_party/tjpgd/tjpgd.c && /tmp/t && cp /tmp/out-*.pbm tests/out/ &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tl \
    tests/test_layout.c src/core/layout.c src/core/html.c src/core/charset.c && /tmp/tl &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/th \
    tests/test_http.c src/core/http11.c src/core/range.c && /tmp/th &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tmig \
    tests/test_migrate.c src/core/migrate.c && /tmp/tmig &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tcfg \
    tests/test_config.c src/core/config.c src/core/url.c src/core/filetypes.c && /tmp/tcfg &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/ts \
    tests/test_smb.c plugins/smb/smbutil.c && /tmp/ts &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tn \
    tests/test_nfs.c plugins/nfs/nfsutil.c && /tmp/tn &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tm \
    tests/test_mime.c plugins/mime/mime.c && /tmp/tm &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tw \
    tests/test_news.c plugins/news/newsutil.c && /tmp/tw &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tl \
    tests/test_ldap.c plugins/ldap/ldaputil.c && /tmp/tl &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/ti \
    tests/test_imap.c plugins/imap/imaputil.c && /tmp/ti &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tg \
    tests/test_images.c plugins/images/common.c plugins/images/macpaint.c plugins/images/pict.c plugins/images/xbm.c \
    plugins/images/bmp.c plugins/images/pnm.c plugins/images/png.c src/core/inflate.c -lm && /tmp/tg &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/tp \
    tests/test_png.c plugins/images/common.c plugins/images/macpaint.c plugins/images/pict.c plugins/images/xbm.c \
    plugins/images/bmp.c plugins/images/pnm.c plugins/images/png.c src/core/inflate.c && /tmp/tp &&
  gcc -std=c99 -Wall -Wextra -Werror -Wno-pointer-sign -fsanitize=address,undefined -g -o /tmp/ta \
    tests/test_archive.c plugins/archive/common.c plugins/archive/zip.c plugins/archive/tar.c plugins/archive/gzip.c plugins/archive/macbinary.c plugins/archive/binhex.c src/core/inflate.c src/core/charset.c src/core/macenc.c \
    src/core/filetypes.c && /tmp/ta'
