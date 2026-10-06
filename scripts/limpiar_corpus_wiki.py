#!/usr/bin/env python3
"""
Paso 0 del pipeline: limpia wikitext de MediaWiki preservando lineas 1:1.

Cada linea del .txt es una revision/documento. page_mapping.bin indexa por
numero de linea (global_id), asi que NO se pueden fusionar ni eliminar lineas:
solo reemplazar el contenido (linea vacia si queda vacia).

Backends (strip wikitext -> prosa):
  Produccion/referencia: regex, mwph, wtp.
  Variantes robustas: *_u2 (doble html.unescape), *_html y *_regex.
  Extractores adicionales: wikiextractor, pywikibot y motores HTML.

Niveles:
  marcado  — quita wikitext/HTML, conserva mayusculas y puntuacion de prosa
  torsen   — marcado + minusculas + solo [a-z0-9 ] (formato torsen)

Salida:
  <out>.txt
  <out>.txt.DOCBOUNDARIES.ul  (offsets u64 little-endian, ndocs+1 entradas)
"""

from __future__ import annotations

import argparse
import html
import multiprocessing as mp
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path
Backend = str
BACKENDS: tuple[Backend, ...] = (
    "regex",
    "mwph",
    "mwph_u2",
    "wtp",
    "wtp_u2",
    "wikiextractor",
    "pywikibot",
    "html_stdlib",
    "html_lxml",
    "html_bs4",
    "html5lib",
    "html_selectolax",
    "html_inscriptis",
    "mwph_u2_html",
    "wtp_u2_html",
    "wikiextractor_html",
    "mwph_u2_regex",
    "wtp_u2_regex",
    "wikiextractor_regex",
    "html_regex",
    "pandoc",
)

# ---------------------------------------------------------------------------
# Regex de limpieza (backend regex)
# ---------------------------------------------------------------------------

RE_COMMENT = re.compile(r"<!--.*?-->", re.S)
RE_NOWIKI = re.compile(r"<nowiki>.*?</nowiki>", re.I | re.S)
RE_REF = re.compile(r"<ref\b[^>]*>.*?</ref>", re.I | re.S)
RE_REF_SELF = re.compile(r"<ref\b[^>]*/>", re.I)
RE_TAGBLOCK = re.compile(
    r"<(math|code|pre|gallery|table|div|span|font)\b[^>]*>.*?</\1>", re.I | re.S
)
RE_TAG = re.compile(r"</?[A-Za-z][^>]*/?>")
RE_TEMPLATE = re.compile(r"\{\{[^{}]*\}\}")
RE_TABLE = re.compile(r"\{\|.*?\|\}", re.S)
RE_NSLINK = re.compile(
    r"\[\[(?:Category|Image|File|Media|Template|Help|Portal)\s*:[^\[\]]*\]\]",
    re.I,
)
RE_LINKPIPE = re.compile(r"\[\[(?:[^\[\]|]*\|)?([^\[\]|]*)\]\]")
RE_EXTLINK = re.compile(r"\[(?:https?|ftp)://[^\s\]]+\s*([^\]]*)\]")
RE_URL = re.compile(r"(?:https?|ftp)://\S+")
RE_BOLDIT = re.compile(r"'{2,5}")
RE_HEADING = re.compile(r"={1,6}\s*([^=]+?)\s*={1,6}")
RE_TABLE_RESIDUE = re.compile(r"[|!][-+]+\|?|\|\}|^\|\s*", re.M)
RE_PIPE_RUN = re.compile(r"\|{2,}")
RE_WS = re.compile(r"[ \t]+")
RE_NONALNUM = re.compile(r"[^0-9a-z]+")

# Backend activo en workers (multiprocessing)
_WORKER_BACKEND: Backend = "regex"


def unescape2(text: str) -> str:
    """Los dumps traen con frecuencia HTML escapado una o dos veces."""
    return html.unescape(html.unescape(text))


def normalize_ws(text: str) -> str:
    return RE_WS.sub(" ", text.replace("\n", " ").replace("\r", " ")).strip()


