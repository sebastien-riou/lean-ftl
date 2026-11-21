"""Chunked area, design c2, over the contract of the EWLF log instead of the log itself: the
pool layer of proto_c2.py, word for word, with the commit of a map reduced to its outcomes
(BeAbs in proto_c2.py). Far fewer states than proto_c2, so the pool rules are explored with
more power cuts and more boots; it says nothing about the log, which proto_v4 covers.

Geometry: chunks, pool units, slots per unit.
"""
import os

os.environ['C2_BACKEND'] = 'abs'

from proto_c2 import *  # noqa: E402,F401,F403
from proto_c2 import BACKEND  # noqa: E402

assert BACKEND == 'abs', 'proto_c2 was imported before proto_c2a'
