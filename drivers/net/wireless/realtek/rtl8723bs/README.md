# Realtek 8723bs Linux WIFI driver with Support for Baytrail Tablets

tested on Trekstor Surftab Wintron 10.1 

To cross-compile for ARM (eg. for a @NextThing CHIP), do something like this:

````
make CONFIG_PLATFORM_ARM_SUNXI=y ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C /path/to/kernel_compile_O_dir M=$PWD CONFIG_RTL8723BS=m -j$(nproc)
make CONFIG_PLATFORM_ARM_SUNXI=y ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C /path/to/kernel_compile_O_dir M=$PWD CONFIG_RTL8723BS=m INSTALL_MOD_PATH=/tmp/chiplinux modules_install
```

To compile for the host, do something like this:

```
sudo apt-get install build-essential linux-headers-generic git
git clone https://github.com/muhviehstah/rtl8723bs.git
cd rtl8723bs
make
sudo make install
sudo depmod -a
sudo modprobe 8723bs
```