def strip_markup_regex(text: str) -> str:
    """Backend regex: quita marcado wiki/HTML, conserva prosa."""
    t = html.unescape(html.unescape(text))
    t = RE_COMMENT.sub(" ", t)
    t = RE_NOWIKI.sub(" ", t)
    t = RE_REF.sub(" ", t)
    t = RE_REF_SELF.sub(" ", t)
    t = RE_TAGBLOCK.sub(" ", t)
    t = RE_TABLE.sub(" ", t)
    for _ in range(6):
        prev = t
        t = RE_TEMPLATE.sub(" ", t)
        if t == prev:
            break
    t = RE_NSLINK.sub(" ", t)
    for _ in range(3):
        prev = t
        t = RE_LINKPIPE.sub(r"\1", t)
        if t == prev:
            break
    t = RE_EXTLINK.sub(r"\1", t)
    t = RE_URL.sub(" ", t)
    t = RE_TAG.sub(" ", t)
    t = RE_HEADING.sub(r" \1 ", t)
    t = RE_BOLDIT.sub("", t)
    t = RE_TABLE_RESIDUE.sub(" ", t)
    t = RE_PIPE_RUN.sub(" ", t)
    t = t.replace("[[", " ").replace("]]", " ")
    t = t.replace("{{", " ").replace("}}", " ")
    t = t.replace("{|", " ").replace("|}", " ")
    return RE_WS.sub(" ", t).strip()


def strip_markup_mwph(text: str) -> str:
    """Backend mwparserfromhell: strip_code estandar MediaWiki."""
    import mwparserfromhell as mw

    return str(mw.parse(text).strip_code(normalize=True, collapse=True)).strip()


def strip_markup_mwph_u2(text: str) -> str:
    """mwparserfromhell después de revelar tags HTML escapados."""
    import mwparserfromhell as mw

    code = mw.parse(unescape2(text), skip_style_tags=True)
    return str(code.strip_code(normalize=True, collapse=True)).strip()


def strip_markup_wtp(text: str) -> str:
    """Backend wikitextparser: plain_text con plantillas/enlaces/tags."""
    import wikitextparser as wtp

    return wtp.parse(text).plain_text(
        replace_templates=True,
        replace_wikilinks=True,
        replace_tags=True,
        unescape_html_entities=True,
    ).strip()


def strip_markup_wtp_u2(text: str) -> str:
    import wikitextparser as wtp

    return wtp.parse(unescape2(text)).plain_text(
        replace_templates=True,
        replace_parser_functions=True,
        replace_parameters=True,
        replace_tags=True,
        replace_external_links=True,
        replace_wikilinks=True,
        unescape_html_entities=True,
        replace_bolds_and_italics=True,
        replace_tables=True,
    ).strip()


def strip_markup_wikiextractor(text: str) -> str:
    """API interna de WikiExtractor aplicada a una revisión independiente."""
    from wikiextractor.extract import Extractor, clean, compact

    extractor = Extractor("0", "0", "", "revision", [])
    parts = compact(
        clean(extractor, unescape2(text), expand_templates=False, html_safe=False),
        extractor=extractor,
    )
    return normalize_ws(" ".join(parts))


def strip_markup_pywikibot(text: str) -> str:
    """Limpiador offline de Pywikibot; no consulta la API."""
    from pywikibot import textlib

    t = unescape2(text)
    t = textlib.removeDisabledParts(
        t,
        tags=[
            "comment",
            "includeonly",
            "nowiki",
            "pre",
            "syntaxhighlight",
            "ref",
            "gallery",
            "math",
            "template",
        ],
    )
    t = textlib.removeHTMLParts(t)
    t = textlib.removeCategoryLinks(t)
    return normalize_ws(t)


class _PlainHTMLParser:
    """Extractor HTML stdlib que conserva texto y descarta bloques no-prosa."""

    DROP = {"script", "style", "ref", "references", "gallery", "math", "code", "pre", "table"}

    def __init__(self) -> None:
        from html.parser import HTMLParser

        class Parser(HTMLParser):
            def __init__(self) -> None:
                super().__init__(convert_charrefs=True)
                self.depth = 0
                self.parts: list[str] = []

            def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
                if self.depth:
                    self.depth += 1
                elif tag.lower() in _PlainHTMLParser.DROP:
                    self.depth = 1
                elif tag.lower() in {"br", "p", "div", "li", "tr", "h1", "h2", "h3", "h4"}:
                    self.parts.append(" ")

            def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
                if not self.depth and tag.lower() in {"br", "hr"}:
                    self.parts.append(" ")

            def handle_endtag(self, tag: str) -> None:
                if self.depth:
                    self.depth -= 1
                elif tag.lower() in {"p", "div", "li", "tr", "h1", "h2", "h3", "h4"}:
                    self.parts.append(" ")

            def handle_data(self, data: str) -> None:
                if not self.depth:
                    self.parts.append(data)

        self.parser = Parser()

    def text(self, source: str) -> str:
        self.parser.feed(source)
        self.parser.close()
        return normalize_ws("".join(self.parser.parts))


