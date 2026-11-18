"""py2app recipe for the Mac app: `make app` runs it, nothing else should.

python.org's Python is the only one whose Tk opens a window on Big Sur, and
its binaries reference each other by absolute path under /Library/Frameworks;
py2app copies the framework into the bundle and rewrites those references,
which is the whole reason it is used. The version comes from VERSION, as
everywhere else. GPLv3.
"""
import os

from setuptools import setup

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

with open(os.path.join(ROOT, "VERSION")) as f:
    VERSION = f.read().strip()

setup(
    name="v7dvs",
    version=VERSION,
    app=[os.path.join(ROOT, "launcher.py")],
    data_files=[
        os.path.join(ROOT, "v7d"),
        os.path.join(ROOT, "VERSION"),
        os.path.join(HERE, "README-macos.md"),
    ],
    options={
        "py2app": {
            "includes": ["configwin", "tkinter"],
            "excludes": ["numpy", "usb1", "rtmidi", "sounddevice", "test",
                         "unittest", "distutils", "setuptools", "pip"],
            "arch": "x86_64",
            "argv_emulation": False,
            "plist": {
                "CFBundleIdentifier": "org.hyperwindmill.v7dvs",
                "CFBundleName": "v7dvs",
                "CFBundleDisplayName": "v7dvs",
                "CFBundleShortVersionString": VERSION,
                "CFBundleVersion": VERSION,
                "LSMinimumSystemVersion": "11.0",
                "NSHumanReadableCopyright": "GPLv3",
                "NSHighResolutionCapable": True,
            },
        }
    },
    setup_requires=["py2app"],
)

