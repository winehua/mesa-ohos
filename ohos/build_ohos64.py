#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Copyright (c) 2022 Huawei Device Co., Ltd.

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
import ntpath
import os
if __name__ == '__main__':
    if len(sys.argv) < 7:
        print("must input the OpenHarmony directory, the product name, the source dir, the asan option, the coverage option and the skia version")
        exit(-1)
    script_dir = os.path.split(os.path.abspath( __file__))[0]
    run_cross_pross_cmd = 'python3 ' + script_dir + '/meson_cross_process64.py ' + sys.argv[1] + ' ' + sys.argv[2] + ' ' + sys.argv[4] + ' ' + sys.argv[5] + ' ' + sys.argv[6]
    os.system(run_cross_pross_cmd)

    run_build_cmd = 'PKG_CONFIG_PATH=./thirdparty/mesa3d/pkgconfig '
    run_build_cmd += 'meson setup '+ sys.argv[3] + ' thirdparty/mesa3d/build-ohos '
    run_build_cmd += '-Dplatforms=ohos -Degl-native-platform=ohos -Dgallium-drivers=zink -Dbuildtype=release -Degl-lib-suffix=_mesa  \
                      -Dvulkan-drivers= -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true -Dcpp_rtti=false -Dglx=disabled -Dtools= \
                      -Dglvnd=disabled -Dshared-glapi=enabled -Dshader-cache=disabled '
    run_build_cmd += '--cross-file=thirdparty/mesa3d/cross_file '
    run_build_cmd += '--prefix=' + os.getcwd() + '/thirdparty/mesa3d'
    print("build command: %s" %run_build_cmd)
    os.system(run_build_cmd)
    os.system('ninja -C thirdparty/mesa3d/build-ohos -j126')
    os.system('ninja -C thirdparty/mesa3d/build-ohos install')