def strip_html_stdlib(text: str) -> str:
    return _PlainHTMLParser().text(unescape2(text))


def strip_html_lxml(text: str) -> str:
    from lxml import html as lhtml

    root = lhtml.fragment_fromstring(unescape2(text), create_parent="div")
    for node in root.xpath(".//script|.//style|.//ref|.//references|.//gallery|.//math|.//code|.//pre|.//table"):
        node.drop_tree()
    return normalize_ws(root.text_content())


def strip_html_bs4(text: str, parser: str = "lxml") -> str:
    from bs4 import BeautifulSoup

    soup = BeautifulSoup(unescape2(text), parser)
    for node in soup.find_all(["script", "style", "ref", "references", "gallery", "math", "code", "pre", "table"]):
        node.decompose()
    return normalize_ws(soup.get_text(" "))


def strip_html_selectolax(text: str) -> str:
    from selectolax.parser import HTMLParser

    tree = HTMLParser(unescape2(text))
    for selector in ("script", "style", "ref", "references", "gallery", "math", "code", "pre", "table"):
        for node in tree.css(selector):
            node.decompose()
    return normalize_ws(tree.text(separator=" "))


def strip_html_inscriptis(text: str) -> str:
    from inscriptis import get_text

    return normalize_ws(get_text(unescape2(text)))


def strip_markup_pandoc(text: str) -> str:
    """Referencia lenta; invoca Pandoc por revisión."""
    proc = subprocess.run(
        ["pandoc", "-f", "mediawiki", "-t", "plain", "--wrap=none"],
        input=unescape2(text),
        text=True,
        capture_output=True,
        check=True,
    )
    return normalize_ws(proc.stdout)


def resolve_stripper(backend: Backend) -> Callable[[str], str]:
    direct: dict[str, Callable[[str], str]] = {
        "regex": strip_markup_regex,
        "mwph": strip_markup_mwph,
        "mwph_u2": strip_markup_mwph_u2,
        "wtp": strip_markup_wtp,
        "wtp_u2": strip_markup_wtp_u2,
        "wikiextractor": strip_markup_wikiextractor,
        "pywikibot": strip_markup_pywikibot,
        "html_stdlib": strip_html_stdlib,
        "html_lxml": strip_html_lxml,
        "html_bs4": strip_html_bs4,
        "html5lib": lambda t: strip_html_bs4(t, "html5lib"),
        "html_selectolax": strip_html_selectolax,
        "html_inscriptis": strip_html_inscriptis,
        "pandoc": strip_markup_pandoc,
    }
    if backend in direct:
        return direct[backend]
    cascades: dict[str, tuple[Callable[[str], str], Callable[[str], str]]] = {
        "mwph_u2_html": (strip_markup_mwph_u2, strip_html_lxml),
        "wtp_u2_html": (strip_markup_wtp_u2, strip_html_lxml),
        "wikiextractor_html": (strip_markup_wikiextractor, strip_html_lxml),
        "mwph_u2_regex": (strip_markup_mwph_u2, strip_markup_regex),
        "wtp_u2_regex": (strip_markup_wtp_u2, strip_markup_regex),
        "wikiextractor_regex": (strip_markup_wikiextractor, strip_markup_regex),
        "html_regex": (strip_html_lxml, strip_markup_regex),
    }
    if backend in cascades:
        first, second = cascades[backend]
        return lambda text: second(first(text))
    raise ValueError(f"backend desconocido: {backend}")


def strip_markup(text: str, backend: Backend = "regex") -> str:
    return resolve_stripper(backend)(text)


def to_torsen(text: str) -> str:
    """Nivel B: formato torsen (minusculas, solo alfanumerico + espacio)."""
    return RE_NONALNUM.sub(" ", text.lower()).strip()


def clean_line(raw: str, nivel: str, backend: Backend = "regex") -> str:
    line = raw.rstrip("\r\n")
    try:
        cleaned = strip_markup(line, backend)
    except Exception:
        # Una revision malformada no debe tumbar el corpus 1:1.
        cleaned = strip_markup_regex(line) if backend != "regex" else ""
    if nivel == "torsen":
        cleaned = to_torsen(cleaned)
    return cleaned


def _worker_init(backend: Backend) -> None:
    global _WORKER_BACKEND
    _WORKER_BACKEND = backend


