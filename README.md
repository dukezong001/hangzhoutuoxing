# hangzhoutuoxing
U盘必须格式化为fat32文件系统格式
在Ubuntu上制作rootfs.ext4镜像方法： https://www.cnblogs.com/dakewei/p/10150984.html dd if=/dev/zero of=rootfs.ext4 bs=1M count=80 mkfs.ext4 rootfs.ext4 mkdir rootf_stmp mount -o loop rootfs.ext4 rootf_stmp/ cp out_rootfs_files rootf_stmp/ umount rootf_stmp/
插上U盘系统上电启动后，会升级u-boot.imx、zimage、b-imx6ull4IGWemmc-emmc.dtb、rootfs.ext4，升级完成后断电重启 如果不插U盘，系统直接启动
升级前左下角led慢闪5次，升级过程常亮，升级完成后快闪5s，如果升级失败灯灭
