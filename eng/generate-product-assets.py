#!/usr/bin/env python3
"""Deterministically derive Windows PNG and ICO assets from the product SVG."""

from __future__ import annotations

import argparse
import binascii
import json
import math
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence


SUPERSAMPLE = 4
SUPPORTED_GRAPHICS = {"rect", "circle", "polygon", "polyline"}
IGNORED_METADATA = {"title", "desc"}


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def repository_path(repository_root: Path, relative: str) -> Path:
    if not isinstance(relative, str) or not relative or "\\" in relative:
        raise ValueError(f"asset path must use a non-empty repository-relative POSIX path: {relative!r}")
    candidate = PurePosixPath(relative)
    if candidate.is_absolute() or candidate == PurePosixPath("."):
        raise ValueError(f"asset path must be repository-relative: {relative!r}")
    if ".." in candidate.parts or any(":" in part for part in candidate.parts):
        raise ValueError(f"asset path must not escape the repository: {relative!r}")

    root = repository_root.resolve()
    resolved = root.joinpath(*candidate.parts).resolve()
    if resolved != root and root not in resolved.parents:
        raise ValueError(f"asset path resolves outside the repository: {relative!r}")
    return resolved


def parse_color(value: str) -> tuple[int, int, int, int]:
    if value == "none":
        return (0, 0, 0, 0)
    if len(value) != 7 or not value.startswith("#"):
        raise ValueError(f"unsupported color: {value!r}")
    return (int(value[1:3], 16), int(value[3:5], 16), int(value[5:7], 16), 255)


def parse_points(value: str) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    for pair in value.split():
        coordinates = pair.split(",")
        if len(coordinates) != 2:
            raise ValueError(f"invalid point: {pair!r}")
        points.append((float(coordinates[0]), float(coordinates[1])))
    if len(points) < 2:
        raise ValueError("a point sequence needs at least two points")
    return points


def point_in_rounded_rect(
    px: float,
    py: float,
    x: float,
    y: float,
    width: float,
    height: float,
    radius: float,
) -> bool:
    if px < x or py < y or px > x + width or py > y + height:
        return False
    radius = min(radius, width / 2.0, height / 2.0)
    if radius <= 0 or x + radius <= px <= x + width - radius:
        return True
    if y + radius <= py <= y + height - radius:
        return True
    corner_x = x + radius if px < x + radius else x + width - radius
    corner_y = y + radius if py < y + radius else y + height - radius
    return (px - corner_x) ** 2 + (py - corner_y) ** 2 <= radius**2


def point_in_polygon(
    px: float, py: float, points: Sequence[tuple[float, float]]
) -> bool:
    inside = False
    previous = points[-1]
    for current in points:
        x1, y1 = previous
        x2, y2 = current
        crosses = (y1 > py) != (y2 > py)
        if crosses:
            crossing_x = (x2 - x1) * (py - y1) / (y2 - y1) + x1
            if px < crossing_x:
                inside = not inside
        previous = current
    return inside


def distance_to_segment_squared(
    px: float,
    py: float,
    x1: float,
    y1: float,
    x2: float,
    y2: float,
) -> float:
    dx = x2 - x1
    dy = y2 - y1
    length_squared = dx * dx + dy * dy
    if length_squared == 0:
        return (px - x1) ** 2 + (py - y1) ** 2
    projection = ((px - x1) * dx + (py - y1) * dy) / length_squared
    projection = max(0.0, min(1.0, projection))
    nearest_x = x1 + projection * dx
    nearest_y = y1 + projection * dy
    return (px - nearest_x) ** 2 + (py - nearest_y) ** 2


def shape_bounds(element: ET.Element) -> tuple[float, float, float, float]:
    kind = local_name(element.tag)
    if kind == "rect":
        x = float(element.attrib["x"])
        y = float(element.attrib["y"])
        return (x, y, x + float(element.attrib["width"]), y + float(element.attrib["height"]))
    if kind == "circle":
        cx = float(element.attrib["cx"])
        cy = float(element.attrib["cy"])
        radius = float(element.attrib["r"])
        return (cx - radius, cy - radius, cx + radius, cy + radius)
    points = parse_points(element.attrib["points"])
    padding = float(element.attrib.get("stroke-width", "0")) / 2.0
    xs = [point[0] for point in points]
    ys = [point[1] for point in points]
    return (
        min(xs) - padding,
        min(ys) - padding,
        max(xs) + padding,
        max(ys) + padding,
    )


