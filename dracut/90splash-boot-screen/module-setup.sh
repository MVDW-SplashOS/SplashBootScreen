#!/bin/bash
# dracut module: run the SplashOS boot screen in the initrd too, so the
# logo is back on the screen right after the kernel (re)initializes the
# display instead of only after switch-root.

check() {
    require_binaries /usr/bin/splash-boot-screen || return 1
    return 0
}

depends() {
    echo systemd
    return 0
}

install() {
    inst_multiple /usr/bin/splash-boot-screen \
        /usr/share/splashos/boot-logo-static.raw
    inst_simple "$systemdsystemunitdir/splash-boot-screen.service"
    inst_simple "$systemdsystemunitdir/emergency.service.d/50-splash-boot-screen.conf"
    inst_simple "$systemdsystemunitdir/rescue.service.d/50-splash-boot-screen.conf"
    $SYSTEMCTL -q --root "$initdir" add-wants sysinit.target splash-boot-screen.service
}
