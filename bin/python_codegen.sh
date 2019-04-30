#!/bin/bash

PYODIDE_ROOT=/usr/local/code/faasm/pyodide
NUMPY_DIR=${PYODIDE_ROOT}/packages/numpy/build/numpy-1.15.1/install/lib/python3.7/site-packages/numpy

find ${NUMPY_DIR} | grep "\.so$" | xargs --max-lines=1 --max-procs=4 /usr/local/code/faasm/cmake-build-debug/bin/codegen
