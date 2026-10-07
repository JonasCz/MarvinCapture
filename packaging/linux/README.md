# Debian package

`scripts/package-linux.sh` builds `marvincapture_<version>_<arch>.deb` (CLI only;
there is no Linux GUI) into `build/linux-<arch>/installer/`. It runs
`scripts/build.sh --skip-gui` first unless `--skip-build` is given. Needs only
`dpkg-deb` (plus `objdump` and `strip` from binutils) on top of the build prerequisites.

| Installed | |
|---|---|
| `/usr/lib/marvincapture/` | `MarvinCaptureCLI`, `libmarvin-core.so`, `firmware/` |
| `/usr/bin/MarvinCaptureCLI` | symlink to the above (`$ORIGIN` rpath and the core's firmware lookup both use the real path) |
| `/usr/lib/udev/rules.d/60-marvincapture.rules` | device access for the five Marvin PIDs |
| `/usr/lib/udev/rules.d/60-marvincapture-cpu-dma-latency.rules` | `/dev/cpu_dma_latency` access |
| `/usr/share/doc/marvincapture/copyright` | DEP-5 |

`postinst`/`postrm` reload udev rules and re-trigger the devices (skipped without
a running udev). The only dependencies are libusb and glibc (the script computes
the glibc minimum from the binaries); FFmpeg is linked statically.
