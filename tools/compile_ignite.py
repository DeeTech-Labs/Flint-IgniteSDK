#!/usr/bin/env python3
"""Compile an Ignite JSON project, UI and optional font/image assets into v1 binary."""

import argparse
import json
import re
import struct
from pathlib import Path
from build_package import build

COMPONENTS = {
    "app": 1,
    "watchface": 2,
    "theme": 3,
    "locale": 4,
    "resource": 5,
    "service": 6,
    "widget": 7,
    "complication": 8,
}
ROLES = {"none": 0, "clock": 1, "launcher": 2, "settings": 3, "theme": 4, "locale": 5}


def text(value):
    value = value.encode("utf-8")
    if len(value) > 65535 or b"\0" in value:
        raise ValueError("invalid string")
    return struct.pack("<H", len(value)) + value


def identifier(value):
    if (
        not re.fullmatch(r"[a-z0-9_-][a-z0-9._-]{0,63}", value)
        or ".." in value
        or value.endswith(".")
    ):
        raise ValueError("invalid identifier")
    return value


def color(value):
    if value in ("theme.background", "theme.text", "theme.accent", "theme.surface", "theme.pressed", "theme.muted"):
        return {
            "theme.background": 0xFFFD,
            "theme.text": 0xFFFC,
            "theme.accent": 0xFFFE,
            "theme.surface": 0xFFFB,
            "theme.pressed": 0xFFFA,
            "theme.muted": 0xFFF9,
        }[value]
    n = int(value.lstrip("#"), 16)
    return ((n >> 19) << 11) | (((n >> 10) & 63) << 5) | ((n >> 3) & 31)


def scene(nodes):
    ops, strings = [], bytearray()
    for node in nodes:
        kind = {"rect": 1, "rounded": 2, "line": 3, "text": 4, "bitmap": 5, "image": 6, "catalog_icon": 7}[
            node["kind"]
        ]
        data = (
            node.get("text", "").encode()
            if kind == 4
            else bytes.fromhex(node.get("bits", ""))
        )
        x, y = node.get("x", 0), node.get("y", 0)
        w, h = node.get("w", 0), node.get("h", 0)
        extra = node.get("resource", 0) if kind == 6 else node.get("extra", 0)
        if kind == 4:
            extra = int(node.get("center", False))
            if "overflow" in node:
                modes = {"clip": 8, "wrap": 10, "ellipsis": 12}
                if (not isinstance(node["overflow"], str) or node["overflow"] not in modes
                        or type(w) is not int or type(h) is not int
                        or not 1 <= w <= 640 or not 1 <= h <= 640):
                    raise ValueError("text overflow requires clip/wrap/ellipsis and positive w/h <= 640")
                extra |= modes[node["overflow"]]
        elif "overflow" in node:
            raise ValueError("overflow is only supported for text")
        if kind == 5:
            extra = node.get("scale", 1)
            w = h = 16 * extra
        if kind == 4 and len(data) > 192:
            raise ValueError("text too long")
        ops.append(
            struct.pack(
                ">BBhhhhHHHH",
                kind,
                node.get("font", 0),
                x,
                y,
                w,
                h,
                color(node.get("color", "#ffffff")),
                len(strings),
                len(data),
                extra,
            )
        )
        strings.extend(data)
    if not 1 <= len(ops) <= 32 or len(strings) > 512:
        raise ValueError("UI budget exceeded")
    if (
        nodes[0]["kind"] != "rect"
        or nodes[0].get("x", 0)
        or nodes[0].get("y", 0)
        or nodes[0].get("w") != 280
        or nodes[0].get("h") != 240
    ):
        raise ValueError("first node must cover the screen")
    return (
        struct.pack(">2sBBHHH", b"FS", 1, len(ops), 280, 240, len(strings))
        + b"".join(ops)
        + strings
    )


