#!/usr/bin/env python3
"""
Layout de bits del docid versionado empaquetado: master|rel en un uint64.

Fuente de verdad: `version_packing.h` del motor uiHRDC (ZDD_MASTER_BITS /
ZDD_REL_BITS), el mismo header que compilan uiHRDC y plus_t. Las variables de
entorno ZDD_MASTER_BITS / ZDD_REL_BITS replican un override de compilación
(-DZDD_MASTER_BITS=...) para que Python y C++ no se desincronicen.

Uso:
  from packed64_layout import MASTER_BITS, REL_BITS, pack, unpack
  python3 scripts/packed64_layout.py          # imprime el layout vigente
  ZDD_REL_BITS=20 ZDD_MASTER_BITS=44 python3 scripts/packed64_layout.py
"""
from __future__ import annotations

import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (
    ROOT / "uiHRDC" / "uiHRDC" / "indexes" / "NOPOS" / "II_docs" / "src" / "utils"
    / "version_packing.h"
)
PACKED_BITS = 64
_DEFAULTS = {"ZDD_MASTER_BITS": 40, "ZDD_REL_BITS": 24}


def _from_header(name: str) -> int | None:
    try:
        text = HEADER.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(rf"^\s*#define\s+{name}\s+(\d+)u?\s*$", text, re.MULTILINE)
    return int(match.group(1)) if match else None


def _resolve(name: str) -> int:
    env = os.environ.get(name)
    if env:
        return int(env)
    return _from_header(name) or _DEFAULTS[name]


MASTER_BITS = _resolve("ZDD_MASTER_BITS")
REL_BITS = _resolve("ZDD_REL_BITS")

if MASTER_BITS + REL_BITS != PACKED_BITS:
    raise ValueError(
        f"layout invalido: master_bits({MASTER_BITS}) + rel_bits({REL_BITS}) "
        f"debe ser {PACKED_BITS}"
    )
if MASTER_BITS < 1 or REL_BITS < 1:
    raise ValueError("layout invalido: ambos campos necesitan al menos 1 bit")

MASTER_SHIFT = REL_BITS
MASTER_MASK = (1 << MASTER_BITS) - 1
REL_MASK = (1 << REL_BITS) - 1
UNIVERSE_SIZE = 1 << MASTER_BITS
MAX_VERSIONS = REL_MASK


def pack(master: int, rel: int) -> int:
    if not 0 <= master <= MASTER_MASK:
        raise ValueError(f"master fuera de rango {MASTER_BITS}-bit: {master}")
    if not 0 <= rel <= REL_MASK:
        raise ValueError(f"rel fuera de rango {REL_BITS}-bit: {rel}")
    return (master << MASTER_SHIFT) | rel


def unpack(packed: int) -> tuple[int, int]:
    return packed >> MASTER_SHIFT, packed & REL_MASK


def meta_fields() -> dict[str, int]:
    """Claves de layout para los .docs.meta (mismo nombre en todo el flujo)."""
    return {"master_bits": MASTER_BITS, "rel_bits": REL_BITS}


def describe() -> str:
    source = "env" if os.environ.get("ZDD_MASTER_BITS") or os.environ.get("ZDD_REL_BITS") else HEADER
    return (
        f"packed64: master={MASTER_BITS}b rel={REL_BITS}b "
        f"universo=2^{MASTER_BITS} versiones_max={MAX_VERSIONS} (fuente: {source})"
    )


if __name__ == "__main__":
    print(describe())
