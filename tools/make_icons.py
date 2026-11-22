#!/usr/bin/env python3
"""Prepares the KickServ release icons for icon.library (from arcedit, public domain).

    python3 -I tools/make_icons.py OUTDIR [PICTURES]

The pictures are PICTURES/<icon>.png + <icon>.sel.png, 64x64 RGBA
(default amiga/package/icons/pictures, the OS 4.1 style set). The selected
pictures are used as they are.
Writes OUTDIR/<icon>.argb and .sel.argb (raw ARGB) and OUTDIR/mkicons.script,
an AmigaDOS script that runs tools/os4/mkicon in the OS4 guest;
icon.library then saves the icons in its own format. The results are kept
in amiga/package/icons.
"""
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WBDRAWER, WBTOOL, WBPROJECT = 2, 3, 4
SIZE = 64

ICONS = {
    # file name (without .info): (type, default tool, tool types, stack, drawer window)
    "KickServ": (WBDRAWER, None, (), None, (80, 60, 400, 160)),
    "Install": (WBPROJECT, "SYS:Utilities/Installer",
                ("APPNAME=KickServ", "MINUSER=NOVICE", "DEFUSER=NOVICE",
                 "LOGFILE=T:KickServ_install_log"), 32768, None),
    "KickServ.doc": (WBPROJECT, "SYS:Utilities/MultiView", (), 32768, None),
    # the commodity: KickServ/KickServ.info in the archive
    "KickServ_KickServ": (WBTOOL, None,
                          ("DONOTWAIT", "CX_PRIORITY=0", "(PORT=6800)", "(DIR=SYS:Kickstart)",
                           "(ALLOW=ANY)"), 65536, None),
}


def argb(img):
    return b"".join(bytes((a, r, g, b)) for (r, g, b, a) in img.get_flattened_data())


def picture(pictures, name):
    img = Image.open(os.path.join(pictures, name)).convert("RGBA")
    if img.size != (SIZE, SIZE):
        sys.exit("%s is not %dx%d" % (name, SIZE, SIZE))
    return img


def main():
    out = sys.argv[1]
    pictures = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "amiga", "package", "icons", "pictures")
    os.makedirs(out, exist_ok=True)
    lines = ["; created by tools/make_icons.py; run in the drawer with the .argb files", "FailAt 21",
             "MakeDir out QUIET"]
    for name, (typ, tool, tts, stack, drawer) in ICONS.items():
        for suffix in ("", ".sel"):
            with open(os.path.join(out, name + suffix + ".argb"), "wb") as f:
                f.write(argb(picture(pictures, name + suffix + ".png")))
        t = {WBDRAWER: "DRAWER", WBTOOL: "TOOL", WBPROJECT: "PROJECT"}[typ]
        args = ['"out/%s"' % name, "ARGB=%s.argb" % name, "SELECTED=%s.sel.argb" % name, "TYPE=" + t]
        if tool:
            args.append('"TOOL=%s"' % tool)
        if stack:
            args.append("STACK=%d" % stack)
        if drawer:
            args.append("WINDOW=%d,%d,%d,%d" % drawer)
        for tt in tts:
            args.append('"TT=%s"' % tt)
        lines.append("mkicon " + " ".join(args))
    with open(os.path.join(out, "mkicons.script"), "w") as f:
        f.write("\n".join(lines) + "\n")
    sheet = Image.new("RGBA", (len(ICONS) * (SIZE + 12) + 8, 2 * SIZE + 30), (170, 170, 170, 255))
    for i, name in enumerate(ICONS):
        sheet.alpha_composite(picture(pictures, name + ".png"), (10 + i * (SIZE + 12), 10))
        sheet.alpha_composite(picture(pictures, name + ".sel.png"), (10 + i * (SIZE + 12), SIZE + 20))
    sheet.save(os.path.join(out, "icons-preview.png"))


if __name__ == "__main__":
    main()
