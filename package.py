"""Rez package description."""

name = "ptex"
uuid = name
authors = ["wdas"]
description = (
    """
    Ptex is a texture mapping system developed by
    Walt Disney Animation Studios for production-quality rendering
    """
)
tools = ["ptxinfo"]
requires = []
build_system = "cmake"
private_build_requires = [
    "cmake",
    "~doxygen",
    "~graphviz",
]


def commands():
    env.PATH.append("{root}/bin")
