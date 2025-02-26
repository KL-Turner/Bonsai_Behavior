//#include <Adafruit_MCP4728.h>
#include <Adafruit_MCP4725.h>
#include <Wire.h>
#include "WaveTable.h"
#include "TeensyTimerTool.h"

using namespace TeensyTimerTool;
 
//Adafruit_MCP4728 mcp; address 0x64
Adafruit_MCP4725 mcp;

const int nreps = 4; // how many pulses to send, e.g., 4 = 4 assym cosines 

const int trigPin = 32; // digital channel to trigger the wave with a ttl
const float trigSec = 0.05; // minimum length of trigger in seconds, too long and it will send multiple signals
const int Fs = 5000; // sampling rate
const float dutyCycle = ceil((1.0/Fs) *  1000000); //wants microseconds 

int trigLen = int(ceil(Fs*trigSec));

uint16_t delayCount = 0;
int wavIncrmntr = 0;
int rep_cnt = 0;
int deBounce = 0;

PeriodicTimer t1;

void setup() {

  pinMode(trigPin, INPUT); 
  
  Serial.begin(115200);
  
  while (!Serial){
    delay(10); 
  }

  Wire.begin();
  
  // Try to initialize 
  if (!mcp.begin(0x63)) {
    Serial.println("Failed to find MCP4725 chip");
    while (1) {
      delay(10);
    } 
  }else{
    Serial.println("Found MCP4725 chip");
      mcp.setVoltage(0, false);
   // mcp.setChannelValue(MCP4728_CHANNEL_A, 0);
    //mcp.setChannelValue(MCP4728_CHANNEL_B, 0);
    //mcp.setChannelValue(MCP4728_CHANNEL_C, 0);
    //mcp.setChannelValue(MCP4728_CHANNEL_D, 0);
  }

  Wire.setClock(1000000);

  t1.begin(waveRun, dutyCycle);
  
}

// functions called by flexitimer should be short, run as quickly 1as
// possible, and should avoid calling other functions if possible.
void waveRun(){

  // triger a stimulus with a ttl, debounce for length trigPoints
  if (digitalRead(trigPin)==HIGH){
    deBounce++;
  }

  if (deBounce>trigLen){

    mcp.setVoltage(waveformsTable[wavIncrmntr],false);
    //mcp.setChannelValue(MCP4728_CHANNEL_A, waveformsTable[wavIncrmntr]);
    Serial.println(waveformsTable[wavIncrmntr]);
    wavIncrmntr++;

    if (wavIncrmntr>=maxSamplesNum){
      if (rep_cnt<nreps-1){
        rep_cnt++;
        wavIncrmntr = 0;
      } else {
        rep_cnt = 0;
        wavIncrmntr = 0;
        deBounce = 0;
      }
    }
    
  }
  
}

void loop() {

}
