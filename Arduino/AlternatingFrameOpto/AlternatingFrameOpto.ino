#include <IntervalTimer.h>
IntervalTimer samplingTimer;
IntervalTimer frameGenTimer; // Simulated waveform timer

// Define pins
const int optoPin = 36;         // Output optogenetic pulses
const int framePeriodPin = 31;   // Read frame period waveform
const int frameSimPin = 32;     // Internal frame waveform generator for training
const int startPin = 35;       // Start stimulation sequence
const int shutterPin = 37;     // PMT shutter TTL (Bruker "Uncaging" BNC). Empirical: HIGH = CLOSED, LOW = OPEN.

// Adjustable parameters
const int durationSec = 2;           // Duration (in seconds) for the stimulation waveform
float frameBufferFraction = 0.10;       // Buffer as a fraction of the frame period (10%)
const bool useInternalFrameGen = false; // Set to true for training, false for 2P

// Stimulus mode:
//   true  (current): the frame-locked ALTERNATING pattern runs unchanged, wrapped by the PMT
//         shutter via a non-blocking sequencer (the ShutterPhase state machine in loop()). A
//         startPin pulse closes the shutter (pin 37 HIGH), waits shutterLeadMs, hands off to the
//         existing alternating machinery for durationSec, then reopens the shutter shutterLagMs
//         after the stim ends. The shutter stays CLOSED across the whole train -- it is NOT
//         reopened between frames; the alternating LED gating is preserved only for testing.
//   false: bypass the alternating scheme -- a startPin pulse runs the blocking generateShutteredStim()
//         (shutter closed, a simple free-running 5/45 ms LED train, shutter open). Handy for
//         bench-testing the shutter + LED wiring with no 2P / no frame input on framePeriodPin.
const bool useAlternatingFrames = true;

// PMT shutter pads (both modes): shutter closes shutterLeadMs before the stim starts and opens
// shutterLagMs after it ends. Nominal shutter-closed window = durationSec*1000 + shutterLeadMs +
// shutterLagMs. In alternating mode the frame-lock adds a small extra lead (0-2 frame periods) --
// always the safe direction (shutter closed slightly LONGER, never shorter).
const unsigned long shutterLeadMs = 100; // shutter closes this long BEFORE the stim starts
const unsigned long shutterLagMs  = 100; // shutter opens this long AFTER the stim ends

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

