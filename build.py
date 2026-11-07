#!/usr/bin/env python3
"""Build using a project-local Python environment, SDK and ARM toolchain."""

import json
import os
from pathlib import Path
import subprocess
import sys
import venv

ROOT = Path(__file__).resolve().parent
VENV = ROOT / ".venv"
SDK_HOME = VENV / "ufbt"
FIRMWARE = "1.5.1"


def run(arguments, **kwargs):
    subprocess.run([str(item) for item in arguments], cwd=ROOT, check=True, **kwargs)


def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "build"
    if action not in ("setup", "build", "test", "format"):
        raise SystemExit("usage: python3 build.py [setup|build|test|format]")
    if not (VENV / "bin/python").exists():
        venv.create(VENV, with_pip=True)
    env = os.environ.copy()
    env["UFBT_HOME"] = str(SDK_HOME)
    env["FBT_TOOLCHAIN_PATH"] = str(SDK_HOME)
    env["PIP_CACHE_DIR"] = str(VENV / "cache/pip")
    if not (VENV / "bin/ufbt").exists() or action == "setup":
        run([VENV / "bin/python", "-m", "pip", "install", "-r",
             ROOT / "requirements-build.txt"], env=env)
    if action == "test":
        output = VENV / "test-session"
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", ROOT,
             ROOT / "session.c", ROOT / "tests/test_session.c", "-o", output])
        run([output])
        activity_output = VENV / "test-activity"
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", ROOT,
             ROOT / "session.c", ROOT / "tests/test_activity.c", "-o", activity_output])
        run([activity_output])
        receiver_output = VENV / "test-receiver"
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", ROOT,
             ROOT / "session.c", ROOT / "receiver.c", ROOT / "tests/test_receiver.c",
             "-o", receiver_output])
        run([receiver_output])
        run([VENV / "bin/python", ROOT / "tests/test_radio_preset.py"])
        run([VENV / "bin/python", ROOT / "tests/test_startup.py"])
        return
    state = SDK_HOME / "current/ufbt_state.json"
    version = (
        json.loads(state.read_text()).get("version") if state.exists() else None
    )
    if version != FIRMWARE:
        run([VENV / "bin/ufbt-bootstrap", "--ufbt-home", SDK_HOME,
             "update", "--branch", FIRMWARE], env=env)
    if action == "setup":
        return
    arguments = [VENV / "bin/ufbt"]
    if action == "format":
        arguments.append("format")
    run(arguments, env=env)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
