# Native module wrapper - re-exports voidview_native
import sys
import os
from pathlib import Path

# Add FFmpeg DLL path (Windows)
_project_root = Path(__file__).parent.parent.parent
_ffmpeg_bin = _project_root / "libs" / "ffmpeg" / "bin"
if _ffmpeg_bin.exists():
    if sys.platform == 'win32' and hasattr(os, 'add_dll_directory'):
        os.add_dll_directory(str(_ffmpeg_bin))
    os.environ["PATH"] = str(_ffmpeg_bin) + os.pathsep + os.environ.get("PATH", "")

# Add native directory to path so voidview_native can be imported
_native_dir = os.path.dirname(os.path.abspath(__file__))
if _native_dir not in sys.path:
    sys.path.insert(0, _native_dir)

from voidview_native import *
import voidview_native

