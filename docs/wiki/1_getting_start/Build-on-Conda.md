<!--- Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved. -->
<!--- SPDX-License-Identifier: Apache-2.0  -->

(Document work-in-progress)

This article introduces the way of using [conda](https://conda.io/) as working environment, as it provides fine-grained version control for rich amount of packages, without asking for root permission.

```bash
# Create an environment called raf-dev
conda create -n raf-dev python=3.7 anaconda ccache cmake cudnn
# Entering the environment
conda activate raf-dev
# Installing necessary packages
conda install clangdev=8.0.1 ipdb -c conda-forge
# Then, verify if llvm are correctly installed
which llvm-config
```

Conda also provides a activation script that is run every time we enter the environment:

```bash
mkdir -p $CONDA_PREFIX/etc/conda/activate.d/
vim $CONDA_PREFIX/etc/conda/activate.d/env_vars.sh
```

We may set environment variables inside this script:

```bash
export CC=$(which gcc)
export CXX=$(which g++)
export RAF_HOME=$HOME/Projects/raf-dev
export TVM_HOME=$RAF_HOME/3rdparty/tvm
export PYTHONPATH=$RAF_HOME/python/:$TVM_HOME/python
export TVM_LIBRARY_PATH=$RAF_HOME/build/lib
export CUDNN_HOME=$CONDA_PREFIX
export LD_LIBRARY_PATH=$CONDA_PREFIX/lib:$LD_LIBRARY_PATH

echo "CC = $CC"
echo "CXX = $CXX"
echo "RAF_HOME = $RAF_HOME"
echo "TVM_HOME = $TVM_HOME"
echo "PYTHONPATH = $PYTHONPATH"
echo "TVM_LIBRARY_PATH = $TVM_LIBRARY_PATH"
echo "CUDNN_HOME = $CUDNN_HOME"
```