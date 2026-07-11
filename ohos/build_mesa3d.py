#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright (c) 2022 Institute of Software, CAS
# Author: xiaofan@iscas.ac.cn

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
import sys
import os
import os.path
import subprocess
import multiprocessing
import glob
import shutil
from meson_cross_process import prepare_environment

# input /path/to/openharmony/out/rk3568

nproc = multiprocessing.cpu_count()
out_dir = sys.argv[1]
mesa3d_install_dir = os.path.join(out_dir, 'packages', 'phone', 'mesa3d')
product = os.path.basename(out_dir)
project_dir = os.path.dirname(os.path.dirname(out_dir))
project_dir = os.path.abspath(project_dir)
mesa3d_dir = os.path.dirname(os.path.dirname(__file__))
mesa3d_dir = os.path.abspath(mesa3d_dir)
pkgconf_dir = os.path.join(mesa3d_dir, './pkgconfig')
os.environ['PKG_CONFIG_PATH'] = pkgconf_dir

# workaround: using system python instead of prebuilt python
path_old = os.environ['PATH'].split(':')
path_filter = lambda p: not p.endswith('/prebuilts/python/linux-x86/3.8.5/bin')
path_new = ':'.join(filter(path_filter, path_old))
os.environ['PATH'] = path_new

os.chdir(mesa3d_dir)
prepare_environment(project_dir, product)

meson_cmd = [
    'meson',
    'setup',
    mesa3d_dir,
    'build-ohos',
    '-Dplatforms=ohos',
    '-Degl-native-platform=ohos',
    '-Ddri-drivers=',
    '-Dgallium-drivers=panfrost',
    '-Dvulkan-drivers=',
    '-Dgbm=enabled',
    '-Degl=enabled',
    '-Dcpp_rtti=false',
    '-Dglx=disabled',
    '-Dtools=panfrost',
    '-Ddri-search-path=/system/lib',
    '--cross-file=cross_file',
    F'--prefix={mesa3d_dir}/build-ohos/install',
]

subprocess.run(meson_cmd, check=True)
subprocess.run(F'ninja -C build-ohos -j{nproc}', shell=True, check=True)
subprocess.run(F'ninja -C build-ohos install', shell=True, check=True)

build_lib = os.path.join(mesa3d_dir, 'build-ohos', 'install', 'lib')
build_lib_dri = os.path.join(build_lib, 'dri')

# install to out/rk3568/packages/phone/mesa3d
shutil.rmtree(mesa3d_install_dir, ignore_errors=True)
os.makedirs(mesa3d_install_dir, exist_ok=True)
for item in glob.glob(os.path.join(build_lib, 'lib*.so.*.*.*')):
    shutil.copy(item, mesa3d_install_dir)
for item in glob.glob(os.path.join(build_lib_dri, '*_dri.so')):
    # all *_dir.so are same file, we need only copy one and rename it to libgallium_dri.so
    shutil.copy(item, os.path.join(mesa3d_install_dir, 'libgallium_dri.so')) # consider create symlink or hardlink
    break
