"""Chunked area, design c2, with the map in the pair: the pool layer of proto_c2.py, word for
word, over the record-in-the-pair variant of the EWLF log (proto_v4p). The map is the data of
the pair and the area has no map units: a pool and two log units.

Geometry: chunks, pool units, slots per unit, two unused figures, then the regular pairs,
reserved pairs and certificates of proto_v4p, 2 each by default.
"""
import os

os.environ['C2_BACKEND'] = 'v4p'

from proto_c2 import *  # noqa: E402,F401,F403
from proto_c2 import BACKEND  # noqa: E402

assert BACKEND == 'v4p', 'proto_c2 was imported before proto_c2p'