def _worker_chunk(args: tuple[int, list[str], str]) -> tuple[int, list[str]]:
    idx, lines, nivel = args
    return idx, [clean_line(ln, nivel, _WORKER_BACKEND) for ln in lines]


def load_page_mapping(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) % 4 != 0:
        raise ValueError(f"page_mapping invalido (bytes no multiplo de 4): {path}")
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def verify_page_mapping(page_map: list[int], n_lines: int) -> None:
    if not page_map:
        return
    if page_map[0] != 0:
        raise ValueError(f"page_mapping[0] debe ser 0, es {page_map[0]}")
    if max(page_map) > n_lines:
        raise ValueError(
            f"page_mapping max={max(page_map)} > n_lineas={n_lines}; "
            "la limpieza rompio el conteo de documentos"
        )
    for i in range(1, len(page_map)):
        if page_map[i] <= page_map[i - 1]:
            raise ValueError(
                f"page_mapping no monotono en indice {i}: "
                f"{page_map[i-1]} -> {page_map[i]}"
            )


def write_docboundaries(path: Path, offsets: list[int]) -> None:
    with path.open("wb") as f:
        for off in offsets:
            f.write(struct.pack("<Q", off))


def process_file_streaming(
    input_path: Path,
    output_path: Path,
    nivel: str,
    backend: Backend,
) -> dict:
    """Procesamiento secuencial linea a linea (memoria O(1) por linea)."""
    t0 = time.time()
    input_path = input_path.resolve()
    output_path = output_path.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    n_lines = 0
    empty_lines = 0
    offsets: list[int] = [0]
    out_bytes = 0

    with input_path.open("r", encoding="utf-8", errors="replace") as fin, output_path.open(
        "w", encoding="utf-8", newline="\n"
    ) as fout:
        for raw in fin:
            cl = clean_line(raw, nivel, backend)
            n_lines += 1
            if not cl:
                empty_lines += 1
            fout.write(cl)
            fout.write("\n")
            out_bytes += len(cl.encode("utf-8")) + 1
            offsets.append(out_bytes)

    page_map_path = input_path.parent / f"page_mapping_{input_path.stem}.bin"
    if page_map_path.exists():
        page_map = load_page_mapping(page_map_path)
        verify_page_mapping(page_map, n_lines)
        print(f"[INFO] page_mapping OK: {len(page_map)} entradas, max={max(page_map)}")

    db_path = Path(str(output_path) + ".DOCBOUNDARIES.ul")
    write_docboundaries(db_path, offsets)

    elapsed = time.time() - t0
    return {
        "input": str(input_path),
        "output": str(output_path),
        "docboundaries": str(db_path),
        "backend": backend,
        "nivel": nivel,
        "n_lines": n_lines,
        "empty_lines": empty_lines,
        "input_bytes": input_path.stat().st_size,
        "output_bytes": output_path.stat().st_size,
        "elapsed_s": elapsed,
    }


def process_file_parallel(
    input_path: Path,
    output_path: Path,
    nivel: str,
    backend: Backend,
    workers: int,
    chunk_lines: int,
) -> dict:
    """Multiprocessing para corpus pequenos (<500 MB)."""
    t0 = time.time()
    input_path = input_path.resolve()
    output_path = output_path.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    n_lines = 0
    with input_path.open("rb") as fin:
        for _ in fin:
            n_lines += 1

    page_map_path = input_path.parent / f"page_mapping_{input_path.stem}.bin"
    if page_map_path.exists():
        page_map = load_page_mapping(page_map_path)
        verify_page_mapping(page_map, n_lines)
        print(f"[INFO] page_mapping OK: {len(page_map)} entradas, max={max(page_map)}")

    cleaned_lines: list[str | None] = [None] * n_lines
    chunk: list[str] = []
    chunk_idx = 0
    tasks: list[tuple[int, list[str], str]] = []
    line_no = 0

    with input_path.open("r", encoding="utf-8", errors="replace") as fin:
        for raw in fin:
            chunk.append(raw)
            line_no += 1
            if len(chunk) >= chunk_lines:
                tasks.append((chunk_idx, chunk, nivel))
                chunk_idx += 1
                chunk = []
        if chunk:
            tasks.append((chunk_idx, chunk, nivel))

    if workers <= 1 or len(tasks) <= 1:
        pos = 0
        for _, lines, nv in tasks:
            for cl in lines:
                cleaned_lines[pos] = clean_line(cl, nv, backend)
                pos += 1
    else:
        with mp.Pool(processes=workers, initializer=_worker_init, initargs=(backend,)) as pool:
            results = pool.map(_worker_chunk, tasks, chunksize=1)
        pos = 0
        for _, lines in sorted(results, key=lambda x: x[0]):
            for cl in lines:
                cleaned_lines[pos] = cl
                pos += 1

    assert all(l is not None for l in cleaned_lines)

    offsets: list[int] = [0]
    out_bytes = 0
    empty_lines = 0
    with output_path.open("w", encoding="utf-8", newline="\n") as fout:
        for cl in cleaned_lines:
            assert cl is not None
            if not cl:
                empty_lines += 1
            fout.write(cl)
            fout.write("\n")
            out_bytes += len(cl.encode("utf-8")) + 1
            offsets.append(out_bytes)

    db_path = Path(str(output_path) + ".DOCBOUNDARIES.ul")
    write_docboundaries(db_path, offsets)

    elapsed = time.time() - t0
    return {
        "input": str(input_path),
        "output": str(output_path),
        "docboundaries": str(db_path),
        "backend": backend,
        "nivel": nivel,
        "n_lines": n_lines,
        "empty_lines": empty_lines,
        "input_bytes": input_path.stat().st_size,
        "output_bytes": output_path.stat().st_size,
        "elapsed_s": elapsed,
    }