def point_in_shape(element: ET.Element, px: float, py: float) -> bool:
    kind = local_name(element.tag)
    if kind == "rect":
        return point_in_rounded_rect(
            px,
            py,
            float(element.attrib["x"]),
            float(element.attrib["y"]),
            float(element.attrib["width"]),
            float(element.attrib["height"]),
            float(element.attrib.get("rx", "0")),
        )
    if kind == "circle":
        dx = px - float(element.attrib["cx"])
        dy = py - float(element.attrib["cy"])
        return dx * dx + dy * dy <= float(element.attrib["r"]) ** 2
    points = parse_points(element.attrib["points"])
    if kind == "polygon":
        return point_in_polygon(px, py, points)
    if kind == "polyline":
        radius_squared = (float(element.attrib["stroke-width"]) / 2.0) ** 2
        return any(
            distance_to_segment_squared(px, py, *start, *end) <= radius_squared
            for start, end in zip(points, points[1:])
        )
    raise ValueError(f"unsupported SVG element: {kind}")


def read_svg(svg_path: Path) -> tuple[tuple[float, float, float, float], list[ET.Element]]:
    root = ET.parse(svg_path).getroot()
    if local_name(root.tag) != "svg":
        raise ValueError("the authoritative source is not an SVG root")
    view_box_values = [float(value) for value in root.attrib["viewBox"].split()]
    if len(view_box_values) != 4 or view_box_values[2] <= 0 or view_box_values[3] <= 0:
        raise ValueError("the SVG needs a positive four-value viewBox")
    if view_box_values[2] != view_box_values[3]:
        raise ValueError("the Windows icon source must use a square viewBox")

    graphics: list[ET.Element] = []
    for element in root:
        kind = local_name(element.tag)
        if kind in IGNORED_METADATA:
            continue
        if kind not in SUPPORTED_GRAPHICS:
            raise ValueError(f"unsupported SVG element: {kind}")
        if kind == "polyline":
            if element.attrib.get("fill") != "none":
                raise ValueError("polylines must have fill=none")
            if element.attrib.get("stroke-linecap") != "round":
                raise ValueError("polylines must use round line caps")
            if element.attrib.get("stroke-linejoin") != "round":
                raise ValueError("polylines must use round line joins")
            parse_color(element.attrib["stroke"])
        else:
            parse_color(element.attrib["fill"])
        graphics.append(element)
    return tuple(view_box_values), graphics  # type: ignore[return-value]


def set_pixel(buffer: bytearray, width: int, x: int, y: int, color: tuple[int, int, int, int]) -> None:
    offset = (y * width + x) * 4
    buffer[offset : offset + 4] = bytes(color)


def render_svg(svg_path: Path, output_size: int) -> bytes:
    view_box, graphics = read_svg(svg_path)
    view_x, view_y, view_width, view_height = view_box
    high_size = output_size * SUPERSAMPLE
    high_pixels = bytearray(high_size * high_size * 4)

    scale_x = high_size / view_width
    scale_y = high_size / view_height
    for element in graphics:
        kind = local_name(element.tag)
        color = parse_color(
            element.attrib["stroke"] if kind == "polyline" else element.attrib["fill"]
        )
        min_x, min_y, max_x, max_y = shape_bounds(element)
        start_x = max(0, math.floor((min_x - view_x) * scale_x) - 1)
        end_x = min(high_size, math.ceil((max_x - view_x) * scale_x) + 1)
        start_y = max(0, math.floor((min_y - view_y) * scale_y) - 1)
        end_y = min(high_size, math.ceil((max_y - view_y) * scale_y) + 1)
        for pixel_y in range(start_y, end_y):
            sample_y = view_y + (pixel_y + 0.5) / scale_y
            for pixel_x in range(start_x, end_x):
                sample_x = view_x + (pixel_x + 0.5) / scale_x
                if point_in_shape(element, sample_x, sample_y):
                    set_pixel(high_pixels, high_size, pixel_x, pixel_y, color)

    pixels = bytearray(output_size * output_size * 4)
    samples_per_pixel = SUPERSAMPLE * SUPERSAMPLE
    for output_y in range(output_size):
        for output_x in range(output_size):
            alpha_sum = 0
            premultiplied = [0, 0, 0]
            for offset_y in range(SUPERSAMPLE):
                high_y = output_y * SUPERSAMPLE + offset_y
                for offset_x in range(SUPERSAMPLE):
                    high_x = output_x * SUPERSAMPLE + offset_x
                    source_offset = (high_y * high_size + high_x) * 4
                    alpha = high_pixels[source_offset + 3]
                    alpha_sum += alpha
                    for channel in range(3):
                        premultiplied[channel] += high_pixels[source_offset + channel] * alpha
            target_offset = (output_y * output_size + output_x) * 4
            if alpha_sum:
                for channel in range(3):
                    pixels[target_offset + channel] = round(premultiplied[channel] / alpha_sum)
                pixels[target_offset + 3] = round(alpha_sum / samples_per_pixel)
    return encode_png(output_size, output_size, bytes(pixels))


