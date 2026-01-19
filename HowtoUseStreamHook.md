1、改一下windows电脑上网络的时间跃点，优先选择有线网络

2、确认好ip地址，使用命令```.\pc_receiver.exe 8888 192.168.109.255```

3、通过adb logcat知道要hook的流，或者使用```adb shell dumpsys media.camera > camera_dump_wechat.txt```打印出流，但是这个抓不到瞬时流
通过系统属性控制hook什么流
```
adb shell setprop persist.camera.hook.enable 0 
adb shell setprop persist.camera.hook.stream_ids "" 
adb shell setprop persist.camera.hook.format -1 
adb shell setprop persist.camera.hook.width 0 
adb shell setprop persist.camera.hook.height 0
```
