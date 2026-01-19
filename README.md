## 修改的文件
- /frameworks/av/camera/cameraserver/cameraserver.rc修改了cameraservice的权限
- /frameworks/av/services/camera/libcameraservice/CameraService.cpp具备自启动的能力
- /frameworks/av/services/camera/libcameraservice/device3/Camera3OutputUtils.cpp主要劫持的位置
- /frameworks/av/services/camera/libcameraservice/device3/CameraStreamTransmitter.cpp和.h文件，增加通信功能
- /frameworks/av/services/camera/libcameraservice/Android.bp让系统知道还需要编译的地方
- /system/sepolicy/private/cameraserver.te，public里面没有要修改的，这是修改sepolicy

可能还有一些文件需要修改，不过不是什么必要的，到时候根据报错修改一下函数类型就行了。

## 编译和使用方法
命令如下
```bash
source build/envsetup.sh
breakfast tokay
croot
brunch tokay

adb reboot bootloader 
fastboot flash boot boot.img
fastboot flash dtbo dtbo.img
fastboot flash vendor_boot vendor_boot.img
fastboot flash vendor_kernel_boot vendor_kernel_boot.img
```

编译成功后，产物位于 out/target/product/tokay/ 目录下。找到以下文件并下载到电脑上：
- boot.img (内核与引导)
- dtbo.img (设备树叠加层)
- vendor_boot.img (厂商引导)
- vendor_kernel_boot.img (厂商内核引导，Pixel 9 特有)
- lineage-22.1-xxxx-UNOFFICIAL-tokay.zip (完整的系统卡刷包)

进入 LineageOS Recovery,镜像刷完后，在手机上用音量键选择 "Recovery Mode"，按电源键确认。手机会重启并进入 LineageOS 的 Recovery 界面。如果进不去 Recovery，说明上面的镜像没刷对，或者源码编译有问题。

在 Recovery 界面，选择 "Factory Reset" -> "Format data / factory reset"。（首次刷入必须格式化数据，否则会卡在 Google 图标。）以后可以脏刷。

返回主菜单，选择 "Apply Update" -> "Apply from ADB"。在电脑上执行：```adb sideload lineage-22.1-xxxxxxxx-UNOFFICIAL-tokay.zip```进度条跑到 47% 左右可能会停住并在电脑上报错 Total xfer: 1.00x，这是正常的，只要手机端显示成功即可。

回到 Recovery 主菜单，选择 "Reboot system now"。等待开机。第一次开机可能需要几分钟。看到 LineageOS 的开机动画即为成功。

## 可选项
最好还是要在这个时候刷一下google三件套MindTheGapps，以免后面用到。https://wiki.lineageos.org/gapps/

最后，如果要用kernelsu，可以用kernelsu修补一下image，修补的image模块可以通过out/target/product/tokay/ 目录下找到。