def adler32(payload: bytes) -> int:
    first = 1
    second = 0
    modulus = 65521
    for byte in payload:
        first = (first + byte) % modulus
        second = (second + first) % modulus
    return (second << 16) | first


def zlib_store(payload: bytes) -> bytes:
    stream = bytearray(b"\x78\x01")
    position = 0
    while position < len(payload):
        block = payload[position : position + 65535]
        position += len(block)
        stream.append(1 if position == len(payload) else 0)
        stream.extend(struct.pack("<HH", len(block), len(block) ^ 0xFFFF))
        stream.extend(block)
    stream.extend(struct.pack(">I", adler32(payload)))
    return bytes(stream)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def encode_png(width: int, height: int, rgba: bytes) -> bytes:
    stride = width * 4
    scanlines = b"".join(
        b"\x00" + rgba[row * stride : (row + 1) * stride] for row in range(height)
    )
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib_store(scanlines))
        + png_chunk(b"IEND", b"")
    )


def encode_ico(images: Sequence[tuple[int, bytes]]) -> bytes:
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = bytearray()
    payloads = bytearray()
    for size, payload in images:
        encoded_size = 0 if size == 256 else size
        entries.extend(
            struct.pack(
                "<BBBBHHII",
                encoded_size,
                encoded_size,
                0,
                0,
                1,
                32,
                len(payload),
                offset,
            )
        )
        payloads.extend(payload)
        offset += len(payload)
    return header + bytes(entries) + bytes(payloads)


def expected_assets(repository_root: Path) -> dict[Path, bytes]:
    identity_path = repository_root / "release/product-identity.json"
    identity = json.loads(identity_path.read_text(encoding="utf-8"))
    graphic = identity["product"]["graphic"]
    windows = graphic["windows"]
    source = repository_path(repository_root, graphic["authoritativeSource"])
    sizes = [int(value) for value in windows["pngSizes"]]

    assets: dict[Path, bytes] = {}
    images: list[tuple[int, bytes]] = []
    for size in sizes:
        payload = render_svg(source, size)
        output = repository_path(
            repository_root, windows["pngPattern"].format(size=size)
        )
        assets[output] = payload
        images.append((size, payload))
    assets[repository_path(repository_root, windows["icon"])] = encode_ico(images)
    return assets


def verify_assets(assets: Iterable[tuple[Path, bytes]]) -> list[str]:
    failures: list[str] = []
    for path, expected in assets:
        if not path.is_file():
            failures.append(f"missing generated asset: {path}")
            continue
        actual = path.read_bytes()
        if actual != expected:
            failures.append(f"generated asset is stale: {path}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify committed derivatives without changing files",
    )
    arguments = parser.parse_args()

    repository_root = Path(__file__).resolve().parents[1]
    assets = expected_assets(repository_root)
    if arguments.check:
        failures = verify_assets(assets.items())
        if failures:
            for failure in failures:
                print(failure, file=sys.stderr)
            return 1
        print(f"product assets: PASS ({len(assets)} files)")
        return 0

    for path, payload in assets.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        print(path.relative_to(repository_root).as_posix())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
