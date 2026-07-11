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

sysroot_stub = 'sysroot_stub'
project_stub = 'project_stub'

corss_file_content='''
[properties]
needs_exe_wrapper = true

c_args = [
    '-march=armv7-a',
    '-mfloat-abi=softfp',
    '-mtune=generic-armv7-a',
    '-mfpu=neon',
    '-mthumb',
    '--target=arm-linux-ohosmusl',
    '--sysroot=sysroot_stub',
    '-fPIC']

cpp_args = [
    '-march=armv7-a',
    '--target=arm-linux-ohosmusl',
    '--sysroot=sysroot_stub',
    '-fPIC']
    
c_link_args = [
    '-march=armv7-a',
    '--target=arm-linux-ohosmusl',
    '-fPIC',
    '--sysroot=sysroot_stub',
    '-Lsysroot_stub/usr/lib/arm-linux-ohos',
    '-Lproject_stub/prebuilts/clang/ohos/linux-x86_64/llvm/lib/clang/10.0.1/lib/arm-linux-ohos',
    '-Lproject_stub/prebuilts/clang/ohos/linux-x87_64/llvm/lib/arm-linux-ohos/c++',
    '--rtlib=compiler-rt',
    ]

cpp_link_args = [
    '-march=armv7-a',
    '--target=arm-linux-ohosmusl',
    '--sysroot=sysroot_stub',
    '-Lsysroot_stub/usr/lib/arm-linux-ohos',
    '-Lproject_stub/prebuilts/clang/ohos/linux-x86_64/llvm/lib/clang/10.0.1/lib/arm-linux-ohos',
    '-Lproject_stub/prebuilts/clang/ohos/linux-x87_64/llvm/lib/arm-linux-ohos/c++',
    '-fPIC',
    '-Wl,--exclude-libs=libunwind_llvm.a',
    '-Wl,--exclude-libs=libc++_static.a',
    '-Wl,--exclude-libs=libvpx_assembly_arm.a',
    '-Wl,--warn-shared-textrel',
    '--rtlib=compiler-rt',
    ]
	
[binaries]
ar = 'project_stub/prebuilts/clang/ohos/linux-x86_64/llvm/bin/llvm-ar'
c = ['ccache', 'project_stub/prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang']
cpp = ['ccache', 'project_stub/prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang++']
c_ld= 'lld'
cpp_ld = 'lld'
strip = 'project_stub/prebuilts/clang/ohos/linux-x86_64/llvm/bin/llvm-strip'
pkgconfig = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
'''

def generate_cross_file(project_stub_in, sysroot_stub_in):
    with open("cross_file", 'w+') as file:
        result = corss_file_content.replace("project_stub", project_stub_in)
        result = result.replace("sysroot_stub", sysroot_stub_in)
        file.write(result)
    print("generate_cross_file")

def generate_pc_file(file_raw, project_dir, product_name):
    print(file_raw)
    if not os.path.exists('pkgconfig'):
        os.makedirs('pkgconfig')
    filename = 'pkgconfig/'+ ntpath.basename(file_raw)
    with open(file_raw, 'r+') as file_raw:
        with open(filename, "w+") as pc_file:
            raw_content = file_raw.read()
            raw_content = raw_content.replace("ohos_project_directory_stub", project_dir)
            raw_content = raw_content.replace("ohos-arm-release", product_name)
            pc_file.write(raw_content)
    print("generate_pc_file")

def process_pkgconfig(project_dir, product_name):
    template_dir = os.path.split(os.path.abspath( __file__))[0] + r"/pkgconfig_template"
    templates = os.listdir(template_dir)
    for template in templates:
        if not os.path.isdir(template):
            generate_pc_file(template_dir + '/' + template, project_dir, product_name)
    print("process_pkgconfig")

def prepare_environment(project_path, product):
    global project_stub
    global sysroot_stub
    product = product.lower()
    project_stub = project_path
    sysroot_stub = os.path.join(project_stub, "out", product, "obj", "third_party", "musl")
    generate_cross_file(project_path, sysroot_stub)
    process_pkgconfig(project_path, product)

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("must input the OpenHarmony directory and the product name")
        exit(-1)
    prepare_environment(sys.argv[1], sys.argv[2])
