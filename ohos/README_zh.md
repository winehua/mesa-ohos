### 编译环境依赖：
1. meson
mesa是使用meson进行编译的并要求版本>= 0.52，可执行以下命令安装meson
```
sudo apt-get install python3 python3-pip ninja-build
pip3 install --user meson
```
2. pkg-config
meson配置编译环境时会使用pkg-config查找依赖库和头文件，可以执行以下命令安装
```
sudo apt-get install pkg-config 
```

### 编译步骤如下：
1. 下载openharmony整个工程代码，并完成编译
2. 因为有些依赖库是静态库，整编后会被删除需要执行以下命令单独编译以rk3568为例
```
./build.sh --product-name=rk3568 --build-target=expat
./build.sh --product-name=rk3568 --build-target=libwayland_server.0
./build.sh --product-name=rk3568 --build-target=libwayland_client.0
./build.sh --product-name=rk3568 --build-target=libwayland_server
./build.sh --product-name=rk3568 --build-target=libwayland_client
```

3. 编译mesa
编译鸿蒙系统下的mesa库时，需要配置鸿蒙的交叉编译链和依赖的库，相关配置和编译步骤已经封装到ohos目录下的python脚本中，编译时只需执行 build_wayland_and_gbm.py 脚本即可，执行命名需要输入三个参数

|  参数   | 说明  |
|  ----  | ----  |
| arg1   | openharmony代码路径 |
| arg2   | 产品名字，实际上为out目录下存放系统编译结果的那个目录 如hi3516dv300 或则 rk3568， 注： LTS3.0 的版本必须是 ohos-arm-release|
| arg3   | mesa 源码路径 |

示例如下：
~/openharmony  是openharmony源码路径， rk3568是对应的产品输出目录，~/mesa3d为mesa源码路径, 执行完命令后会生成一个名叫builddir的文件夹，该文件夹是mesa的编译目录. 编译完成后相关的库存放在builddir/install/lib 下

```
cd ~/mesa3d
python ohos/build_ohos.py ~/openharmony rk3568 ~/mesa3d
python ohos/build_ohos64.py ~/ohos_50 rpi4 ~/ohos_50/third_party/mesa3d-opc
python ohos/build_ohos64.py ~/ohos_40 beryllium ~/ohos_40/third_party/mesa3d
python ohos/build_ohos64.py ~/ohos_40 sagit ~/ohos_40/third_party/mesa3d

```