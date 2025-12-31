import re
import os

files = [
    "thirdparty/amd-brixelizer/shaders/ffx_brixelizer_callbacks_glsl.h",
    "thirdparty/amd-brixelizer/shaders/ffx_brixelizergi_callbacks_glsl.h"
]

defines = set()

for file_path in files:
    with open(file_path, "r") as f:
        content = f.read()
        matches = re.findall(r"BRIXELIZER(?:_GI)?_BIND_\w+", content)
        defines.update(matches)

sorted_defines = sorted(list(defines))

header_content = "#ifndef FFX_BRIXELIZER_DEFINES_H\n#define FFX_BRIXELIZER_DEFINES_H\n\n"
for i, define in enumerate(sorted_defines):
    header_content += f"#define {define} {i}\n"

header_content += "\n#endif // FFX_BRIXELIZER_DEFINES_H\n"

with open("servers/rendering/renderer_rd/environment/brixelizer/ffx_brixelizer_defines.h", "w") as f:
    f.write(header_content)

print(header_content)
