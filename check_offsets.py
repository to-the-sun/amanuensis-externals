import numpy as np
import sys
import os

extension_path = os.path.abspath("analyze~/python")
sys.path.insert(0, extension_path)

import cumulative_transience as ct
# We can't easily check offsets from Python unless we use ctypes,
# but we can check if the data read makes sense.
# Actually, let's just use the C results as the truth.
