# rtl8723bu
Driver for Realtek RTL8723BU Wireless Adapter with Hardware ID `0bda:b720`

# How to use?
## a. Concurrent Mode
If you want to operate the hardware as a station AND as an access point *simultaneously*, follow these instructions.  This will show two devices when you run the `iwconfig` command.

Run the following commands in the Linux terminal.

```
git clone https://github.com/lwfinger/rtl8723bu.git
cd rtl8723bu
make
sudo make install
sudo modprobe -v 8723bu

```

## b. Non Concurrent Mode
If you do not want two devices (station and an access point) separately, then follow these instructions.

Step - 1: Run the following commands in the Linux terminal. 
```
git clone https://github.com/lwfinger/rtl8723bu.git
cd rtl8723bu
nano Makefile
```

Step - 2: Find the line that contains `EXTRA_CFLAGS += -DCONFIG_CONCURRENT_MODE` and insert a `#` symbol at the beginning of that line. This comments that line and disables concurrent mode.

Step - 3: Now, run the following commands in the same Linux terminal.

```
make
sudo make install
sudo modprobe -v 8723bu
