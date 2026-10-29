"""Independent read-back acceptance checks for the M1 export formats.

This script does not reuse any project code: it parses .slmslice, CLI, SVG, PNG
and CSV with the Python standard library (plus Pillow when available for a
second PNG decode), mirroring what a third party would do when importing the
files.

Usage:
    set SLMSLICER_EXPORT_TEST_ARTIFACTS=D:\\temp\\slice-artifacts
    slice_export_tests.exe
    python tests/verify_exports.py D:\\temp\\slice-artifacts

The artifact hook is implemented in slice_export_tests.cpp; setting
SLMSLICER_EXPORT_TEST_ARTIFACTS makes the test write one sample per format.
"""

import csv
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path
from xml.etree import ElementTree

try:
    from PIL import Image
except ImportError:  # Pillow is optional; the PNG is checked manually below.
    Image = None

ART = Path(sys.argv[1] if len(sys.argv) > 1 else "slice-artifacts")
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def read(path):
    return (ART / path).read_bytes()


def parse_slmslice():
    data = read("sample.slmslice")
    check(data[:8] == b"SLMSLICE", "slmslice magic")
    version, header_bytes = struct.unpack_from("<II", data, 8)
    check(version == 1, "slmslice version")
    header = json.loads(data[16 : 16 + header_bytes].decode("utf-8"))
    check(header["format"] == "slmslice", "slmslice header format")
    check(header["coordinate_system"]["z_reference"] == "layer_midplane",
          "slmslice z reference")
    payload = data[16 + header_bytes :]
    checks = header["layers"]["checksums"]
    check(sum(c["bytes"] for c in checks) == len(payload),
          "slmslice payload length")
    check((16 + header_bytes + len(payload)) == len(data),
          "slmslice file size equation")
    offset = 0
    layers = []
    for index, entry in enumerate(checks):
        blob = payload[offset : offset + entry["bytes"]]
        offset += entry["bytes"]
        digest = hashlib.sha256(blob).hexdigest()
        check(digest == entry["sha256"].lower(),
              f"slmslice layer {index} sha256")
        grid, z, flags, rings, hatches = struct.unpack_from("<qdIII", blob, 0)
        check(flags & ~1 == 0, "slmslice flags reserved bits")
        cursor = 28
        contours = []
        for _ in range(rings):
            model_id, role, closed, _res, points = struct.unpack_from(
                "<QBBHI", blob, cursor)
            cursor += 16
            xy = struct.unpack_from(f"<{points * 2}d", blob, cursor)
            cursor += 16 * points
            contours.append((model_id, role, closed,
                             list(zip(xy[0::2], xy[1::2]))))
        hatch_lines = []
        for _ in range(hatches):
            model_id, x1, y1, x2, y2 = struct.unpack_from("<Qdddd", blob, cursor)
            cursor += 40
            hatch_lines.append((model_id, (x1, y1), (x2, y2)))
        check(cursor == len(blob), f"slmslice layer {index} exact consumption")
        layers.append((grid, z, flags, contours, hatch_lines))
    check(offset == len(payload), "slmslice payload fully consumed")
    check(len(layers) == 3, "slmslice should keep the empty grid layer")
    check(layers[0][0] == 0 and layers[1][0] == 1 and layers[2][0] == 2,
          "slmslice grid indices")
    check(layers[1][2] & 1 == 1, "slmslice empty layer flag")
    check(abs(layers[0][1] - 0.015) < 1e-9, "slmslice mid-plane z")
    check(len(layers[0][3]) == 2 and len(layers[0][4]) == 2,
          "slmslice first layer counts")
    check(layers[0][3][0][0] == 1 and layers[0][3][1][0] == 1,
          "slmslice model attribution")
    check(layers[0][3][0][1] == 0 and layers[0][3][1][1] == 1,
          "slmslice ring roles")
    check(len(header["parts"]) == 2, "slmslice parts")
    return layers


def contour_length(points):
    total = sum(
        ((points[i][0] - points[i - 1][0]) ** 2 +
         (points[i][1] - points[i - 1][1]) ** 2) ** 0.5
        for i in range(1, len(points))
    )
    return total


def parse_cli():
    text = read("sample.cli").decode("ascii")
    check("$$HEADERSTART" in text and "$$GEOMETRYEND" in text,
          "CLI header/geometry markers")
    check("$$UNITS/1.0" in text, "CLI units")
    layers = []
    current = None
    contours = 0
    hatches = 0
    contour_len = 0.0
    hatch_len = 0.0
    for line in text.splitlines():
        if line.startswith("$$LAYER/"):
            current = {"z": float(line.split("/")[1]), "polylines": 0,
                       "hatches": 0}
            layers.append(current)
        elif line.startswith("$$POLYLINE/"):
            fields = line.split("/")[1].split(",")
            _id, direction, count = int(fields[0]), int(fields[1]), int(fields[2])
            coords = [float(v) for v in fields[3:]]
            check(len(coords) == count * 2, "CLI polyline point count")
            points = list(zip(coords[0::2], coords[1::2]))
            contour_len += contour_length(points)
            contours += 1
            current["polylines"] += 1
            check(direction in (0, 1), "CLI polyline direction")
        elif line.startswith("$$HATCHES/"):
            fields = line.split("/")[1].split(",")
            count = int(fields[1])
            coords = [float(v) for v in fields[2:]]
            check(len(coords) == count * 4, "CLI hatch coordinate count")
            for i in range(count):
                x1, y1, x2, y2 = coords[i * 4 : i * 4 + 4]
                hatch_len += ((x2 - x1) ** 2 + (y2 - y1) ** 2) ** 0.5
            hatches += count
            current["hatches"] += count
    check(len(layers) == 2, "CLI should skip the empty layer")
    check(abs(layers[0]["z"] - 0.03) < 1e-4 and abs(layers[1]["z"] - 0.09) < 1e-4,
          "CLI layer top-surface z")
    check(contours == 3 and hatches == 3, "CLI contour/hatch totals")
    check(abs(contour_len - 120.0) < 0.01, "CLI contour length")
    check(abs(hatch_len - 30.0) < 0.01, "CLI hatch length")


