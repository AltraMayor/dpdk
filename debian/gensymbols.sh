#!/bin/bash

v=19.08
p=libgkrte
d=debian
for i in $(grep 'Package: libgkrte' $d/control|cut -d- -f2-|sed "s/$v//"); do
  dpkg-gensymbols \
    -v${v} \
    -p${p}-${i}${v} \
    -P${d}/${p}-${i}${v} \
    -O${d}/${p}-${i}${v}.symbols
done
