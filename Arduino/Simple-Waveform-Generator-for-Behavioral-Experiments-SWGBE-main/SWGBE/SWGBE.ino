#include "WaveTable.h"
#include <Adafruit_MCP4728.h>
#include <Wire.h>
#include <Bounce.h>

const int Fs = 5000; // sampling rate
const int waveMax = 4095; // it's a 12bit dac, so this will always be the max voltage out
const int trigPin = 27; // digital channel to trigger the wave with a ttl

// waveform parameters to be set over serial for each of the 4 DAC channels
volatile int waveType[4] = {1,1,1,1}; // wave types: 0 = whale, 1 = square
volatile int waveDur[4] = {0,0,0,0}; // duration of pulse, fixed for whale right now, set via serial in ms, converted to sample points
volatile int waveAmp[4] = {0,0,0,0}; // max voltage amplitude, in 12bit - 0-4095 
volatile int waveIPI[4] = {0,0,0,0}; // duration between pulses, set via serial in ms, converted to sample points
volatile int waveReps[4] = {0,0,0,0}; // number of times to repeat wave pulse and interpulse interval
volatile int rampStep[4] = {0,0,0,0}; // specific to ramping variables, still in development
volatile int whaleStep[4] = {0,0,0,0}; // specific to ramping variables, still in development
volatile int waveBase[4] = {0,0,0,0}; // baseline prior to stimulus onset, in ms, allows for offsets between the stimuli, set via serial in ms, converted to sample points

volatile int wavIncrmntr[4] = {0,0,0,0}; // for keeping track of where we are in a given stimulus presentation
volatile int repCntr[4] = {0,0,0,0}; // for keeping track of pulse repitions
volatile int ipiCntr[4] = {0,0,0,0}; // for keeping track of where we are in an interpulse interval
volatile int BaseCntr[4] = {0,0,0,0}; // for keeping track of where we are in a baseline period
volatile int whaleCntr[4] = {0,0,0,0}; //

volatile int chanSelect = 0; // 
volatile int waveParams[7] = {0,0,0,0,0,0,0};
volatile int curVal[4] = {0,0,0,0};

volatile bool inIpi[4] = {false,false,false,false};
volatile bool inBase[4] = {false,false,false,false};
volatile bool stimOn[4] = {false,false,false,false};
volatile bool fire = false;

const byte numChars = 255;
char receivedChars[numChars];
const int nVarIn = 7;
bool newData = false;

Adafruit_MCP4728 mcp;
IntervalTimer t1;
Bounce trigger = Bounce(trigPin,50); 

void setup() {

  Serial.begin(9600); // baud rate here doesn't matter for teensy
  Serial.println("Connected");

  Wire.begin();
  
  // Try to initialize
  if (!mcp.begin(0x64)) {
    Serial.println("Failed to find MCP4728 chip"); 
  } else{
    Serial.println("Found MCP4728 chip");
    mcp.setChannelValue(MCP4728_CHANNEL_A, 0);
    mcp.setChannelValue(MCP4728_CHANNEL_B, 0);
    mcp.setChannelValue(MCP4728_CHANNEL_C, 0);
    mcp.setChannelValue(MCP4728_CHANNEL_D, 0);
  }
  
  Wire.setClock(1000000); // holy fucking shit, this must be set after mcp.begin or else it doesn't work
  
  t1.begin(waveRun, 1E6/Fs);

}

// functions called by timer should be short, run as quickly as
// possible, and should avoid calling other functions if possible.

void waveRun(){

  recvWithStartEndMarkers();
  parseData();
  
  pollTrigger(); // check for triggers
  waveWrite(); // set values

  mcp.setChannelValue(MCP4728_CHANNEL_A, curVal[0]); // send the value to the dac
  mcp.setChannelValue(MCP4728_CHANNEL_B, curVal[1]); // send the value to the dac
  mcp.setChannelValue(MCP4728_CHANNEL_C, curVal[2]); // send the value to the dac
  mcp.setChannelValue(MCP4728_CHANNEL_D, curVal[3]); // send the value to the dac

}

void pollTrigger(){
  // check for a trigger
  if (trigger.update()){
    if (trigger.risingEdge()){
      fire = true;
      for (int i = 0; i< 4; i++){
        stimOn[i] = true;
        inBase[i] = true;
      }
    }
  }
}

