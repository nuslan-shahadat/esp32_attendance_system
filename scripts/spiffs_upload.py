"""
scripts/spiffs_upload.py
========================
Overrides PlatformIO's built-in `uploadfs` target so that spiffsgen.py is
called with the SAME parameters that are compiled into the ESP-IDF SPIFFS
library (read from sdkconfig).

Root cause this fixes
---------------------
PlatformIO's built-in uploadfs calls spiffsgen.py with hardcoded defaults,
most critically --obj-name-len 32.  Your sdkconfig has:
    CONFIG_SPIFFS_OBJ_NAME_LEN=64
When the two values differ the SPIFFS library can still mount and fopen()
can still locate a file's directory entry, but fread() calculates the wrong
data-block offset and returns 0 immediately → blank page in Chrome.

Usage (unchanged from before)
------------------------------
    pio run -t upload       # flash firmware
    pio run -t uploadfs     # build + flash SPIFFS with correct parameters
"""

Import("env")  # noqa: F821  (SCons magic — not a real import)

import os
import sys
import subprocess


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _read_sdkconfig(project_dir):
    """Return a dict of key→value from the project's sdkconfig file."""
    for name in ("sdkconfig.esp32-s3-devkitm-1", "sdkconfig"):
        path = os.path.join(project_dir, name)
        if not os.path.isfile(path):
            continue
        cfg = {}
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
        return cfg
    return {}


