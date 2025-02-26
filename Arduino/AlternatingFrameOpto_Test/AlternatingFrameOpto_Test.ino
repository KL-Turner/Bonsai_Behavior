#include <IntervalTimer.h>
IntervalTimer samplingTimer;
IntervalTimer frameGenTimer; // Simulated waveform timer

// Define pins
const int optoPin = 3;         // Output optogenetic pulses
const int framePeriodPin = 9;   // Read frame period waveform
const int frameSimPin = 10;     // Internal frame waveform generator for training
const int startPin = 27;       // Start stimulation sequence

// Adjustable parameters
const int durationSec = 2;           // Duration (in seconds) for the stimulation waveform
float frameBufferFraction = 0.10;       // Buffer as a fraction of the frame period (10%)
const bool useInternalFrameGen = true; // Set to true for training, false for 2P

// Frame simulation variables
const int frameHighTime = 5000; // (e.g., 25,000 µs high pulse)
const int frameLowTime  = 45000; // (e.g., 25,000 µs low pulse)
volatile bool frameState = false;

// Variables for timing
volatile unsigned long frameCounter = 0;  // Count of frames
volatile unsigned long frameHighDuration = 0;
volatile unsigned long frameLowDuration = 0;
volatile unsigned long lastFrameRiseTime = 0;
unsigned long framePeriod = 0;
unsigned long lastOptoFrameExecuted = 0;
volatile bool measuringHigh = true;
volatile bool newFrameDataReady = false; // Flag to signal new frame timing data

// Variables for pulse generation
unsigned long pulseWidth = 0;
unsigned long bufferWidth = 0;
volatile bool stimulationActive = false;
volatile bool stimulationPending = false;

// Debounce timing for button press
const unsigned long debounceInterval = 1000;  // 1 second debounce interval
unsigned long lastPressTime = 0;  // Store the time of the last valid press

// Serial output timing
unsigned long startTime;
unsigned long lastPrintTime = 0; // Last time the data was printed
unsigned long stimulationStartTime = 0; // Time when stimulation starts
const unsigned long printInterval = 5000; // Print every 5 seconds

// Setup function
void setup() {
  pinMode(startPin, INPUT);
  pinMode(framePeriodPin, INPUT);
  pinMode(optoPin, OUTPUT);
  Serial.begin(115200);

  if (useInternalFrameGen) {
    pinMode(frameSimPin, OUTPUT);
    frameGenTimer.begin(generateFrameWaveform, frameHighTime); // Start with the high-phase
  }

  attachInterrupt(digitalPinToInterrupt(framePeriodPin), measureFramePeriod, CHANGE);
  startTime = millis();
}

// Main loop
void loop() {
  // Button press handling for waveform start with debounce
  if (digitalRead(startPin) == HIGH && (millis() - lastPressTime >= debounceInterval)) {
    lastPressTime = millis(); // Update the last press time
    if (!stimulationActive && !stimulationPending) {
      stimulationPending = true; // Wait for the next odd frame
    }
  }
  // Periodically log timing and averages
  if (millis() - startTime >= printInterval && (millis() - lastPrintTime >= printInterval)) {
    lastPrintTime = millis();
    unsigned long elapsedTime = (millis() - startTime) / 1000;
    Serial.print("Elapsed Time (s): "); Serial.println(elapsedTime);
    Serial.print("Frame Period (ms): "); Serial.println(framePeriod / 1000.0);
    Serial.print("Pulse Width (ms): "); Serial.println(pulseWidth / 1000.0);
    Serial.print("Buffer Width (ms): "); Serial.println(bufferWidth / 1000.0);
    Serial.println();
  }

  // Process new frame timing data (set in the ISR)
  if (newFrameDataReady) {
    newFrameDataReady = false; // Reset flag
    
    // If stimulation is pending and we're on an odd-numbered frame, start stimulation.
    if (stimulationPending && (frameCounter % 2 != 0)) {
      stimulationPending = false;
      stimulationActive = true;
      stimulationStartTime = millis();
      lastOptoFrameExecuted = frameCounter;  // Record current frame as starting point
      generateOptoPulse();
    }
    // If stimulation is already active, only generate a pulse if exactly two frames have passed
    else if (stimulationActive && (frameCounter % 2 != 0)) {
      if (frameCounter == lastOptoFrameExecuted + 2) {
        lastOptoFrameExecuted = frameCounter;
        generateOptoPulse();
      }
    }
  }

  // Deactivate stimulation after the specified duration
  if (stimulationActive && (millis() - stimulationStartTime) >= (durationSec * 1000)) {
    stimulationActive = false;
    stimulationStartTime = 0;
    digitalWrite(optoPin, LOW); // Ensure optoPin is turned off
  }
}

