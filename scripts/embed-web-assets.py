"""CMake-owned source generator. No package manager or JS bundler is used."""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
assets = [
    ("/", "text/html; charset=utf-8", "web/index.html"),
    ("/softline.css", "text/css; charset=utf-8", "web/softline.css"),
    ("/softline.js", "text/javascript; charset=utf-8", "web/softline.js"),
    ("/completion.js", "text/javascript; charset=utf-8", "web/completion.js"),
    ("/prompt.js", "text/javascript; charset=utf-8", "web/prompt.js"),
    ("/transcripts.js", "text/javascript; charset=utf-8", "web/transcripts.js"),
    ("/images.js", "text/javascript; charset=utf-8", "web/images.js"),
    ("/image-viewer.js", "text/javascript; charset=utf-8", "web/image-viewer.js"),
    ("/markdown.js", "text/javascript; charset=utf-8", "web/markdown.js"),
    ("/terminal", "text/html; charset=utf-8", "web/terminal.html"),
    ("/terminal.js", "text/javascript; charset=utf-8", "web/terminal.js"),
    ("/terminal-panel.js", "text/javascript; charset=utf-8", "web/terminal-panel.js"),
    ("/pane-divider.js", "text/javascript; charset=utf-8", "web/pane-divider.js"),
    ("/terminal.css", "text/css; charset=utf-8", "web/terminal.css"),
    ("/xterm.js", "text/javascript; charset=utf-8", "vendor/xterm/xterm.mjs"),
    ("/xterm-addon-fit.js", "text/javascript; charset=utf-8", "vendor/xterm/addon-fit.mjs"),
    ("/xterm.css", "text/css; charset=utf-8", "vendor/xterm/xterm.css"),
    ("/licenses/xterm.txt", "text/plain; charset=utf-8", "vendor/xterm/LICENSE"),
    ("/licenses/xterm-addon-fit.txt", "text/plain; charset=utf-8", "vendor/xterm/LICENSE.addon-fit"),
    ("/smd.js", "text/javascript; charset=utf-8", "vendor/streaming-markdown/smd.js"),
    ("/JetBrainsMono-Regular.woff2", "font/woff2", "vendor/jetbrains-mono/JetBrainsMono-Regular.woff2"),
    ("/licenses/streaming-markdown.txt", "text/plain; charset=utf-8", "vendor/streaming-markdown/LICENSE"),
    ("/licenses/JetBrainsMono.txt", "text/plain; charset=utf-8", "vendor/jetbrains-mono/OFL.txt"),
]
def write_assets(stream, name, entries):
    for index, (_, _, data) in enumerate(entries):
        stream.write(f"static const unsigned char {name}_{index}[] = {{\n")
        for at in range(0, len(data), 16):
            stream.write("  " + ",".join(str(c) for c in data[at:at+16]) + ",\n")
        stream.write("};\n")
    stream.write(f"static const sl_www_asset_t {name}[] = {{\n")
    for index, (path, mime, _) in enumerate(entries):
        data = f"{name}_{index}"
        stream.write(f'  {{"{path}", "{mime}", {data}, sizeof({data})}},\n')
    stream.write("};\n")


def read_assets(entries):
    return [(path, mime, (root / source).read_bytes()) for path, mime, source in entries]


if sys.argv[3:] == ["--demo"]:
    demo_assets = [
        ("/assets/vectis-logo.svg", "image/svg+xml", "assets/vectis-logo.svg"),
        ("/licenses/vectis-logo.txt", "text/plain; charset=utf-8", "assets/vectis-logo.LICENSE"),
    ]
    with output.open("w") as stream:
        stream.write("/* Generated: do not edit. */\n")
        write_assets(stream, "demo_assets", read_assets(demo_assets))
    sys.exit(0)

assets = read_assets(assets)
page, mime, html = assets[0]
head = b'  <link rel="stylesheet" href="./motion.css">\n  <script type="module" src="./motion.js"></script>\n'
if html.count(b"</head>") != 1:
    raise ValueError("expected one closing head tag in the web page")
motion_assets = [
    (page, mime, html.replace(b"</head>", head + b"</head>")),
    ("/motion.css", "text/css; charset=utf-8", (root / "web/motion.css").read_bytes()),
    ("/motion.js", "text/javascript; charset=utf-8", (root / "web/motion.js").read_bytes()),
]
with output.open("w") as stream:
    stream.write("/* Generated: do not edit. */\n")
    write_assets(stream, "www_assets", assets)
    write_assets(stream, "www_motion_assets", motion_assets)
