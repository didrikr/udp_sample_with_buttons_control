# udp_sample_with_buttons_control
Adds button control from the udp sample from the nRF Connect SDK (originally taken from v2.2.0, but
updated to work with v.2.5.x) for the nRF91 series.

Controls:
- Button 1 sends a UDP packet
- Button 2 turns off the modem
- Button 3 toggles PSM (default is PSM enabled)
- Button 4 toggles RAI (default is off)

## Note:
At the moment, this sample is only low power for nRF916**1** and nRF9151 DKs.
The nRF9160 will have elevated current consumption in idle.
I might get around to fixing that sometime.