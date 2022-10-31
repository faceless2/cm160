# cm160
Connect to OWL CM160 energy monitor and relay the live data to MQTT

This is a cut down version of originals found at

* https://github.com/cornetp/eagle-owl
* https://groups.google.com/g/openhab/c/G4_yariNZRk/m/IYo0j6cRCBwJ

Sample usage
```
cm160 --host mqtt.local --port 1883 --voltage 230
```

## Building

```
apt install libusb-dev libmosquitto-dev
gcc *.c -Wall -o cm160 -lmosquitto -lusb
```
