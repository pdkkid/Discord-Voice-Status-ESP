import os
from SCons.Script import Import

Import("env")

def after_build(source, target, env):
    build_dir = env.subst("$BUILD_DIR")

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    app = os.path.join(build_dir, "firmware.bin")

    out = os.path.join(build_dir, "firmware_merged.bin")

    esptool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    esptool_py = os.path.join(esptool_dir, "esptool.py")

    cmd = (
        f'"{env.subst("$PYTHONEXE")}" "{esptool_py}" --chip esp32 merge_bin -o "{out}" '
        f'0x1000 "{bootloader}" 0x8000 "{partitions}" 0x10000 "{app}"'
    )

    print("Merging ESP32 firmware:", cmd)
    if env.Execute(cmd) != 0:
        raise Exception("Failed to merge ESP32 binaries")

env.AddPostAction("buildprog", after_build)