def parse_metadata(text):
    values = {}
    for field in text.split(";"):
        if "=" in field:
            key, value = field.split("=", 1)
            values[key] = value
    return values


def parse_svg():
    root = ElementTree.parse(ART / "sample.svg").getroot()
    namespace = "{http://www.w3.org/2000/svg}"
    desc = root.find(f"{namespace}desc")
    check(desc is not None, "SVG desc element")
    meta = parse_metadata(desc.text or "")
    check(meta.get("format") == "svg", "SVG metadata format")
    check(meta.get("layers") == "2", "SVG metadata layers")
    check(meta.get("contours") == "3", "SVG metadata contours")
    check(meta.get("hatches") == "3", "SVG metadata hatches")
    groups = root.findall(f".//{namespace}g[@class='layer']")
    check(len(groups) == 2, "SVG layer groups")
    outer = root.findall(f".//{namespace}polyline[@class='contour outer']")
    inner = root.findall(f".//{namespace}polyline[@class='contour inner']")
    hatch = root.findall(f".//{namespace}line[@class='hatch']")
    check(len(outer) == 2, "SVG outer contours")
    check(len(inner) == 1, "SVG inner contours")
    check(len(hatch) == 3, "SVG hatch lines")
    labels = [t.text for t in root.findall(f".//{namespace}text")]
    check(any(label and label.startswith("L0 ") for label in labels),
          "SVG layer label")
    check(root.get("viewBox") is not None, "SVG viewBox")


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def parse_png():
    data = read("sample.png")
    check(data[:8] == PNG_SIGNATURE, "PNG signature")
    offset = 8
    idat = b""
    metadata = None
    width = height = None
    colour_type = None
    palette = b""
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4 : offset + 8]
        chunk = data[offset + 8 : offset + 8 + length]
        crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        check(crc == zlib.crc32(kind + chunk) & 0xFFFFFFFF,
              f"PNG chunk {kind!r} crc")
        if kind == b"IHDR":
            width, height, depth, colour_type = struct.unpack_from(">IIBB", chunk, 0)
            check(depth == 8 and colour_type == 3, "PNG IHDR format")
        elif kind == b"PLTE":
            palette = chunk
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"tEXt":
            key, _, value = chunk.partition(b"\0")
            if key == b"slmslicer":
                metadata = value.decode("utf-8")
        elif kind == b"IEND":
            break
        offset += 12 + length
    check(metadata is not None, "PNG slmslicer metadata")
    meta = parse_metadata(metadata or "")
    check(meta.get("format") == "png", "PNG metadata format")
    check(meta.get("layers") == "2", "PNG metadata layers")
    check(meta.get("contours") == "3", "PNG metadata contours")
    check(meta.get("hatches") == "3", "PNG metadata hatches")
    check(int(meta.get("width", "0")) == width and
          int(meta.get("height", "0")) == height, "PNG metadata size")
    raw = zlib.decompress(idat)
    check(len(raw) == height * (width + 1), "PNG raw scanline size")
    check(all(raw[row * (width + 1)] == 0 for row in range(height)),
          "PNG scanline filters")
    indices = {raw[row * (width + 1) + 1 + col]
               for row in range(height) for col in range(width)}
    check({1, 2, 3}.issubset(indices), "PNG report colours present")
    check(len(palette) >= 15, "PNG palette")
    # Pillow must be able to decode what we write (optional dependency).
    if Image is None:
        print("note: Pillow not installed, skipped the Pillow PNG decode")
    else:
        with Image.open(ART / "sample.png") as image:
            check(image.size == (width, height), "Pillow PNG size")
            check(image.mode == "P", "Pillow PNG palette mode")
            image.load()


def parse_csv():
    text = read("sample.csv").decode("utf-8")
    rows = list(csv.reader(text.splitlines()))
    check(rows[0] == ["grid_index", "z_mm", "empty", "contours", "hatches",
                      "contour_length_mm", "hatch_length_mm",
                      "total_length_mm"], "CSV header")
    check(len(rows) == 4, "CSV header + 3 rows")
    check(int(rows[2][0]) == 1 and int(rows[2][2]) == 1, "CSV empty layer row")
    contours = sum(int(row[3]) for row in rows[1:])
    hatches = sum(int(row[4]) for row in rows[1:])
    contour_len = sum(float(row[5]) for row in rows[1:])
    hatch_len = sum(float(row[6]) for row in rows[1:])
    check(contours == 3 and hatches == 3, "CSV totals")
    check(abs(contour_len - 120.0) < 1e-6, "CSV contour length")
    check(abs(hatch_len - 30.0) < 1e-6, "CSV hatch length")


parse_slmslice()
parse_cli()
parse_svg()
parse_png()
parse_csv()

if failures:
    print("FAILURES:")
    for failure in failures:
        print(" -", failure)
    sys.exit(1)
print("ALL INDEPENDENT READ-BACK CHECKS PASSED")
