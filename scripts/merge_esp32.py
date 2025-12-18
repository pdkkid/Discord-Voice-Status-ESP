import os
from SCons.Script import Import

Import("env")

def after_build(source, target, env):
    build_dir = env.subst("$BUILD_DIR")

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    app = os.path.join(build_dir, "firmware.bin")

    # Typical ESP32 offsets when flashing (bootloader, partitions, app)
    # (If you change partition scheme, this can change.)
    offset_bootloader = "0x1000"
    offset_partitions = "0x8000"
    offset_app = "0x10000"

    out = os.path.join(build_dir, "firmware_merged.bin")

    # Use PlatformIO's bundled esptool
    esptool = env.subst("$PYTHONEXE") + " " + env.PioPlatform().get_package_dir("tool-esptoolpy") + "/esptool.py"

    cmd = f'{esptool} --chip esp32 merge_bin -o "{out}" {offset_bootloader} "{bootloader}" {offset_partitions} "{partitions}" {offset_app} "{app}"'
    print("Merging ESP32 firmware:", cmd)

    # run
    if env.Execute(cmd) != 0:
        raise Exception("Failed to merge ESP32 binaries")

env.AddPostAction("buildprog", after_build)
