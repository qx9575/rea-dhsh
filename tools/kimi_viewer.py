#!/usr/bin/env python3
"""kimi_viewer 启动器：python tools/kimi_viewer.py [--shdata DIR]"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from kimi_viewer.__main__ import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
