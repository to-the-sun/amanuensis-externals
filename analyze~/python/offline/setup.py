from setuptools import setup, Extension
from Cython.Build import cythonize
import numpy as np
import os

extensions = [
    Extension(
        "cumulative_transience",
        sources=["ct_extension.pyx", "cumulative_transience.c", "ct_exporter.c"],
        include_dirs=[np.get_include(), "."],
        define_macros=[("NPY_NO_DEPRECATED_API", "NPY_1_7_API_VERSION")],
        extra_compile_args=["-O3"] if os.name != "nt" else ["/O2"],
    )
]

setup(
    name="cumulative_transience",
    ext_modules=cythonize(extensions, language_level="3"),
)
