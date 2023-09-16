ESP32 Failsafe switch for Home Assistant

![sgready](https://github.com/velvet-jones/sgready/assets/2877548/6b257951-6b20-4478-a085-fba193280c0e)

Hardware: ESP32 with i2c OLED https://randomnerdtutorials.com/esp32-built-in-oled-ssd1306

NOTE: You must rename 'credentials_template.h' to 'credentials.h'
      and put in your own network credentials!

----
My home heat pump supports the new "Smart Grid Ready" (SG Ready) standard. This standard
defines four modes of operation, implemented on the pump as two switches representing
binary digits.

The SG Ready modes are:

  00 - Normal
  01 - Excess (Low price)
  10 - Block (Pause functioning)
  11 - Force (Force functioning)

At present I am experimenting with only two of these modes, "Excess" and "Normal". In Excess
mode the pump is encouraged to use electricity because it is free or inexpensive. In "Normal"
mode the pump chases the lowest price on the Nordpool spot market.

This project implements a smarthome switch that has a fallback mode that reverts the
pump to Normal mode if MQTT communications are lost for a period of time.

Transition: A transtion is when the pump changes from one mode to a different mode.
            The pump must not transition more often than every ten(10) minutes (SG Ready spec).

The heat pump mode is controlled using a microcontroller that implements a remote switch and a
sensor. The switch represents the desired mode and the sensor represents the current mode.
The microcontroller uses the Home Assistant MQTT Discovery mechanism over WiFi to automatically
register a device named "SGReady" having these two entities. The switch entity represents the
desired mode of the heat pump, and it can be set as often as you wish. In accordance with the
SG Ready specification, the microcontroller ensures that the heat pump stays in any given mode
for at least 10 minutes. It also reverts the pump to Normal mode if MQTT publications are not
acknowledged for a certain period of time (3 publication intervals).

To use this microcontrolled switch I have created a Threshold sensor in Home Assistant that
triggers when my grid export power exceeds a certain amount, and an Automation that flips
the SGReady microcontroller switch to 'ON' if the threshold has been exceeded for at least 2
minutes or 'OFF' if the exceeded threshold subsequently subsides for any length of time. The
YAML for the automation is:

            alias: SGReady.Excess
            description: >-
              Changes the SGReady mode on the heat pump to encourage using excess solar
              production.
            trigger:
              - platform: state
                entity_id:
                  - binary_sensor.sgready_export_threshold
                for:
                  hours: 0
                  minutes: 2
                  seconds: 0
                to: "on"
              - platform: state
                entity_id:
                  - binary_sensor.sgready_export_threshold
                to: "off"
            condition: []
            action:
              - if:
                  - condition: state
                    entity_id: binary_sensor.sgready_export_threshold
                    state: "on"
                then:
                  - type: turn_on
                    device_id: 6bacddb2c5431e33a3483482d77d219d
                    entity_id: c0a130c68e09da4b83bdb32670d9c52d
                    domain: switch
                else:
                  - type: turn_off
                    device_id: 6bacddb2c5431e33a3483482d77d219d
                    entity_id: c0a130c68e09da4b83bdb32670d9c52d
                    domain: switch
            mode: single

If my solar panel output rises to where I am exporting more than 1.5kW to the grid and
I continue exporting that for more than 2 consecutive minutes then I request the heat
pump to go into Excess mode. If the heat pump was transitioned into its current mode
within the past 10 minutes then my request for Excess mode will not happen immediately
but will instead be pending. If during this time my solar panels drop back to exporting
less than 1.5kW for any amount of time, the heat pump will be requested to switch to
Normal mode and this will be the pending state.

After having been in the current state for at least 10 minutes, if a new state is pending
then the heat pump transitions to that state and resets the state timer to 0. If the
pending state is the same as the current state, however, the state timer is not reset and
it continues counting upwards. (This has the side effect of showing on the display the
length of time the heat pump has been able to transition to another state). During this
time any request to change state will take effect immediately since the pump has been in
its current state for more than 10 minutes.
