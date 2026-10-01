# AirQualitySensor

Thingy:53 firmware using the nRF7002EB Wi-Fi expansion board to send BME688
measurements to the PHP telemetry endpoint and display them on the dashboard.

## Configure Wi-Fi

The nRF7002EB connects to 2.4 GHz Wi-Fi networks. Copy
`src/wifi_config.h.example` to `src/wifi_config.h`, then set the network SSID
and WPA/WPA2 password in the new local file. The local configuration is ignored
by Git so credentials are not committed. If both values are blank, firmware
uses Wi-Fi credentials already saved in device settings; if settings are also
empty, it reports an error.

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

If the serial console is unavailable, a flashing blue LED means the firmware
is waiting for the Wi-Fi driver or access point. If association or DHCP fails,
the firmware shows a repeating red pulse code before retrying: 1 pulse means
the access point was not found, 2 means authentication/password failure,
3 means connection timeout, 4 means another connection-request failure, and
5 means Wi-Fi connected but DHCP did not complete. After DHCP, solid blue
means DNS lookup is in progress, solid purple means the HTTPS/TLS connection
is in progress, and solid yellow means the HTTP POST is in progress. Green
indicates telemetry accepted by the server. Red pulse codes 6, 7, and 8 indicate,
respectively, DNS lookup failure, HTTPS/TLS connection failure, and HTTP POST
or server-response failure. These codes repeat three times before the next
telemetry attempt. DNS lookup, TCP connect, and TLS handshake each have a
finite timeout, so a failure should return to the retry loop rather than
leaving the firmware stuck indefinitely.
