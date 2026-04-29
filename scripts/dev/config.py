"""Project-local configuration for VoidPlayer development commands."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from .paths import ROOT


CONFIG_PATH = ROOT / "dev_config.json"


def load_dev_config(path: Path = CONFIG_PATH) -> None:
    """Load project-local dev settings before running a dev command."""
    if not path.exists():
        return

    with path.open("r", encoding="utf-8") as file:
        config = json.load(file)

    if not isinstance(config, dict):
        raise ValueError(f"{path}: root value must be an object")

    env = config.get("env", config.get("environment", {}))
    if env is None:
        return
    if not isinstance(env, dict):
        raise ValueError(f"{path}: 'env' must be an object")

    _apply_env(env)


def _apply_env(env: dict[str, Any]) -> None:
    for key, value in env.items():
        name = str(key)
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = str(value)
