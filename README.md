# cm160
Connect to OWL CM160 energy monitor and relay the live data to MQTT

Based on a number of sources including

* https://github.com/cornetp/eagle-owl
* https://groups.google.com/g/openhab/c/G4_yariNZRk/m/IYo0j6cRCBwJ
* https://sourceforge.net/p/electricowl/discussion/1083264/thread/7f01752f/?limit=25#693c


Sample usage
```
cm160 --host mqtt.local --port 1883 --voltage 230
```

## Building

```
apt install libusb-dev libmosquitto-dev
gcc *.c -Wall -o cm160 -lmosquitto -lusb-1.0
```
