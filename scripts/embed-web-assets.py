"""CMake-owned source generator. No package manager or JS bundler is used."""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
assets = [
    ("/", "text/html; charset=utf-8", "web/index.html"),
    ("/softline.css", "text/css; charset=utf-8", "web/softline.css"),
    ("/softline.js", "text/javascript; charset=utf-8", "web/softline.js"),
    ("/markdown.js", "text/javascript; charset=utf-8", "web/markdown.js"),
    ("/smd.js", "text/javascript; charset=utf-8", "vendor/streaming-markdown/smd.js"),
    ("/JetBrainsMono-Regular.woff2", "font/woff2", "vendor/jetbrains-mono/JetBrainsMono-Regular.woff2"),
    ("/licenses/streaming-markdown.txt", "text/plain; charset=utf-8", "vendor/streaming-markdown/LICENSE"),
    ("/licenses/JetBrainsMono.txt", "text/plain; charset=utf-8", "vendor/jetbrains-mono/OFL.txt"),
]
with output.open("w") as stream:
    stream.write("/* Generated: do not edit. */\n")
    for index, (_, _, path) in enumerate(assets):
        data = (root / path).read_bytes()
        stream.write(f"static const unsigned char www_asset_{index}[] = {{\n")
        for at in range(0, len(data), 16):
            stream.write("  " + ",".join(str(c) for c in data[at:at+16]) + ",\n")
        stream.write("};\n")
    stream.write("static const sl_www_asset_t www_assets[] = {\n")
    for index, (path, mime, _) in enumerate(assets):
        stream.write(f'  {{"{path}", "{mime}", www_asset_{index}, sizeof(www_asset_{index})}},\n')
    stream.write("};\n")
