#include <IntervalTimer.h>
IntervalTimer samplingTimer;

// Define pins
const int startPin = 36; // Start stimulation sequence
const int framePeriodPin = 35; // Read frame period waveform
const int optoPin = 34; // Output optogenetic pulses

// Adjustable parameters
const int durationSec = 2; // Duration in seconds for the stimulation waveform
float frameBufferFraction = 0.1; // Buffer as a fraction of the frame period (10%)

// Variables for timing
volatile unsigned long frameCounter = 0;  // Count of frames
volatile unsigned long frameHighDuration = 0;
volatile unsigned long frameLowDuration = 0;
volatile unsigned long lastFrameRiseTime = 0;
volatile bool measuringHigh = true;
unsigned long framePeriod = 0;

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
      Serial.println("Button pressed. Waiting for next odd frame...");
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
  // Deactivate stimulation after the specified duration
  if (stimulationActive && (millis() - stimulationStartTime) >= (durationSec * 1000)) {
    stimulationActive = false;
    stimulationStartTime = 0;
    digitalWrite(optoPin, LOW); // Ensure optoPin is turned off
  }
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
      // Start stimulation on the next odd frame if pending
      if (stimulationPending && frameCounter % 2 != 0) {
        stimulationPending = false;
        stimulationActive = true;
        stimulationStartTime = millis();
      }
      // Generate opto pulse during alternating frames if stimulation is active
      if (stimulationActive && frameCounter % 2 != 0) {
        // Start LOW for leading buffer period
        digitalWrite(optoPin, LOW); 
        delayMicroseconds(bufferWidth);
        // Set HIGH for pulse width
        digitalWrite(optoPin, HIGH);
        delayMicroseconds(pulseWidth);
        // Set LOW for lagging buffer period
        digitalWrite(optoPin, LOW); 
      }
    }
    lastFrameRiseTime = currentTime;
  }
}
