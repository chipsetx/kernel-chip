#!/bin/bash

qemu-system-x86_64 \
-kernel fatbase/kernel \
-initrd \
fatbase/mod/zero.ko,\
fatbase/mod/random.ko,\
fatbase/mod/serial.ko,\
fatbase/mod/debug_sh.ko,\
fatbase/mod/procfs.ko,\
fatbase/mod/tmpfs.ko,\
fatbase/mod/ata.ko,\
fatbase/mod/ext2.ko,\
fatbase/mod/ps2kbd.ko,\
fatbase/mod/ps2mouse.ko,\
fatbase/mod/lfbvideo.ko,\
fatbase/mod/vbox.ko,\
fatbase/mod/vmware.ko,\
fatbase/mod/vidset.ko,\
fatbase/mod/packetfs.ko,\
fatbase/mod/snd.ko,\
fatbase/mod/ac97.ko,\
fatbase/mod/net.ko,\
fatbase/mod/pcnet.ko,\
fatbase/mod/rtl.ko,\
fatbase/mod/e1000.ko,\
fatbase/mod/pcspkr.ko,\
fatbase/mod/portio.ko,\
fatbase/mod/tarfs.ko,\
fatbase/ramdisk.img \
-append "root=/dev/ram0 root_type=tar logtoserial=2 vid=qemu" \
-enable-kvm \
-serial mon:stdio \
-m 1G -soundhw ac97,pcspk