void waveWrite(){
  if (fire){ // if ttl received
    for (int i = 0; i < 4; i++){ // for each dac channel
      if (stimOn[i]) {   // if there's a stim on
        if (inBase[i]){ // is it in baseline?
          curVal[i] = 0; // then output stays at 0
          BaseCntr[i]++; // increment the baseline counter
          if (BaseCntr[i]>=waveBase[i]){ // check if we're at the end of the baseline
            inBase[i] = false; // if we've gone past the baseline period, then end it
            BaseCntr[i] = 0; // and reset counter
          }
        } else if (inIpi[i]){ // check if in an inter-pulse interval         
          curVal[i] = 0; // if yes, output is 0
          ipiCntr[i]++; // increment
          if (ipiCntr[i]>=waveIPI[i]){ // check if we're at the end of the ipi
            inIpi[i] = false; // we're out of ipi period
            ipiCntr[i] = 0; // reset counter
          }
        } else { // if not in baseline or in an ipi, then we are presenting the waveform   
          if (waveType[i] == 0){ // whale stim
            if (wavIncrmntr[i] % whaleStep[i] == 0){
              curVal[i] = map(asymCos[whaleCntr[i]], 0, waveMax, 0, waveAmp[i]);
              whaleCntr[i]++;
            }   
          } else if (waveType[i] == 1){ // square wave
            curVal[i] = waveAmp[i];  
          } else if (waveType[i] == 2){ // ramp up
            curVal[i] = linspace((float) waveDur[i], 0, (float) waveAmp[i] , wavIncrmntr[i]);
          } else if (waveType[i] == 3){ // ramp down
            curVal[i] = linspace((float) waveDur[i], (float) waveAmp[i], 0 , wavIncrmntr[i]);  
          } else if (waveType[i] == 4){ // pyramid
            if(wavIncrmntr[i]<waveDur[i]/2){
              curVal[i] = linspace((float) waveDur[i]/2,0, (float) waveAmp[i] , wavIncrmntr[i]); 
            } else {
              curVal[i] = linspace((float) waveDur[i]/2, (float) waveAmp[i], 0 , wavIncrmntr[i]-waveDur[i]/2); 
            }
          }
          wavIncrmntr[i] = wavIncrmntr[i] + 1;
        }
        if (wavIncrmntr[i] >= waveDur[i]){ // if it's the end of one wave
          if (repCntr[i] < waveReps[i]-1){ // but if it's not the end of the number of wave repititions
            repCntr[i] = repCntr[i] + 1; // increment rep counter
            whaleCntr[i] = 0;
            wavIncrmntr[i] = 0; // reset wave indexer
            inIpi[i] = true; // go into ipi
            curVal[i] = 0;
          } else { // else that's the end of the requested signal, so reset stuff
            repCntr[i] = 0;
            whaleCntr[i] = 0;
            wavIncrmntr[i] = 0;
            inIpi[i] = false; // go into ipi
            stimOn[i] = false;
            curVal[i] = 0;
          }
        }
      }      
    }
  }
  // check if all stimuli are done
  for (int i = 0; i < 4; i++){
    if (stimOn[i]){
      break;
    }
    fire = false; 
  }
}

void recvWithStartEndMarkers() {
    static boolean recvInProgress = false;
    static byte ndx = 0;
    char startMarker = '<';
    char endMarker = '>';
    char rc;
    while (Serial.available() > 0 && newData == false) {
        rc = Serial.read();

        if (recvInProgress == true) {
            if (rc != endMarker) {
                receivedChars[ndx] = rc;
                ndx++;
                if (ndx >= numChars) {
                    ndx = numChars - 1;
                }
            }
            else {
                receivedChars[ndx] = '\0'; // terminate the string
                recvInProgress = false;
                ndx = 0;
                newData = true;
            }
        }

        else if (rc == startMarker) {
            recvInProgress = true;
        }
    }
}


void parseData() {      // split the data into its parts

  if (newData == true) {

    int cntr = 0;
    char * ptr;

    ptr = strtok(receivedChars,",");
    waveParams[cntr] = atoi(ptr);
    cntr++;

    ptr = strtok(NULL,",");

    while ((ptr != NULL) & (cntr < 7)){
      waveParams[cntr] = atoi(ptr);
      cntr++;
      ptr = strtok(NULL,",");
    }

    // select channel
    chanSelect = waveParams[0];

    if (chanSelect>3){
      Serial.println("Bad channel selection, setting channel 0 to the requested values");
      chanSelect = 0;
    }

    // set wave type
    waveType[chanSelect] = waveParams[1];
    // set duration
    waveDur[chanSelect] = (int) round((waveParams[2]/1000.0) * Fs);

    if ((waveType[chanSelect] == 0) & (waveDur[chanSelect] < SamplesNum)){
      Serial.println("The requested duration is too short for the whalestim, setting to minimum of 10 ms");
      waveDur[chanSelect] = SamplesNum;
    }

    // set amplitude
    waveAmp[chanSelect] = waveParams[3];
    // set interpulse interval
    waveIPI[chanSelect] = (int) round((waveParams[4]/1000.0) * Fs);
    // set number of pulses
    waveReps[chanSelect] = waveParams[5]; 
    // get baseline length
    waveBase[chanSelect] = (int) round((waveParams[6]/1000.0) * Fs);

    // this is always computed, but only used if ramping up or down
    rampStep[chanSelect] = (int) ceil((float) waveAmp[chanSelect]/waveDur[chanSelect]);

    // this is always computed, but only used if using whale stim
    whaleStep[chanSelect] = (int) ceil((float) waveDur[chanSelect]/SamplesNum); // how quickly to step through the asymCosine

    Serial.println("---------");
    for (int i = 0; i < 4; i++){
      Serial.print("Channel ");
      Serial.print(i);
      Serial.print(": Wave Type ");
      Serial.print(waveType[i]);
      Serial.print(", ");
      Serial.print(" Wave Duration = ");
      Serial.print(waveDur[i] * 1000.0/Fs);
      Serial.print(" ms, ");
      Serial.print(" wave Ampplitude (12bit) = ");
      Serial.print(waveAmp[i]);
      Serial.print(", ");
      Serial.print(" waveIPI = ");
      Serial.print(waveIPI[i] * 1000.0/Fs);
      Serial.print(" ms, ");
      Serial.print(" waveReps = "); 
      Serial.print(waveReps[i]);
      Serial.print(", ");
      Serial.print(" wave Baseline = ");
      Serial.print(waveBase[i] * 1000.0/Fs);
      Serial.print(" ms");
      Serial.println("");
    }
    Serial.println("---------");

  newData = false;

  }
}

int linspace(float const n, float const d1, float const d2, int const i){
  float n1 = n-1;
  return round(d1 + (i)*(d2 - d1)/n1);
}
 
void loop(){
}




