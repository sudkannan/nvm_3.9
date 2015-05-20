#!/bin/bash -x

#fakeroot make-kpkg clean
#fakeroot make-kpkg --initrd kernel-image kernel-headers
#dpkg -i ../linux-image-3.9.0_3.9.0-10.00.Custom_amd64.deb
#dpkg -i ../linux-headers-3.9.0_3.9.0-10.00.Custom_amd64.deb
#dpkg -i ../linux-headers-2.6.32.49+drm33.21_2.6.32.49+drm33.21-10.00.Custom_amd64.deb
#exit


CC=/usr/lib/ccache/bin/gcc make -j15 &>compile.out
CC=/usr/lib/ccache/bin/gcc make bzImage -j15 &>>compile.out
CC=/usr/lib/ccache/bin/gcc make  modules -j15
CC=/usr/lib/ccache/bin/gcc make  modules_install -j15

 y="3.9.0+"	
   if [[ x$ == x ]];
  then
      echo You have to say a version!
      exit 1
   fi

cp ./arch/x86/boot/bzImage /boot/vmlinuz-$y
cp System.map /boot/System.map-$y
cp .config /boot/config-$y
update-initramfs -c -k $y
echo Now edit menu.lst or run /sbin/update-grub

grep -r "warning:" compile.out &> warnings.out
grep -r "error:" compile.out &> errors.out
./copy.sh
#sudo reboot
