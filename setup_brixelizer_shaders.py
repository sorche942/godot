import os

THIRD_PARTY_SHADERS_DIR = "thirdparty/amd-brixelizer/shaders"
TARGET_DIR = "servers/rendering/renderer_rd/shaders/environment/brixelizer"

os.makedirs(TARGET_DIR, exist_ok=True)

glsl_files = [f for f in os.listdir(THIRD_PARTY_SHADERS_DIR) if f.endswith(".glsl")]

for f in glsl_files:
    content = f"""#[compute]

#version 450

#VERSION_DEFINES
#define FFX_GPU
#define FFX_GLSL 1

#include "{THIRD_PARTY_SHADERS_DIR}/{f}"
"""
    with open(os.path.join(TARGET_DIR, f), "w") as out:
        out.write(content)

print(f"Created {len(glsl_files)} shader wrappers.")

scsub_content = """
Import("env")

if "RD_GLSL" in env["BUILDERS"]:
    glsl_files = Glob("*.glsl")
    for glsl_file in glsl_files:
        env.RD_GLSL(glsl_file)
"""

with open(os.path.join(TARGET_DIR, "SCsub"), "w") as out:
    out.write(scsub_content)

print("Created SCsub.")
