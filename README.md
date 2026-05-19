# IMX296 V4L2 OOT Driver — Jetson Orin Nano Super

Sony IMX296LQR-C (color) / IMX296LLR-C (mono) out-of-tree V4L2 driver for L4T 5.15.148-tegra / JetPack 6.

**Platform:** Jetson Orin Nano Super (p3767/p3768 devkit)  
**Sensor:** Waveshare IMX296, CSI-2, 2-lane, I2C address `0x1a`  
**Clock:** 51 MHz via bpmp EXTPERIPH1 (nearest clean division from PLLP_OUT0 408 MHz)

---

## Repository contents

```
imx296.c                   — V4L2 sensor driver (OOT)
imx296_cam0_overlay.dts    — Device tree overlay source (CAM0, I2C bus 9)
Makefile                   — Out-of-tree kernel module build
README.md                  — This file
```

---

## Prerequisites

```bash
sudo apt install nvidia-l4t-kernel-headers nvidia-l4t-kernel-oot-headers \
                 build-essential device-tree-compiler
```

---

## Fresh build and deploy

### 1. Install kernel headers

```bash
sudo apt install nvidia-l4t-kernel-headers nvidia-l4t-kernel-oot-headers
```

### 2. Build the module

```bash
make
```

### 3. Install the module

```bash
sudo cp imx296.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
```

### 4. Back up the active DTB — do this before any overlay work

```bash
sudo cp /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb \
        /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb.bak
```

Verify:
```bash
ls -lh /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb.bak
```
Add backup boot option to extlinux
```
sudo nano /boot/extlinux/extlinux.conf

```

```
TIMEOUT 50
DEFAULT primary

MENU TITLE L4T boot options

LABEL primary
  MENU LABEL primary kernel
  LINUX /boot/Image
  INITRD /boot/initrd
  APPEND ${cbootargs} quiet root=/dev/mmcblk0p1 rw rootwait rootfstype=ext4
  FDT /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb

LABEL backup
  MENU LABEL backup - original DTB
  LINUX /boot/Image
  INITRD /boot/initrd
  APPEND ${cbootargs} quiet root=/dev/mmcblk0p1 rw rootwait rootfstype=ext4
  FDT /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb.bak
```


### 5. Compile the device tree overlay

```bash
dtc -@ -I dts -O dtb -o imx296_cam0_overlay.dtbo imx296_cam0_overlay.dts
```

Warnings about `unit_address_vs_reg` and `graph_child_address` are harmless.

### 6. Apply overlay to the backup DTB

Always apply against `.bak`, never against the already-patched active DTB.

```bash
sudo fdtoverlay -i /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb.bak \
                -o /tmp/combined.dtb \
                imx296_cam0_overlay.dtbo
```

### 7. Verify the clock rate is correct in the combined DTB

```bash
dtc -I dtb -O dts /tmp/combined.dtb 2>/dev/null \
    | grep -A3 "assigned-clock-rates" | grep "0x30a7040\|51000000"
```

Should return one match for the camera node.

### 8. Copy combined DTB into place

```bash
sudo cp /tmp/combined.dtb \
        /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb
```

### 9. Reboot

```bash
sudo reboot
```

---

## After reboot — verify

```bash
# Module loaded and sensor probed
sudo dmesg | grep -E "imx296|nvcsi|INCKSEL"

# Device node exists
ls /dev/video0

# Attempt a single frame capture
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=1456,height=1088,pixelformat=RG10 \
  --stream-mmap --stream-count=1 \
  --stream-to=/tmp/test.raw
```

Expected dmesg output:
```
imx296 2-001a: IMX296 probed: color, 2 lanes, 51000000 Hz INCK
tegra-camrtc-capture-vi tegra-capture-vi: subdev imx296 2-001a bound
```

---

## Recovery — if the system fails to boot

If the DTB change causes a boot failure, restore the backup from another terminal or SSH session:

```bash
sudo cp /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb.bak \
        /boot/tegra234-p3768-0000+p3767-0005-nv-super.dtb
sudo reboot
```

---

## Rebuilding after driver changes

```bash
make clean && make
sudo cp imx296.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
sudo reboot
```

The DTB does not need to be reapplied unless `imx296_cam0_overlay.dts` changed.

---

## Key register notes

| Register | Address | Value | Source |
|---|---|---|---|
| CSI activation | 0x3005 | 0xf0 | Required — enables MIPI output |
| INCKSEL 0–3 | 0x3089–0x308c | b0 0f b0 0c | Confirmed via RPi i2c trace |
| CTRL418C | 0x418c | 0xa8 | Confirmed via RPi i2c trace |
| GTTABLENUM | 0x4114 | 0xc5 | Confirmed via RPi i2c trace |
| VMAX default | 0x3010 | 1109 | ~60 fps at 51 MHz |
| HMAX | 0x3014 | 1100 | Fixed horizontal blanking |

INCKSEL values correspond to the 54 MHz entry in the mainline Linux driver. The Waveshare IMX296 module uses 54 MHz clock settings. The Jetson bpmp delivers 51 MHz (nearest clean value: 408 MHz / 8). The 5.5% deviation may require INCKSEL tuning if streaming fails.

---

## Capture pipeline

```
IMX296 sensor
  │  I2C (config)     CSI-2 2-lane (data)
  └──────────────────────────────────────▶ NVCSI ──▶ VI5 ──▶ /dev/video0
```

Format: RAW10, 1456×1088, pixel format `RG10` (RGGB Bayer).

## TODO — current build fixes required

- [x] **Driver — remove SENSOR_INFO probe block** (`imx296.c`)  
  The `imx296_power_on` / `regmap_write(IMX296_STANDBY, 0x00)` block in `imx296_probe`
  causes a kernel oops during boot by briefly taking the sensor out of standby while
  NVCSI is initialising. Remove it entirely — it was debug-only.

- [ ] **Driver — accept 51 MHz clock** (`imx296.c`)  
  bpmp delivers 51000000 Hz (408 MHz / 8), not 54 MHz. Update the clock check:  
  `if (sensor->clk_freq != 54000000 && sensor->clk_freq != 51000000)`

- [ ] **DTS — set clock rate to 51 MHz** (`imx296_cam0_overlay.dts`)  
  Change `assigned-clock-rates = <54000000>` to `assigned-clock-rates = <51000000>`

- [ ] **DTS — add extlinux backup boot entry** (`/boot/extlinux/extlinux.conf`)  
  Add a `LABEL backup` entry pointing to `.dtb.bak` with `TIMEOUT 50`.
  Prevents needing reflash if a bad DTB is applied. Do this once after first backup.

- [ ] **Verify INCKSEL values work at 51 MHz**  
  Current table uses 54 MHz INCKSEL values `{0xb0, 0x0f, 0xb0, 0x0c}` with
  CTRL418C = 0xa8. These were confirmed on RPi at exactly 54 MHz. At 51 MHz (5.5% low)
  the sensor PLL may not lock. If STREAMON still returns EIO after the above fixes,
  the INCKSEL values need tuning for 51 MHz — no confirmed values exist yet for this
  frequency. Options: find a clock source that delivers exactly 54 MHz, or
  empirically tune INCKSEL for 51 MHz.Sonnet 4.6Adaptive