def process_file(
    input_path: Path,
    output_path: Path,
    nivel: str,
    backend: Backend,
    workers: int,
    chunk_lines: int,
    stream_threshold_mb: int = 500,
) -> dict:
    size_mb = input_path.stat().st_size / (1024 * 1024)
    if size_mb >= stream_threshold_mb or workers <= 1:
        print(f"[INFO] modo streaming ({size_mb:.0f} MB >= {stream_threshold_mb} MB)")
        return process_file_streaming(input_path, output_path, nivel, backend)
    return process_file_parallel(
        input_path, output_path, nivel, backend, workers, chunk_lines
    )


def check_backend_deps(backend: Backend) -> None:
    if backend == "pandoc" and shutil.which("pandoc") is None:
        raise SystemExit("ERROR: backend 'pandoc' requiere el binario pandoc")
    try:
        resolve_stripper(backend)("test")
    except (ImportError, ModuleNotFoundError) as exc:
        raise SystemExit(
            f"ERROR: faltan dependencias para backend '{backend}'.\n"
            "  .venv/bin/pip install mwparserfromhell wikitextparser "
            "beautifulsoup4 lxml html5lib selectolax inscriptis wikiextractor pywikibot\n"
            f"  ({exc})"
        ) from exc


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Limpia wikitext wiki preservando lineas 1:1"
    )
    parser.add_argument("--input", required=True, help="Corpus .txt sucio")
    parser.add_argument("--output", required=True, help="Corpus .txt limpio")
    parser.add_argument(
        "--nivel",
        default="marcado",
        choices=["marcado", "torsen"],
        help="marcado=quitar wikitext; torsen=formato torsen",
    )
    parser.add_argument(
        "--backend",
        default=os.environ.get("WIKI_CLEAN_BACKEND", "regex"),
        choices=list(BACKENDS),
        help="Backend/cascada de limpieza; regex es el default de produccion",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=max(1, (os.cpu_count() or 4) - 1),
        help="Procesos paralelos (default: ncpu-1)",
    )
    parser.add_argument(
        "--chunk-lines",
        type=int,
        default=500,
        help="Lineas por bloque de trabajo",
    )
    args = parser.parse_args()
    backend: Backend = args.backend

    check_backend_deps(backend)
    print(f"[INFO] backend={backend}  nivel={args.nivel}")

    stats = process_file(
        Path(args.input),
        Path(args.output),
        args.nivel,
        backend,
        args.workers,
        args.chunk_lines,
    )

    pct = 100.0 * (1.0 - stats["output_bytes"] / stats["input_bytes"])
    print("[OK] Limpieza finalizada")
    print(f"  backend={stats['backend']}")
    print(f"  nivel={stats['nivel']}")
    print(f"  lineas={stats['n_lines']}  vacias={stats['empty_lines']}")
    print(f"  bytes: {stats['input_bytes']:,} -> {stats['output_bytes']:,}  ({pct:.1f}% reduccion)")
    print(f"  output={stats['output']}")
    print(f"  docboundaries={stats['docboundaries']}")
    print(f"  elapsed={stats['elapsed_s']:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
