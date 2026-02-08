# Acer Brightness module

## Current features
* Customizable attenuation of keyboard light
* Customizable timer to turn it off after a key press

## Warning
Use at your own risk! Acer was not involved in developing this driver, and everything is developed by reverse engineering the official Predator Sense app. This driver interacts with low-level WMI methods that haven't been tested on all series.

## Extra features are welcome but the scope of the module must remain about keyboard lightning.

## Credits
* To JafarAkhondali owner of acer-predator-turbo-and-rgb-keyboard-linux-module, the repo where this mod was inspired.
* To hackbnw owner of faustus, the original module.
* To the Acer-WMI(module from the linux kernel) maintainers, without that research nothing of this would probably exist.

## Will this work on my laptop?

### Compatibility table:
| Product name | Attenuation (Implemented) | Attenuation (Tested)
| -------- | -------- | -------- |
| Predator PHN16-73  | Yes  | Yes  |


## Compilation (May vary with your system, tested on debian)
### Requirements 
1. Install Linux headers and build tools using your distro package manager: Ubuntu (or other Debian based distros):
sudo apt install -y build-essential linux-headers-$(uname -r) gcc make
2. If you use secure boot you need to have your custom MOK.der and MOK.priv to sign the module whose generation is outside this guide

### Instructions
1. Clone the repo and cd into the root dir
2. Run make clean all

## Installation

1. If you use Secure Boot, sign the module.  
   The following command may be useful but may not work with your setup:
```bash
sudo bash -c "/lib/modules/$(uname -r)/build/scripts/sign-file sha256 \
<(sudo openssl rsa -in <MOK.priv full path> -passin pass:<passphrase>) \
<MOK.der full path> \
<acer_brightness.ko full path>"
````
2. Verify the signature:
```bash
modinfo <acer_brightness.ko full path> | grep -E 'signer|sig_key|sig_hashalg'
```
3. Remove the module in case it was previously installed:
```bash
sudo modprobe -r acer_brightness
```
4. Copy the module to the extras directory:
    1. Create the extras directory:
    ```bash
    sudo mkdir -p /lib/modules/$(uname -r)/extra
    ```
    2. Copy the module:
    ```bash
    sudo cp <acer_brightness.ko full path> \
      /lib/modules/$(uname -r)/extra/acer_brightness.ko
    ```
5. Enable the module:
```bash
sudo modprobe acer_brightness
```
6. Verify log errors:
    1. Using journalctl:
    ```bash
    sudo journalctl -b --no-pager | grep -iE 'acer_brigh|modprobe|module'
    ```
    2. Using dmesg:
    ```bash
    sudo dmesg | tail -n 20
    ```
7. If it works without issues, create a `.conf` to load it on startup:
```bash
echo acer_brightness | sudo tee /etc/modules-load.d/acer_brightness.conf
```
## Configuration #Important
The module supports the following options:
* auto_off_ms: Time in milliseconds after a key press to turn the light off, 0 to disable, default 2000
* initial_brightness: Attenuation level from 0 to 100, 0 turns the light off, default 100
* apply_on_load: Whether the module applies anything on load or after the first key press, 0 or 1, default 0
* on_debounce_ms: Time in milliseconds after a keypress when it wont listen to key presses for performance, default o

### Edit config manually (Examples)
```
echo 1 | sudo tee /sys/module/acer_brightness/parameters/apply_on_load
```
```
echo 5000 | sudo tee /sys/module/acer_brightness/parameters/auto_off_ms
```
```
echo 40 | sudo tee /sys/class/leds/acer::kbd_backlight/brightness
```
```
echo 1500 | sudo tee /sys/module/acer_brightness/parameters/on_debounce_ms
```
### Read Config
```
cat /sys/module/acer_brightness/parameters/apply_on_load
```
```
cat /sys/module/acer_brightness/parameters/auto_off_ms
```
```
cat /sys/class/leds/acer::kbd_backlight/brightness
```
```
cat /sys/module/acer_brightness/parameters/on_debounce_ms
```

### Persist config across reboots
1. Create a config file:
```
sudo nano /etc/modprobe.d/acer_brightness.conf
```
3. Write:
```
options acer_brightness \
  auto_off_ms=3000 \
  initial_brightness=20\
  apply_on_load=1 \
  on_debounce_ms=150
```
5. Reload the module to see changes:
    1. ```
       sudo modprobe -r acer_brightness
       ```
    3. ```
       sudo modprobe acer_brightness
       ```
## Uninstall
1. Run `sudo modprobe -r acer_brightness`
2. Delete the module from the system: `sudo rm /lib/modules/$(uname -r)/extra/acer_brightness.ko`
3. Delete the other configuration files if they were created:
    1. ```
       sudo rm /etc/modprobe.d/acer_brightness.conf
       ```
    3. ```
       sudo rm /etc/modules-load.d/acer_brightness.conf
       ```

## Known problems

## FeedBack
If this worked or didn't work for you, kindly make a new issue, and attach the following if possible:
* ```
  sudo dmidecode | grep "Product Name" -B 2 -A 4
  ```
* ```
  lsmod | grep acer_brightness
  ```
* ```
  modinfo acer_brightness
  ```
* ```
  grep . /sys/module/acer_brightness/parameters/*
  ```
* ```
  ls /sys/class/leds | grep acer
  ```
* ```
  sudo dmesg -T | grep -i acer_brightness
  ```
* ```
  sudo dmesg -T | grep -iE 'wmi|acpi|acer'
  ```

## Contributing
### Are you a developer?

1. Fork it!
2. Create your feature branch: git checkout -b my-new-feature
3. Commit your changes: git commit -am 'Add some feature'
4. Push to the branch: git push origin my-new-feature
5. Submit a pull request

## License
[GNU General Public License v3](https://www.gnu.org/licenses/gpl-3.0.en.html)
