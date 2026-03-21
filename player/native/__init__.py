# Native module wrapper - re-exports voidview_native
import sys
import os
import importlib.util
from pathlib import Path

# Detect Nuitka compiled environment
_is_nuitka = "__compiled__" in globals() or hasattr(sys, 'frozen')

# Determine paths based on environment
if _is_nuitka:
    # Nuitka onefile extracts to temp directory
    # __file__ is like: C:\Users\...\AppData\Local\Temp\ONEFIL~1\player\native\__init__.py
    _base_dir = Path(__file__).parent
    # Go up to temp root: player/native -> player -> temp_root
    _temp_root = _base_dir.parent.parent
    # FFmpeg DLLs are at: temp_root/libs/ffmpeg/bin
    _ffmpeg_bin = _temp_root / "libs" / "ffmpeg" / "bin"
else:
    _base_dir = Path(__file__).parent
    _project_root = _base_dir.parent.parent
    _ffmpeg_bin = _project_root / "libs" / "ffmpeg" / "bin"

# Add FFmpeg DLL path (Windows) - must be done BEFORE importing native module
if _ffmpeg_bin.exists():
    if sys.platform == 'win32' and hasattr(os, 'add_dll_directory'):
        os.add_dll_directory(str(_ffmpeg_bin))
    os.environ["PATH"] = str(_ffmpeg_bin) + os.pathsep + os.environ.get("PATH", "")

# Add native directory to path so voidview_native can be imported
_native_dir = str(_base_dir)
if _native_dir not in sys.path:
    sys.path.insert(0, _native_dir)

# Try standard import first, fall back to explicit .pyd loading for Nuitka
try:
    from voidview_native import *
    import voidview_native
except ImportError:
    # In Nuitka, the .pyd is included as data file, need to load it explicitly
    # Find the .pyd file in the native directory
    _pyd_files = list(_base_dir.glob("voidview_native*.pyd"))
    if _pyd_files:
        _pyd_path = _pyd_files[0]
        # Load the module using importlib
        _spec = importlib.util.spec_from_file_location("voidview_native", _pyd_path)
        voidview_native = importlib.util.module_from_spec(_spec)
        sys.modules["voidview_native"] = voidview_native
        _spec.loader.exec_module(voidview_native)
        # Re-export all public names
        _public_names = [name for name in dir(voidview_native) if not name.startswith('_')]
        globals().update({name: getattr(voidview_native, name) for name in _public_names})
    else:
        raise ImportError("voidview_native module not found")

