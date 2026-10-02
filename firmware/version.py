# PlatformIO pre-build script: stamp local builds with `git describe`
# (e.g. v1.1.0-3-g2e71c36). CI passes FW_VERSION itself, which wins.
import os
import subprocess

Import("env")  # noqa: F821 - provided by PlatformIO

if "FW_VERSION" not in os.environ.get("PLATFORMIO_BUILD_FLAGS", ""):
    try:
        version = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env.subst("$PROJECT_DIR"), text=True, stderr=subprocess.DEVNULL).strip()  # noqa: F821
    except (OSError, subprocess.CalledProcessError):
        version = "dev"
    env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])  # noqa: F821
