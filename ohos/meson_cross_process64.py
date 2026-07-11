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
    '--target=aarch64-linux-ohosmusl',
    '--sysroot=sysroot_stub',
    '-fno-emulated-tls',
    '-fPIC',
    '-g',
    '-O2',
    '-D_FORTIFY_SOURCE=2',
    '-fstack-protector-all',
    compile_args_stub
    ]

cpp_args = [
    '--target=aarch64-linux-ohosmusl',
    '--sysroot=sysroot_stub',
    '-fno-emulated-tls',
    '-fPIC',
    '-g',
    '-O2',
    '-D_FORTIFY_SOURCE=2',
    '-fstack-protector-all',
    compile_args_stub
    ]
    
c_link_args = [
    '--target=aarch64-linux-ohosmusl',
    '-fPIC',
    '--sysroot=sysroot_stub',
    '-Lsysroot_stub/usr/lib/aarch64-linux-ohos',
    '-Lproject_stub/prebuilts/clang/ohos/linux-x86_64/llvm/lib/clang/current/lib/aarch64-linux-ohos',
    '-Lproject_stub/prebuilts/clang/ohos/linux-x86_64/llvm/lib/aarch64-linux-ohos/c++',
    '--rtlib=compiler-rt',
    link_args_stub
    ]

cpp_link_args = [
    '--target=aarch64-linux-ohosmusl',
    '--sysroot=sysroot_stub',
    '-Lsysroot_stub/usr/lib/aarch64-linux-ohos',
    '-Lproject_stub/prebuilts/clang/ohos/linux-x86_64/llvm/lib/clang/current/lib/aarch64-linux-ohos',
    '-Lproject_stub/prebuilts/clang/ohos/linux-x86_64/llvm/lib/aarch64-linux-ohos/c++',
    '-fPIC',
    '-Wl,--exclude-libs=libunwind_llvm.a',
    '-Wl,--exclude-libs=libc++_static.a',
    '-Wl,--exclude-libs=libvpx_assembly_arm.a',
    '-Wl,--warn-shared-textrel',
    '--rtlib=compiler-rt',
    link_args_stub
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
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
'''

hwasan_compile_args = '''
    '-shared-libasan',
    '-fsanitize=hwaddress',
    '-mllvm',
    '-hwasan-globals=0',
    '-fno-lto',
    '-fno-whole-program-vtables',
'''
hwasan_link_args = '''
    '-shared-libasan',
    '-fsanitize=hwaddress',
'''

swasan_compile_args = '''
    '-fsanitize=address',
    '-fno-inline-functions',
    '-fno-inline',
    '-fsanitize-address-use-after-scope',
    '-fno-omit-frame-pointer',
'''
swasan_link_args = '''
    '-lclang_rt.asan',
'''
noasan_compile_args = '''
'''
noasan_link_args = '''
'''

coverage_compile_args = '''
    '--coverage',
'''
coverage_link_args = '''
    '--coverage',
'''
def generate_cross_file(project_stub_in, sysroot_stub_in, asan_option, coverage_option):
    with open("thirdparty/mesa3d/cross_file", 'w+') as file:
        result = corss_file_content.replace("project_stub", project_stub_in)
        result = result.replace("sysroot_stub", sysroot_stub_in)
        compile_args = ""
        link_args = ""
        if asan_option == "hwasan":
            compile_args = hwasan_compile_args
            link_args = hwasan_link_args
        elif asan_option == "swasan":
            compile_args = swasan_compile_args
            link_args = swasan_link_args
        else:
            compile_args = noasan_compile_args
            link_args = noasan_link_args
        if coverage_option == "use_clang_coverage":
            compile_args = compile_args + coverage_compile_args
            link_args = link_args + coverage_link_args
        result = result.replace("compile_args_stub", compile_args)
        result = result.replace("link_args_stub", link_args)
        file.write(result)
    print("generate_cross_file")

def generate_pc_file(file_raw, project_dir, product_name, skia_version):
    print(file_raw)
    if not os.path.exists('thirdparty/mesa3d/pkgconfig'):
        os.makedirs('thirdparty/mesa3d/pkgconfig')
    shortfilename = ntpath.basename(file_raw)
    filename = 'thirdparty/mesa3d/pkgconfig/'+ shortfilename
    with open(file_raw, 'r+') as file_raw:
        with open(filename, "w+") as pc_file:
            raw_content = file_raw.read()
            raw_content = raw_content.replace("ohos_project_directory_stub", project_dir)
            raw_content = raw_content.replace("ohos-arm-release", product_name)
            raw_content = raw_content.replace("ohos-arm", "ohos-arm64")
            if shortfilename == "expat.pc":
                if skia_version == "new_skia":
                    raw_content = raw_content.replace("skia_folder_stub", "skia/m133")
                    raw_content = raw_content.replace("expat_lib_stub", "expatm133")
                else:
                    raw_content = raw_content.replace("skia_folder_stub", "skia")
                    raw_content = raw_content.replace("expat_lib_stub", "expat")
            pc_file.write(raw_content)
    print("generate_pc_file")

def process_pkgconfig(project_dir, product_name, skia_version):
    template_dir = os.path.split(os.path.abspath( __file__))[0] + r"/pkgconfig_template"
    templates = os.listdir(template_dir)
    for template in templates:
        if not os.path.isdir(template):
            generate_pc_file(template_dir + '/' + template, project_dir, product_name, skia_version)
    print("process_pkgconfig")

def prepare_environment(project_path, product, asan_option, coverage_option, skia_version):
    global project_stub
    global sysroot_stub
    product = product.lower()
    project_stub = project_path
    sysroot_stub = os.path.join(project_stub, "out", product, "obj", "third_party", "musl")
    generate_cross_file(project_path, sysroot_stub, asan_option, coverage_option)
    process_pkgconfig(project_path, product, skia_version)

if __name__ == '__main__':
    if len(sys.argv) < 6:
        print("must input the OpenHarmony directory, the product name, the asan option and the coverage option")
        exit(-1)
    prepare_environment(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