def atlas(path, size, chars):
    from PIL import Image, ImageDraw, ImageFont

    font = ImageFont.truetype(str(path), size)
    records = []
    payload = bytearray()
    chars = sorted(set(chars + "?"))
    offset = 12 + 24 * len(chars)
    for char in chars:
        left, top, right, bottom = font.getbbox(char, anchor="ls")
        w, h = right - left, bottom - top
        if w > 64 or h > 64:
            raise ValueError("glyph exceeds scratch budget")
        image = Image.new("L", (max(1, w), max(1, h)))
        ImageDraw.Draw(image).text(
            (-left, -top), char, font=font, fill=255, anchor="ls"
        )
        pixels = image.tobytes()[: w * h]
        records.append(
            struct.pack(
                "<IHHhhHHII",
                ord(char),
                w,
                h,
                left,
                -top,
                round(font.getlength(char)),
                0,
                offset,
                len(pixels),
            )
        )
        payload.extend(pixels)
        offset += len(pixels)
    ascent, descent = font.getmetrics()
    return (
        struct.pack("<4sHHHH", b"FLGF", len(chars), ascent, descent, 0)
        + b"".join(records)
        + payload
    )


def compile_project(project, root):
    allowed = {"id", "name", "publisher", "version", "components", "resources", "icons"}
    if not isinstance(project, dict) or set(project) - allowed:
        raise ValueError("unknown project fields: " + ", ".join(sorted(set(project) - allowed)))
    identifier(project["id"])
    identifier(project["publisher"])
    components = project["components"]
    if not isinstance(components, list) or not 1 <= len(components) <= 32:
        raise ValueError("manifest component budget exceeded")
    name = project["name"]
    if not isinstance(name, str) or not name or len(name.encode()) > 96 or any(ord(c) < 32 or ord(c) == 127 for c in name):
        raise ValueError("invalid name")
    def number(value, maximum=0xffffffff):
        if type(value) is not int or not 0 <= value <= maximum:
            raise ValueError("invalid unsigned integer")
        return value
    version = number(project.get("version", 1))
    if not version:
        raise ValueError("version must be positive")
    manifest = bytearray(b"FLM3" + text(project["id"]) + text(name) + text(project["publisher"]))
    manifest.extend(struct.pack("<IH", version, len(components)))
    used, ids, prepared = {1}, set(), []
    for resource in project.get("resources", []):
        section = number(resource["id"])
        if not section or section in used:
            raise ValueError("duplicate resource section")
        used.add(section)
    services = 0
    union = set()
    for index, component in enumerate(components):
        allowed_component = {"id", "type", "role", "section", "ui", "values", "logic", "budget", "capabilities", "permissions", "app_api", "sensor_api", "ipc_api"}
        if not isinstance(component, dict) or set(component) - allowed_component:
            raise ValueError("unknown component fields")
        if not {"id", "type", "budget", "capabilities", "permissions"} <= set(component):
            raise ValueError("component requires explicit budget, capabilities and permissions")
        ident = identifier(component["id"])
        if ident in ids:
            raise ValueError("duplicate component ID")
        ids.add(ident)
        kind = COMPONENTS[component["type"]]
        service = component["type"] == "service"
        services += service
        if services > 1:
            raise ValueError("at most one service per package")
        section = number(component.get("section", 0 if service else index + 2))
        if service:
            if section or "ui" in component or "values" in component or component.get("role", "none") != "none":
                raise ValueError("service cannot contain UI or declarative resources")
        else:
            if not section or section in used:
                raise ValueError("duplicate component section")
            used.add(section)
        budget = component["budget"]
        if not isinstance(budget, dict) or set(budget) != {"internal", "external", "instructions"}:
            raise ValueError("component budget requires internal, external and instructions")
        budgets = tuple(number(budget[key]) for key in ("internal", "external", "instructions"))
        caps = number(component["capabilities"], 0xffffffffffffffff)
        flags = {}
        for flag in ("app_api", "sensor_api", "ipc_api"):
            value = component.get(flag, 0)
            if type(value) is not int or value not in (0, 1):
                raise ValueError(flag + " must be 0 or 1")
            flags[flag] = value
        if service:
            flags["app_api"] = 1
            caps |= 16384 | 64 | 128
        if (flags["sensor_api"] or flags["ipc_api"]) and not flags["app_api"]:
            raise ValueError("sensor_api and ipc_api require app_api")
        caps |= (128 if flags["app_api"] else 0) | (8192 if flags["sensor_api"] else 0) | (32768 if flags["ipc_api"] else 0)
        if not isinstance(component["permissions"], list):
            raise ValueError("permissions must be a list")
        if "ui" in component and "values" in component:
            raise ValueError("component cannot contain both UI and values")
        declarations = []
        for permission in component["permissions"]:
            if isinstance(permission, str): permission = {"id": permission, "scope": ""}
            if not isinstance(permission, dict) or set(permission) - {"id", "scope"} or "id" not in permission:
                raise ValueError("invalid permission declaration")
            permission_id, scope = identifier(permission["id"]), permission.get("scope", "")
            if not isinstance(scope, str) or len(scope.encode()) > 192 or "\0" in scope:
                raise ValueError("invalid permission scope")
            if service and (permission_id.startswith("ui.") or permission_id == "input.focused"):
                raise ValueError("service cannot declare UI permissions")
            declaration = (permission_id, scope)
            if declaration in declarations:
                raise ValueError("duplicate permission declaration")
            declarations.append(declaration)
        if len(declarations) > 64:
            raise ValueError("permission budget exceeded")
        union.update(declarations)
        if len(union) > 64:
            raise ValueError("package permission budget exceeded")
        if service and ("app.background.execute", "") not in declarations:
            raise ValueError("service requires app.background.execute")
        if ("app.background.autostart", "") in declarations and (not service or ("app.background.execute", "") not in declarations):
            raise ValueError("autostart requires service execution permission")
        if service and "logic" not in component:
            raise ValueError("service requires Lua logic")
        if flags["app_api"] and "logic" not in component:
            raise ValueError("app_api requires Lua logic")
        prepared.append((component, section, budgets, caps, declarations, flags))
    for component, *_ in prepared:
        logic = component.get("logic")
        if isinstance(logic, dict) and "section" in logic:
            logic_id = number(logic["section"])
            if not logic_id or logic_id in used:
                raise ValueError("duplicate logic section")
            used.add(logic_id)
    sections = []
    for component, section, budgets, caps, declarations, flags in prepared:
        logic_section = 0
        if "logic" in component:
            logic = component["logic"]
            if (not isinstance(logic, dict) or not {"language", "path"} <= set(logic)
                    or set(logic) - {"language", "path", "includes", "section"} or logic["language"] != "lua"):
                raise ValueError("logic requires language=lua and path")
            if component["type"] not in ("app", "watchface", "service"):
                raise ValueError("Lua requires app, watchface or service component")
            if not caps & 64:
                raise ValueError("Lua requires capability 64")
            if not 48*1024 <= budgets[0] <= 144*1024 or not 1 <= budgets[2] <= 100000:
                raise ValueError("quota_exceeded: invalid Lua budget")
            logic_section = number(logic.get("section", max(used) + 1))
            if "section" not in logic:
                if not logic_section or logic_section in used:
                    raise ValueError("duplicate logic section")
                used.add(logic_section)
            includes = logic.get("includes", [])
            if not isinstance(includes, list) or len(includes) > 8 or any(not isinstance(path, str) or not path for path in includes):
                raise ValueError("logic includes must be up to eight source paths")
            parts = []
            for source in [*includes, logic["path"]]:
                if not isinstance(source, str) or not source:
                    raise ValueError("invalid logic path")
                path = (root / source).resolve()
                if not path.is_relative_to(root.resolve()):
                    raise ValueError("logic escapes project")
                if path.stat().st_size > 16384:
                    raise ValueError("Lua source exceeds 16384 bytes")
                parts.append(path.read_text(encoding="utf-8"))
            data = "\n".join(parts).encode("utf-8")
            if not data or len(data) > 16384 or data.startswith(b"\x1b") or b"\0" in data:
                raise ValueError("Lua requires text source of 1..16384 bytes")
            if flags["app_api"] and component["type"] != "service":
                names = {}
                for index, node in enumerate(component.get("ui", []), 1):
                    if "id" in node:
                        name = node["id"]
                        if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,31}", name) or name in names:
                            raise ValueError("UI IDs must be unique names of 1..32 lowercase ASCII characters")
                        names[name] = index
                data = ("local ui={" + ",".join('["' + k + '"]=' + str(v) for k, v in sorted(names.items())) + "}; ").encode("ascii") + data
                if len(data) > 16384:
                    raise ValueError("Lua source including UI names exceeds 16384 bytes")
            sections.append((logic_section, "lua", data))
        manifest.extend(text(component["id"]) + struct.pack("<HBIIIIIQH", COMPONENTS[component["type"]], ROLES[component.get("role", "none")], section, logic_section, *budgets, caps, len(declarations)))
        for permission_id, scope in sorted(declarations): manifest.extend(text(permission_id) + text(scope))
        if component["type"] == "service":
            continue
        if "ui" in component:
            if not flags["app_api"] and any("id" in node for node in component["ui"]):
                raise ValueError("UI node IDs require app_api=1")
            sections.append((section, "ui", scene(component["ui"])))
        elif "values" in component:
            values = component["values"]
            sections.append((section, "locale" if component["type"] == "locale" else "component", struct.pack("<H", len(values)) + b"".join(text(k) + text(v) for k, v in sorted(values.items()))))
        else: raise ValueError("component requires declarative UI or values")
    for resource in project.get("resources", []):
        section = resource["id"]
        path = (root / resource["path"]).resolve()
        if not path.is_relative_to(root.resolve()):
            raise ValueError("resource escapes project")
        if resource.get("format") == "font":
            data = atlas(path, resource.get("size", 20), resource["characters"])
        elif resource.get("format") == "image":
            from flim import convert
            data = convert(path, resource, root)
        else:
            data = path.read_bytes()
        sections.append((section, "resource", data))
    icons = project.get("icons", {})
    if not isinstance(icons, dict) or set(icons) - {"main", "mask", "light", "dark"}:
        raise ValueError("icons accepts main, mask, light, dark resource IDs")
    if icons and not icons.get("main"):
        raise ValueError("icons requires a main color resource")
    for key in ("main", "mask", "light", "dark"):
        ident = icons.get(key, 0)
        if type(ident) is not int or not 0 <= ident <= 0xffffffff:
            raise ValueError("invalid icon resource ID")
        if ident:
            resource = next((data for section, kind, data in sections
                             if section == ident and kind == "resource"), None)
            if resource is None or len(resource) < 16:
                raise ValueError("icon must reference a FLIM resource")
            magic, version, fmt, w, h, count, reserved = struct.unpack("<4sBBHHHI", resource[:16])
            if (magic != b"FLIM" or version != 2 or reserved or (w, h) != (48, 48)
                    or (fmt != 1 if key == "mask" else fmt not in (2, 3))):
                raise ValueError("icons require 48x48 FLIM: A4 for mask, color for other variants")
            expected = 16 + count*3 + h * ((w+1)//2 if fmt == 1 else w if fmt == 2 else w*3)
            if (len(resource) != expected or (not 1 <= count <= 256 if fmt == 2 else count != 0)
                    or (fmt == 2 and any(i >= count for i in resource[16+count*3:]))):
                raise ValueError("invalid FLIM icon data")
        manifest.extend(struct.pack("<I", ident))
    return build([(1, "manifest", bytes(manifest)), *sections])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(
        compile_project(
            json.loads(args.project.read_text(encoding="utf-8")),
            args.project.resolve().parent,
        )
    )


if __name__ == "__main__":
    main()
