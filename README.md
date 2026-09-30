# AirQualitySensor

Thingy:53 firmware using the nRF7002EB Wi-Fi expansion board to send BME688
measurements to the PHP telemetry endpoint and display them on the dashboard.

## Configure Wi-Fi

The nRF7002EB connects to 2.4 GHz Wi-Fi networks. Copy
`src/wifi_config.h.example` to `src/wifi_config.h`, then set the network SSID
and WPA/WPA2 password in the new local file. The local configuration is ignored
by Git so credentials are not committed. Firmware startup reports an error if
either value is still empty.

```powershell
Copy-Item src/wifi_config.h.example src/wifi_config.h
```

## Build and flash

This application uses Wi-Fi on the application core and does not use the
nRF5340 network core, so sysbuild leaves that image out. From an nRF Connect
SDK terminal, build for Thingy:53 with the `nrf7002eb` shield, then flash the
board using the normal west commands for that target:

```powershell
west build -b thingy53/nrf5340/cpuapp --shield nrf7002eb -p always
west flash
```

Connect to the board's serial console to see Wi-Fi association, DHCP, sensor
sampling, and HTTP response status messages. The RGB LED shows connection,
network, telemetry, and error states.