def _read_partition(project_dir, name="spiffs"):
    """Return (offset, size) for the named partition, or raise."""
    csv_path = os.path.join(project_dir, "partitions.csv")
    with open(csv_path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 5 and cols[0] == name:
                return int(cols[3], 16), int(cols[4], 16)
    raise RuntimeError(f"Partition '{name}' not found in {csv_path}")


def _find_spiffsgen(idf_path):
    # 1. Honour an explicit IDF_PATH if set (native IDF installs)
    if idf_path:
        candidate = os.path.join(idf_path, "components", "spiffs", "spiffsgen.py")
        if os.path.isfile(candidate):
            return candidate

    # 2. PlatformIO bundles its own ESP-IDF — search ~/.platformio/packages/
    pio_home = os.path.expanduser("~/.platformio")
    packages_dir = os.path.join(pio_home, "packages")
    if os.path.isdir(packages_dir):
        for entry in sorted(os.listdir(packages_dir), reverse=True):
            # Package folder is named e.g. "framework-espidf" or "framework-espidf@3.x"
            if not entry.startswith("framework-espidf"):
                continue
            candidate = os.path.join(packages_dir, entry, "components", "spiffs", "spiffsgen.py")
            if os.path.isfile(candidate):
                print(f"  spiffsgen.py  = {candidate}")
                return candidate

    # 3. Ask SCons/PlatformIO directly (most reliable fallback)
    try:
        framework_dir = env.subst("$FRAMEWORK_DIR")  # noqa: F821
        if framework_dir:
            candidate = os.path.join(framework_dir, "components", "spiffs", "spiffsgen.py")
            if os.path.isfile(candidate):
                print(f"  spiffsgen.py  = {candidate}")
                return candidate
    except Exception:
        pass

    raise RuntimeError(
        "spiffsgen.py not found.\n"
        "Searched:\n"
        f"  IDF_PATH       = {idf_path or '(not set)'}\n"
        f"  ~/.platformio/packages/framework-espidf*/components/spiffs/\n"
        "Set IDF_PATH in your environment if using a custom ESP-IDF install."
    )


def _find_esptool():
    """
    Return an argv prefix for running esptool.  Prefer PlatformIO's bundled
    copy; fall back to `python -m esptool` (works if esptool is on sys.path).
    """
    pio_home = os.path.expanduser("~/.platformio")
    for rel in (
        "packages/tool-esptoolpy/esptool.py",
        "packages/tool-esptoolpy/esptool",
    ):
        p = os.path.join(pio_home, rel)
        if os.path.isfile(p):
            return [sys.executable, p]
    # fallback
    return [sys.executable, "-m", "esptool"]


# ---------------------------------------------------------------------------
# Main action
# ---------------------------------------------------------------------------

def _build_and_flash_spiffs(source, target, env):  # noqa: ARG001
    project_dir = env.subst("$PROJECT_DIR")
    build_dir   = env.subst("$BUILD_DIR")
    idf_path    = os.environ.get("IDF_PATH", "")

    # ── 1. Resolve tools ────────────────────────────────────────────────────
    spiffsgen = _find_spiffsgen(idf_path)
    esptool   = _find_esptool()

    # ── 2. Read sdkconfig ───────────────────────────────────────────────────
    cfg = _read_sdkconfig(project_dir)

    page_size    = cfg.get("CONFIG_SPIFFS_PAGE_SIZE",   "256")
    obj_name_len = cfg.get("CONFIG_SPIFFS_OBJ_NAME_LEN", "32")
    meta_len     = cfg.get("CONFIG_SPIFFS_META_LENGTH",   "4")

    print()
    print("=" * 60)
    print("SPIFFS upload — parameters from sdkconfig")
    print(f"  page_size    = {page_size}")
    print(f"  obj_name_len = {obj_name_len}")
    print(f"  meta_len     = {meta_len}")

    # ── 3. Read partition table ──────────────────────────────────────────────
    offset, size = _read_partition(project_dir)
    print(f"  partition    : offset={hex(offset)}, size={hex(size)}")

    # ── 4. Build SPIFFS image ────────────────────────────────────────────────
    data_dir   = os.path.join(project_dir, "data")
    image_path = os.path.join(build_dir, "spiffs.bin")

    print()
    print(f"Building SPIFFS image → {image_path}")
    subprocess.check_call([
        sys.executable, spiffsgen,
        str(size),
        data_dir,
        image_path,
        "--page-size",    page_size,
        "--obj-name-len", obj_name_len,
        "--meta-len",     meta_len,
    ])

    # ── 5. Flash ─────────────────────────────────────────────────────────────
    upload_port  = env.subst("$UPLOAD_PORT")
    upload_speed = env.subst("$UPLOAD_SPEED") or "921600"

    flash_cmd = esptool + [
        "--chip",   "esp32s3",
        "--baud",   upload_speed,
        "--before", "default_reset",
        "--after",  "hard_reset",
        "write_flash", "-z",
        hex(offset), image_path,
    ]

    # Only add --port if PlatformIO resolved one; otherwise let esptool
    # auto-detect (handles the case where the port isn't set yet).
    if upload_port:
        flash_cmd = flash_cmd[:2] + ["--port", upload_port] + flash_cmd[2:]

    print(f"Flashing → {' '.join(flash_cmd)}")
    print("=" * 60)
    subprocess.check_call(flash_cmd)
    print()
    print("✓ SPIFFS upload complete!")
    print()


# ---------------------------------------------------------------------------
# Register the target — overrides PlatformIO's built-in uploadfs.
# Because this script runs as a post: script, the platform has already
# registered its own "uploadfs" target.  We must evict it from
# __PIO_TARGETS before calling AddCustomTarget, otherwise PlatformIO
# raises: AssertionError – assert name not in env["__PIO_TARGETS"]
# ---------------------------------------------------------------------------
_pio_targets = env.get("__PIO_TARGETS", {})  # noqa: F821
if "uploadfs" in _pio_targets:
    del _pio_targets["uploadfs"]

env.AddCustomTarget(  # noqa: F821
    name="uploadfs",
    dependencies=None,
    actions=_build_and_flash_spiffs,
    title="Upload SPIFFS filesystem",
    description=(
        "Builds the SPIFFS image using parameters from sdkconfig "
        "(fixes SPIFFS_OBJ_NAME_LEN mismatch) then flashes it."
    ),
)
