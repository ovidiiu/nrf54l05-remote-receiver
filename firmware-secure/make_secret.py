#!/usr/bin/env python3
"""Generate common/secret.h with a random 128-bit master key (needed by the secure build)."""
import os, pathlib
k = os.urandom(16)
p = pathlib.Path(__file__).parent / "common" / "secret.h"
p.write_text(
    "/* Generated master secret. Both remote and receiver MUST be built with the same file.\n"
    " * Keep it private (it is git-ignored). */\n#ifndef SECRET_H\n#define SECRET_H\n#include <stdint.h>\n"
    "static const uint8_t MASTER_SECRET[16] = {" + ",".join("0x%02x" % b for b in k) + "};\n#endif\n")
print("wrote", p)
