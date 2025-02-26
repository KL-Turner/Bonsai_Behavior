# Simple Waveform Generator for Behavioral Experiments (SWGBE)
## Currently in use with a [Teensy 4.0](https://www.pjrc.com/store/teensy40.html) (or 4.1) and an [Adafruit MCP4728 Quad DAC](https://www.adafruit.com/product/4470)
It uses [IntervalTimer](https://www.pjrc.com/teensy/td_timing_IntervalTimer.html) library to run at 5 kHz, which is just fine for the Moore Lab's tactile detection experiments.

## Changing waveform parameters over serial
The program is looking for serial messages of the following form: <1,2,3,4,5,6,7>\
Where,\
1 = DAC channel 0-3\
2 = waveform type (0 = asymm cos. 1 = square, 2 = ramp up, 3 = ramp down, 4 = ramp-up then ramp-down\
3 = wave duration in milliseconds (i.e., the length of one pulse of the waveform)\
4 = wave max amplitude (12 bit number, 0-4095)\
5 = duration between waves (inter-pulse-interval) un milliseconds\
6 = number of wave repetitions/pulses\
7 = baseline length in milliseconds (i.e., the time after a waveform is triggered but before the start of the actual waveform. This allows you to offset the timing between the different DAC channels relative to eachother.\
\
Example:\
<3,2,20,2047,25,5,100>\
This will (1) output the wave on channel 3, (2) it will be a ramping-up stimili, (3) it will take 20 ms to go from min-max, (4) it will have an amplitude of about half the maximum voltage output capable of your setup (e.g., without any amplification the max out of the DAC will be 5V. (5) there will be 25ms between repeats of the waveform, (6) the waveform will be repeated 5 times, and (7) there will be a delay of 100ms between when the output is triggered and when it actually begins.\
\
The above example would look like this:

![Alt text](https://github.com/JeremyWMurphy/Simple-Waveform-Generator-for-Behavioral-Experiments-SWGBE/blob/main/exampleWaveform.png)