// Function to generate the internal frame waveform (if using internal generation)
void generateFrameWaveform() {
  frameState = !frameState;
  digitalWrite(frameSimPin, frameState);
  if (frameState) {
    frameGenTimer.update(frameHighTime);
  } else {
    frameGenTimer.update(frameLowTime);
  }
}

// Function to generate the opto pulse during the frame
void generateOptoPulse() {
  unsigned long blockStartTime = micros();
  digitalWrite(optoPin, LOW);
  delayMicroseconds(bufferWidth); // Leading buffer period
  
  // Generate pulse within the pulse window
  while (micros() - blockStartTime < pulseWidth) {
    digitalWrite(optoPin, HIGH);
    delayMicroseconds(5000);  // 5 µs on pulse (adjust as needed)
    digitalWrite(optoPin, LOW);
    delayMicroseconds(45000); // 45 µs off period (adjust as needed)
  }
  digitalWrite(optoPin, LOW);
}

// Measure the frame period and generate opto pulses
void measureFramePeriod() {
  unsigned long currentTime = micros();
  if (digitalRead(framePeriodPin) == HIGH) {
    frameCounter++; // Increment frame count
    if (!measuringHigh) {
      frameLowDuration = currentTime - lastFrameRiseTime;
      measuringHigh = true;
    }
    lastFrameRiseTime = currentTime;
  } else {
    if (measuringHigh) {
      frameHighDuration = currentTime - lastFrameRiseTime;
      measuringHigh = false;
      // Calculate frame period
      framePeriod = frameHighDuration + frameLowDuration;
      pulseWidth = framePeriod * (1.0 - frameBufferFraction);
      bufferWidth = (framePeriod - pulseWidth) / 2;
      // Set flag to process these new timing values in the loop
      newFrameDataReady = true;

      // Start stimulation on the next odd frame if pending
      if (stimulationPending && frameCounter % 2 != 0) {
        stimulationPending = false;
        stimulationActive = true;
        stimulationStartTime = millis();
      }
      // Generate opto pulse during alternating frames if stimulation is active
      if (stimulationActive && frameCounter % 2 != 0) {
        unsigned long blockStartTime = micros();
        digitalWrite(optoPin, LOW); 
        delayMicroseconds(bufferWidth);
        
        while (micros() - blockStartTime < pulseWidth) {
          unsigned long timeElapsed = micros() - blockStartTime;
          unsigned long remainingTime = pulseWidth - timeElapsed;

          if (remainingTime < 5000) { // If less than one full ON pulse time remains, exit early
            break;
          }

          digitalWrite(optoPin, HIGH);
          delayMicroseconds(5000);  // 5 ms ON pulse

          remainingTime = pulseWidth - (micros() - blockStartTime);
          if (remainingTime < 45000) { // If there's no room for a full OFF period, exit
            break;
          }

          digitalWrite(optoPin, LOW);
          delayMicroseconds(45000);  // 45 ms OFF pulse
        }

        digitalWrite(optoPin, LOW); 
      }
    }
    lastFrameRiseTime = currentTime;
  }
}
