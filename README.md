# SplashBootScreen

The SplashOS boot screen: the Splash Boot Manager's logo, static, with a
rounded progress bar underneath.

The boot manager plays its logo animation and leaves the **last frame** on
the screen when it kexecs SplashOS. This boot screen paints that same frame
at the same position and size (25% of the screen height, centered, on
black, same resampler as `SplashBootManager/splash-logo.c`), so the handoff
does not jump. Below it sits a progress bar driven by systemd's
`Manager.Progress`.

## Pieces

| File | Role |
|------|------|
| `splash-boot-screen.c` | fbdev renderer + progress loop (`--restore-console`, `--extract IN OUT`) |
| `assets/boot-logo-static.raw.xz` | last frame of the boot manager's `boot-logo.raw` (SPLASHLOGO2, 1 frame) |
| `systemd/splash-boot-screen.service` | runs in the initrd and after switch-root (`quiet` boots only) |
| `systemd/gdm.service.d/` | GDM stops the boot screen before mutter takes the display |
| `systemd/splash-boot-screen-console.service` | gives fbcon + cursor back ~10 s after GDM |
| `systemd/{emergency,rescue}.service.d/` | console back for the emergency shell |
| `dracut/90splash-boot-screen/` | puts the boot screen into the initrd |

## Keeping it in sync with the boot manager

When the boot manager's animation changes, regenerate the asset:

    splash-boot-screen --extract boot-logo.raw boot-logo-static.raw
    xz -9 boot-logo-static.raw && mv boot-logo-static.raw.xz assets/

If `splash-logo.c` changes its geometry or resampler, copy the change here.

## Kernel command line

Add `vt.global_cursor_default=0` next to `quiet` so the console cursor does
not blink between the kernel's display init and the boot screen. Boot with
`nosplash` (or without `quiet`) to get the text console.

## Build

    make && make install DESTDIR=...

Needs libsystemd. Regenerate the initramfs afterwards (dracut).
