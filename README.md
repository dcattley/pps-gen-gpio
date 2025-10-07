# pps-gen-gpio #
============

Linux kernel PPS generator using GPIO pins.

In kernel 4.14 there is no support for using a GPIO pin as a PPS generator, only a GPIO PPS client is available. This driver is derived from the current parallel port PPS generator and provides a PPS signal through a GPIO pin specified in the device tree. The PPS signal is synchronized to the tv_sec increment of the wall clock.

We have tested the driver with kernel 4.14.20 on a Beaglebone Black where P9 pin 16 (GPIO1_19) is used as PPS output. The corresponding modified device tree file is here shown.

## Beaglebone Black ##
If you use a Beaglebone Black and want to change the PPS output pin, you have to modify your device tree file (am335x-boneblack.dts) accordingly:
- Make sure you have the right pin multiplexing setting:

		pps_gen_pins: pinmux_pps_gen_pins {
			pinctrl-single,pins = <
				0x4C (PIN_OUTPUT_PULLDOWN | MUX_MODE7) /* gpmc_a3.gpio1_19 */

Check the [BBB System Reference Manual](https://github.com/CircuitCo/BeagleBone-Black/blob/master/BBB_SRM.pdf?raw=true) and the [am335x Technical Reference Manual](http://www.ti.com/lit/ug/spruh73k/spruh73k.pdf) to find out the correct pinmux table offset of the chosen pin.
- The pps-gen-gpios property defines the pin you want to use as a PPS output:

		pps-gen {
				pinctrl-names = "default";
				pinctrl-0 = <&pps_gen_pins>;
				compatible = "pps-gen-gpio";
				gpios = <&gpio1 19 GPIO_ACTIVE_HIGH>;
				default-state = "off";

Please note that in order to use the module with any other board using the device tree infrastructure, the following matching definitions are required in the device tree:

		pps-gen               node defined for the PPS GPIO
		pps-gen-gpio          value of ".compatible" property in pps-gen node
		gpios                 property in pps-gen node that defines which GPIO pin is used

After modifying the device tree, add the files into drivers/pps/generators and configure the driver to be built as a module. You need to enable PPS support in the kernel.

## Raspberry Pi ##
Clone or copy this repository to the RPI.  DKMS will be used to manage compiling and installing the driver as a module from sources.

### Install with DKMS ###
```
# Install prerequisites
sudo apt install gcc dkms raspberrypi-kernel-headers  # For Raspberry Pi OS
# sudo apt install gcc dkms linux-headers-$(uname -r)  # For Ubuntu

# Clone or extract repo
# git clone <git-system>/pps-gen-gpio.git
cd pps-gen-gpio

sudo dkms remove pps-gen-gpio -v 0.1
sudo dkms add .
sudo dkms build   --force pps-gen-gpio -v 0.1
sudo dkms install --force pps-gen-gpio -v 0.1
```
or a one-liner
```
sudo dkms remove pps-gen-gpio/0.1 ; sudo dkms add . && sudo dkms build --force pps-gen-gpio -v 0.1 && sudo dkms install --force pps-gen-gpio -v 0.1
```

### Compile and Install Device Tree Overlay ###
Compile directly on target with:
```
sudo dtc -@ -I dts -O dtb -o /boot/firmware/overlays/pps-gen-gpio.dtbo ./dts/overlays/pps-gen-gpio-overlay.dts
```

NOTE:  The source file name ends in ```-overlay``` but the generated file omits the ```-overlay```.

### Edit ```/boot/firmware/config.txt``` ###
Add overlay lines to ```/boot/firmware/config.txt```:
```
sudo nano /boot/firmware/config.txt
```

Add one line for each gpio at the end of the file:
```
# Enable pps-gen-gpio
dtoverlay=pps-gen-gpio,gpio=17
dtoverlay=pps-gen-gpio,gpio=27
dtoverlay=pps-gen-gpio,gpio=22
dtoverlay=pps-gen-gpio,gpio=23
dtoverlay=pps-gen-gpio,gpio=24
dtoverlay=pps-gen-gpio,gpio=25
```
