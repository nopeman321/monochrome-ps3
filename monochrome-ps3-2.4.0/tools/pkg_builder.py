#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Wrapper de compatibilidad para el empaquetador PKG de PS3.

La implementacion real vive en tools/pkg3.py (formato PKG3 de PS3, variante
"debug / no finalizada", que es la que instala en hardware real PS3HEN/CFW).

Se mantiene la interfaz historica:
    python tools/pkg_builder.py <carpeta_origen> <salida.pkg> [content_id]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pkg3  # noqa: E402

DEFAULT_CONTENT_ID = "UP0001-MCRM00001_00-MONOCHROMEPS3001"


def build_pkg(source_dir, output_pkg, content_id=DEFAULT_CONTENT_ID):
    print("[+] Building PS3 Debug PKG from: %s" % source_dir)
    print("[+] Content-ID: %s" % content_id)
    pkg3.build_pkg(source_dir, output_pkg, content_id)
    size = os.path.getsize(output_pkg)
    print("[SUCCESS] PS3 Debug PKG generated (%d bytes): %s" % (size, output_pkg))
    return 0


if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else 'pkg_build'
    dst = sys.argv[2] if len(sys.argv) > 2 else 'MCRM00001.pkg'
    cid = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_CONTENT_ID
    sys.exit(build_pkg(src, dst, cid))