// Shutter sequencer (alternating mode): a non-blocking state machine in loop() that keeps the PMT
// shutter closed across the whole frame-locked stim. Non-blocking (no delay()) so the alternating
// pattern's frame processing keeps running. Only touched in loop(), so no volatile is needed.
enum ShutterPhase { SHUTTER_IDLE, SHUTTER_LEAD, SHUTTER_STIM, SHUTTER_LAG };
ShutterPhase shutterPhase = SHUTTER_IDLE;
unsigned long shutterPhaseStart = 0;             // millis() when the current shutter phase began
// Safety watchdog: if the frame-locked stim never starts (e.g. the 2P scan is not running), reopen the
// shutter instead of holding it closed forever. The stim starts on the next ODD frame (<= ~2 frame
// periods after arming), so the timeout is scaled to the measured frame period with a floor -- a fixed
// value would falsely abort a valid trial on slow scans (frame period >~ floor/2).
const unsigned long stimStartWatchdogFloorMs = 3000;            // minimum wait before giving up (covers "no frames")
unsigned long stimStartWatchdogRunMs = stimStartWatchdogFloorMs; // per-sequence value (max of floor, 4x frame period)
bool prevStartPin = false;                       // previous startPin level, for rising-edge ("go" pulse) detection

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
  // PMT shutter: drive LOW (= OPEN = normal imaging) as the very first action so pin 37
  // spends the least possible time floating during boot. Must precede Serial.begin and
  // all other init. (External pull-downs on pin 37 AND on optoPin/pin 36 are still recommended to
  // define both lines during the reset window before setup() runs -- the MCU holds all pins high-Z
  // until then, so hardware pull-downs are what guarantee shutter-open + LED-off at power-on.)
  pinMode(shutterPin, OUTPUT);
  digitalWrite(shutterPin, LOW);

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
  // Start on a RISING edge of startPin (a brief "go" pulse), debounced. Edge-triggering (not level)
  // guarantees exactly one sequence per pulse, so a held/long trigger line cannot auto-repeat.
  bool startPinHigh = (digitalRead(startPin) == HIGH);
  if (startPinHigh && !prevStartPin && (millis() - lastPressTime >= debounceInterval)) {
    lastPressTime = millis(); // Update the last press time
    if (useAlternatingFrames) {
      // Alternating mode: arm the shutter sequencer. Close the shutter now; after the lead pad the
      // sequencer (end of loop) hands off to the existing frame-locked machinery. Ignore the trigger
      // if a sequence is already in flight, so a stray re-trigger can't reopen the shutter mid-stim.
      if (shutterPhase == SHUTTER_IDLE && !stimulationActive && !stimulationPending) {
        digitalWrite(shutterPin, HIGH); // CLOSE shutter at the trigger edge
        shutterPhase = SHUTTER_LEAD;
        shutterPhaseStart = millis();
        Serial.println("Shutter CLOSED (pin 37 HIGH); alternating stim starts after lead pad.");
      }
    } else {
      // Bypass mode: shutter-protected, non-alternating stimulus (blocking).
      generateShutteredStim();
    }
  }
  prevStartPin = startPinHigh; // sample for next iteration's edge detection
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

  // Shutter sequencer (alternating mode): non-blocking state machine that holds the PMT shutter
  // closed across the whole frame-locked stim. Idle in bypass mode (shutterPhase stays SHUTTER_IDLE).
  switch (shutterPhase) {
    case SHUTTER_LEAD:
      // Hold the shutter closed for the lead pad, then start the alternating stim.
      if (millis() - shutterPhaseStart >= shutterLeadMs) {
        stimulationPending = true;        // existing machinery starts on the next odd frame
        shutterPhase = SHUTTER_STIM;
        shutterPhaseStart = millis();
        // Scale the "stim never started" watchdog to the measured frame period (the stim starts on
        // the next odd frame, <= ~2 periods away). Floor covers the no-frames case (framePeriod ~ 0).
        unsigned long framePeriodMs = framePeriod / 1000UL;
        stimStartWatchdogRunMs = stimStartWatchdogFloorMs;
        if (4UL * framePeriodMs > stimStartWatchdogRunMs) stimStartWatchdogRunMs = 4UL * framePeriodMs;
      }
      break;
    case SHUTTER_STIM:
      // Stim fully complete (both flags clear again) -> begin the lag pad.
      if (!stimulationPending && !stimulationActive) {
        shutterPhase = SHUTTER_LAG;
        shutterPhaseStart = millis();
      }
      // Safety watchdog: if no frame ever started the stim, do not hold the shutter closed forever.
      // Cancel the pending start atomically w.r.t. the frame ISR so we can't reopen just as a stim
      // begins (which would expose the PMTs).
      else if (stimulationPending && (millis() - shutterPhaseStart >= stimStartWatchdogRunMs)) {
        noInterrupts();
        bool startedInTime = stimulationActive; // did a frame start the stim while we waited?
        if (!startedInTime) stimulationPending = false;
        interrupts();
        if (!startedInTime) {
          digitalWrite(shutterPin, LOW);
          shutterPhase = SHUTTER_IDLE;
          Serial.println("Shutter sequencer: no frames started the stim -- shutter reopened (check 2P scan).");
        }
        // else: a frame just started the stim; stay in SHUTTER_STIM and let the normal path close it out.
      }
      break;
    case SHUTTER_LAG:
      // Hold the shutter closed for the lag pad, then reopen.
      if (millis() - shutterPhaseStart >= shutterLagMs) {
        digitalWrite(shutterPin, LOW);    // OPEN shutter -- normal imaging
        shutterPhase = SHUTTER_IDLE;
        Serial.println("Shutter OPEN (pin 37 LOW); sequence complete.");
      }
      break;
    case SHUTTER_IDLE:
    default:
      break;
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

// Bypass mode (useAlternatingFrames == false): deliver one opto stimulus with the PMT shutter
// closed around it, with the frame-alternating scheme intentionally bypassed. The shutter
// physically protects the PMTs for the entire stimulus, so frame interleaving is unnecessary.
// This is a single BLOCKING stimulus -- one shutter window wraps one stimulus.
//
// Because this path never sets stimulationActive/stimulationPending, the alternating machinery
// in loop() and in the measureFramePeriod() ISR stays dormant and does not touch optoPin.
//
// Timing (referenced to this Teensy's own clock, so the 100 ms lead is exact):
//   shutterPin HIGH (CLOSED) --100 ms--> LED train (durationSec) --100 ms--> shutterPin LOW (OPEN)
//   HIGH (closed) window = durationSec*1000 + shutterLeadMs + shutterLagMs (= durationSec*1000 + 200 ms).
//   The stimulus is therefore delayed shutterLeadMs (100 ms) after the shutter-close trigger.
//
// LED waveform: free-running 5 ms ON / 45 ms OFF (~20 Hz, 10% duty) -- option (a): preserves the
// actual opto waveform (the alternating gating was only about imaging-frame interleave). To deliver
// a single continuous pulse instead (option (b)), replace the for-loop below with:
//     digitalWrite(optoPin, HIGH); delay(durationSec * 1000UL); digitalWrite(optoPin, LOW);
void generateShutteredStim() {
  const unsigned long pulseOnUs  = 5000;   // 5 ms ON
  const unsigned long pulseOffUs = 45000;  // 45 ms OFF
  const unsigned long pulsePeriodMs = (pulseOnUs + pulseOffUs) / 1000; // 50 ms per cycle
  const unsigned long stimDurationMs = (unsigned long)durationSec * 1000UL;
  // numPulses floors, so the train covers the full stimDurationMs only when durationSec*1000
  // divides evenly into the 50 ms cycle. Guard at compile time so a future durationSec change
  // can't silently shorten the train while the shutter window still spans durationSec*1000+200 ms.
  static_assert((durationSec * 1000) % 50 == 0, "durationSec*1000 must be a multiple of the 50 ms pulse period");
  const unsigned long numPulses = stimDurationMs / pulsePeriodMs;       // e.g. 2000 / 50 = 40 pulses

  Serial.println("Shuttered stim: shutter CLOSED (pin 37 HIGH), 100 ms lead, LED train, 100 ms lag.");

  digitalWrite(shutterPin, HIGH); // CLOSE shutter -- protect PMTs
  delay(shutterLeadMs);           // lead before LED onset (nothing else between: keeps lead exact)

  for (unsigned long i = 0; i < numPulses; i++) {
    digitalWrite(optoPin, HIGH);
    delayMicroseconds(pulseOnUs);
    digitalWrite(optoPin, LOW);
    delayMicroseconds(pulseOffUs);
  }
  digitalWrite(optoPin, LOW);     // ensure LED off

  delay(shutterLagMs);            // lag after LED offset
  digitalWrite(shutterPin, LOW);  // OPEN shutter -- normal imaging

  Serial.println("Shuttered stim: shutter OPEN (pin 37 LOW). Done.");
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
