"""VoidPlayer dev script entry point.

Usage examples: see python dev.py -h
"""

from tools.dev.cli import main


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted.")
