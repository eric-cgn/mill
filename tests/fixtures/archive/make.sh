#!/bin/sh
# Test archives for the container cores (tests/test_archive.c), made with the standard tools
# (Info-ZIP zip, tar, gzip) and, for what those don't make (MacBinary, BinHex, archives with
# hostile names, a gzip header with every field), make.py from the published specifications.
# ./make.sh rewrites everything here. Dates are Christmas Day 1997, UTC.
set -e
cd "$(dirname "$0")"
export TZ=UTC LC_ALL=C
rm -rf src *.zip *.tar *.gz *.tgz *.bin *.hqx
mkdir -p src/docs/sub
python3 - <<'PY'
import random
with open("src/hello.txt", "w") as f: f.write("Hello from the archive.\n")
with open("src/docs/readme.txt", "w") as f:
    for i in range(300): f.write("Line %d of the readme, with some words to squeeze: mill, wind, sail, grain.\n" % i)
open("src/docs/empty.txt", "w").close()
r = random.Random(1997)
with open("src/bin.dat", "wb") as f: f.write(bytes(r.randrange(256) for _ in range(20000)))
PY
ln -s hello.txt src/link
find src -exec touch -h -t 199712250000 {} +
(cd src &&
 zip -q -X -r ../kit.zip hello.txt docs bin.dat &&
 zip -q -X -0 -r ../stored.zip hello.txt docs &&
 zip -q -X -y ../link.zip link hello.txt &&
 echo "A comment for the archive." | zip -q -X -z ../comment.zip hello.txt &&
 zip -q -X -P secret ../encrypted.zip hello.txt &&
 zip -q -X -fz ../zip64.zip hello.txt &&
 tar --format ustar -cf ../kit.tar hello.txt docs bin.dat link &&
 tar --format v7tar -cf ../v7.tar hello.txt docs &&
 gzip -c -9 docs/readme.txt > ../readme.txt.gz &&
 gzip -c -n < hello.txt > ../noname.gz)
gzip -c -n < kit.tar > kit.tgz
python3 make.py
