# Runs on the Raspberry Pi (sh -s, from ./ci hmi_sysroot): prints, one per line and relative to /, the files the
# cross build needs from the Pi: headers, the gcc runtime, crt objects, pkg-config files and the shared libraries
# ksphmi links against with everything they need (ldd closure), soname links and -dev links included.
# /lib is a link to usr/lib on Debian 13: every path is printed under usr/.
set -eu
L=usr/lib/aarch64-linux-gnu
cd /
# headers, and the targets of header links (trixie's asm/*.h point into usr/lib/linux/uapi)
find usr/include \( -type f -o -type l \) | while IFS= read -r f; do
  echo "$f"
  if [ -L "$f" ]; then real="$(readlink -f "/$f")"; echo "${real#/}"; fi
done
find usr/lib/gcc/aarch64-linux-gnu usr/share/pkgconfig "$L/pkgconfig" \( -type f -o -type l \)
# include directories kept under the library tree (glib-2.0/include and similar, named by .pc cflags)
find "$L" -mindepth 2 -path '*/include/*' \( -type f -o -type l \)
# crt objects, small static stubs (libc_nonshared.a, the empty libpthread.a/libdl.a of glibc >= 2.34)
find "$L" -maxdepth 1 \( -name '*.o' -o \( -name '*.a' -size -200k \) \) \( -type f -o -type l \)
# -dev links and linker scripts (libc.so, libm.so): links, or tiny text files
find "$L" -maxdepth 1 -name '*.so' \( -type l -o -size -8k \)
# and the libraries those linker scripts name (libm.so: libm.so.6 and libmvec.so.1)
find "$L" -maxdepth 1 -name '*.so' -type f -size -8k -exec grep -l '^GROUP\|^INPUT' {} + 2>/dev/null |
  xargs grep -ohE '/(usr/)?lib/[^ )]+' | sed 's#^/lib/#usr/lib/#; s#^/##' | sort -u |
  while IFS= read -r lib; do
    [ -e "/$lib" ] || continue
    echo "$lib"; real="$(readlink -f "/$lib")"; echo "${real#/}"
  done
ldd "$L/libSDL2-2.0.so.0" "$L/libSDL2_ttf-2.0.so.0" "$L/libstdc++.so.6" 2>/dev/null |
  sed -n 's#.*=> /\(.*\) (0x.*#\1#p; s#^[[:space:]]*/\([^ ]*ld-linux[^ ]*\) (0x.*#\1#p' |
  sed 's#^lib/#usr/lib/#' | sort -u |
  while IFS= read -r lib; do
    echo "$lib"
    real="$(readlink -f "/$lib")"
    echo "${real#/}"
  done
for lib in libSDL2-2.0.so.0 libSDL2_ttf-2.0.so.0 libstdc++.so.6; do
  echo "$L/$lib"; real="$(readlink -f "/$L/$lib")"; echo "${real#/}"
done
