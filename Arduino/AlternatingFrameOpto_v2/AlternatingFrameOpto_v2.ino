#include <IntervalTimer.h>

// Hardware timer for the internal (simulated) frame clock. The opto pulse train is generated
// non-blockingly from loop() (driveOpto), so nothing busy-waits inside an interrupt.
IntervalTimer frameGenTimer;  // internal frame clock (only when useInternalFrameGen)

// ===================================================================================================
//  ADJUSTABLE PARAMETERS  (modes, then opto, then shutter)
// ===================================================================================================

// ---- Stimulus MODE switches -----------------------------------------------------------------------
// Stimulus pattern:
//   useAlternatingFrames true  = frame-locked ALTERNATING pattern -- opto fires on odd frames only, off
//                               on even frames (needs a frame clock: real 2P or internal gen).
//                        false = BYPASS -- a startPin pulse runs a simple continuous carrier train for
//                               optoDurationSec (no frame clock needed).
const bool useAlternatingFrames = true;

// PMT shutter control:
//   useAlternatingShutter false = keep the shutter CLOSED for the whole stim; true = OPEN the shutter on
//                    non-stim frames to image between stim frames, re-closing before each stim frame.
//                    Auto-falls back to closed-whole-stim (Serial warning) when frames are too fast, i.e.
//                    requires framePeriod >= 2*shutterActuationMs + minImagingWindowMs.
const bool useAlternatingShutter = false;

// Frame source:
//   useInternalFrameGen false = use the external 2P frame sync on framePeriodPin (pin 31).
//                       true  = the Teensy generates the frame clock INTERNALLY in software at
//                               internalFrameRateHz (nothing wired to pin 31) -- for training mice on a
//                               Prairie-View-like flashing pattern with no microscope. Internal frames
//                               feed the exact same stim + shutter pipeline, so every mode above applies.
const bool useInternalFrameGen = false;
const float internalFrameRateHz = 5.0;       // internal frame-gen rate (Hz); Prairie-View-like training default
const unsigned long internalFramePeriodUs = (unsigned long)(1000000.0 / internalFrameRateHz + 0.5);

// ---- Opto parameters ------------------------------------------------------------------------------
// Opto "carrier" pulse train, specified as duration + frequency + pulse width (the standard optogenetic
// parameterization). Duty cycle = optoPulseWidthMs * optoFreqHz (10% at 20 Hz / 5 ms). Keeping the pulse
// WIDTH fixed when you change the frequency is the physiologically correct behavior -- short pulses evoke
// clean single spikes; long (e.g. 50% duty) pulses risk depolarization block / ChR2 desensitization.
const int optoDurationSec        = 2;      // stimulation duration (s)
constexpr float optoFreqHz       = 20.0f;  // opto carrier frequency (Hz)
constexpr float optoPulseWidthMs = 5.0f;   // opto pulse width (ms); must be shorter than one period
static_assert(optoPulseWidthMs * optoFreqHz < 1000.0f, "opto pulse width must be < one period (duty < 100%)");
constexpr unsigned long optoPeriodUs = (unsigned long)(1000000.0f / optoFreqHz + 0.5f);
constexpr unsigned long pulseOnUs    = (unsigned long)(optoPulseWidthMs * 1000.0f + 0.5f);
constexpr unsigned long pulseOffUs   = optoPeriodUs - pulseOnUs;

// ---- Shutter Adjustable parameters ----------------------------------------------------------------
// (Rarely changed.) PMT shutter timing.
const unsigned long shutterLeadMs = 100;        // PMT shutter closes this long BEFORE the stim (whole-stim wrap)
const unsigned long shutterLagMs  = 100;        // PMT shutter opens this long AFTER the stim (whole-stim wrap)
const unsigned long shutterActuationMs = 50;    // Measured PMT shutter open/close time (per-frame alternation)
const unsigned long shutterSettleMarginMs = 25; // Extra guard-band before arming opto in per-frame mode
const unsigned long minImagingWindowMs = 30;    // per-frame mode used only if the open window would exceed this
// Whole-stim opto arms after the lead pad, so the lead must cover the shutter's close time or opto could
// fire before the shutter has physically settled closed.
static_assert(shutterLeadMs >= shutterActuationMs, "shutterLeadMs must be >= shutterActuationMs");

// ===================================================================================================

// Define pins
const int framePeriodPin = 31;  // External 2P frame-sync input (used only when useInternalFrameGen == false)
const int startPin       = 35;  // Start-stimulation trigger (brief "go" pulse) in
const int optoPin        = 36;  // Optogenetic LED pulses out
const int shutterPin     = 37;  // PMT shutter TTL (Bruker "Uncaging" BNC). Empirical: HIGH = CLOSED, LOW = OPEN.

// ---- State ----------------------------------------------------------------------------------------
// Frame timing (shared with the frame ISR / internal timer)
volatile unsigned long frameCounter = 0;
volatile unsigned long framePeriod = 0;          // most recent frame period (us)
volatile unsigned long lastFrameRiseTime = 0;    // micros() of the last external frame edge
volatile unsigned long frameHighDuration = 0;
volatile unsigned long frameLowDuration = 0;
volatile bool measuringHigh = true;

// Stimulation state
volatile bool stimulationActive = false;
volatile bool stimulationPending = false;        // waiting for the next odd frame to start
volatile bool optoArmed = false;                 // opto may fire ONLY when true (shutter confirmed CLOSED)
bool optoState = false;                          // current opto LED level (owned by driveOpto in loop())
unsigned long optoEdgeUs = 0;                    // micros() of the last opto edge (drives the 5/45 timing)
volatile unsigned long stimulationStartTime = 0; // written in the frame ISR, read in loop()

// Shutter sequencer (alternating mode): non-blocking state machine in loop().
enum ShutterPhase { SHUTTER_IDLE, SHUTTER_LEAD, SHUTTER_STIM, SHUTTER_LAG };
ShutterPhase shutterPhase = SHUTTER_IDLE;
unsigned long shutterPhaseStart = 0;
// Watchdog: if the frame-locked stim never starts (no frame clock), reopen the shutter instead of
// holding it closed forever. Scaled to the measured frame period with a floor.
const unsigned long stimStartWatchdogFloorMs = 3000;
unsigned long stimStartWatchdogRunMs = stimStartWatchdogFloorMs;

// Per-frame shutter alternation state (loop() only)
bool alternatingShutterActive = false;           // runtime: useAlternatingShutter && frame rate feasible
unsigned long lastShutterFrame = 0;
unsigned long shutterFrameCloseAt = 0;           // millis() deadline to CLOSE before the next stim frame (0 = none)
unsigned long optoArmAt = 0;                     // millis() deadline to arm opto after a close settles (0 = none)

// Trigger debounce / rising-edge detect
const unsigned long debounceInterval = 1000;
unsigned long lastPressTime = 0;
bool prevStartPin = false;

// Serial logging
unsigned long startTime;
unsigned long lastPrintTime = 0;
const unsigned long printInterval = 5000;

// ---- Helpers --------------------------------------------------------------------------------------
// Drive the PMT shutter. HIGH = CLOSED (PMTs protected), LOW = OPEN. Every operational shutter write goes
// through here (the boot LOW in setup() stays a direct digitalWrite).
void setShutter(bool closed) {
  digitalWrite(shutterPin, closed ? HIGH : LOW);
}

// Force the opto LED off and disarm it. Used before opening the shutter and at stim end so a pulse can
// never straddle a shutter-open (which would expose the PMTs). Opto is driven only from loop(), so no
// interrupt masking is needed here -- the frame ISR never touches optoPin / optoArmed / optoState.
void forceOptoOff() {
  optoArmed = false;
  optoState = false;
  digitalWrite(optoPin, LOW);
  optoEdgeUs = micros();
}

// ---- Setup / loop ---------------------------------------------------------------------------------
void setup() {
  // PMT shutter: drive LOW (= OPEN = normal imaging) as the very first action so pin 37 spends the least
  // possible time floating during boot. (External pull-downs on pin 37 AND optoPin/pin 36 are still
  // recommended to define both lines during the reset window before setup() runs.)
  pinMode(shutterPin, OUTPUT);
  digitalWrite(shutterPin, LOW);

  pinMode(optoPin, OUTPUT);
  digitalWrite(optoPin, LOW);
  pinMode(startPin, INPUT);

  Serial.begin(115200);
  Serial.print("AlternatingFrameOpto v2 | useAlternatingFrames="); Serial.print(useAlternatingFrames);
  Serial.print(" useAlternatingShutter="); Serial.print(useAlternatingShutter);
  Serial.print(" useInternalFrameGen="); Serial.print(useInternalFrameGen);
  Serial.print(" internalFrameRateHz="); Serial.println(internalFrameRateHz);

  // Frame source: internal software clock, or the external 2P sync on pin 31.
  if (useInternalFrameGen) {
    frameGenTimer.begin(internalFrameTick, internalFramePeriodUs);
  } else {
    pinMode(framePeriodPin, INPUT);
    attachInterrupt(digitalPinToInterrupt(framePeriodPin), measureFramePeriod, CHANGE);
  }

  startTime = millis();
}

void loop() {
  // Start on a RISING edge of startPin (a brief "go" pulse), debounced -- exactly one sequence per pulse,
  // so a held/long trigger line cannot auto-repeat.
  bool startPinHigh = (digitalRead(startPin) == HIGH);
  if (startPinHigh && !prevStartPin && (millis() - lastPressTime >= debounceInterval)) {
    lastPressTime = millis();
    if (useAlternatingFrames) {
      // Arm the shutter sequencer (ignored if a sequence is already in flight, so a stray re-trigger
      // can't reopen the shutter mid-stim).
      if (shutterPhase == SHUTTER_IDLE && !stimulationActive && !stimulationPending) {
        setShutter(true);  // CLOSE shutter at the trigger edge
        shutterPhase = SHUTTER_LEAD;
        shutterPhaseStart = millis();
        Serial.println("Shutter CLOSED (pin 37 HIGH); alternating stim starts after lead pad.");
      }
    } else {
      generateShutteredStim();  // bypass: blocking continuous train
    }
  }
  prevStartPin = startPinHigh;

  // Periodic logging.
  if (millis() - startTime >= printInterval && (millis() - lastPrintTime >= printInterval)) {
    lastPrintTime = millis();
    Serial.print("Elapsed (s): "); Serial.print((millis() - startTime) / 1000);
    Serial.print("  Frame period (ms): "); Serial.print(framePeriod / 1000.0);
    Serial.print("  Frames: "); Serial.println(frameCounter);
  }

  // Deactivate stimulation after the specified duration (force opto off first).
  if (stimulationActive && (millis() - stimulationStartTime) >= (unsigned long)optoDurationSec * 1000UL) {
    forceOptoOff();
    stimulationActive = false;
    stimulationStartTime = 0;
  }

  // Shutter sequencer (alternating mode). Idle in bypass mode (shutterPhase stays SHUTTER_IDLE).
  switch (shutterPhase) {
    case SHUTTER_LEAD:
      // Hold the shutter closed for the lead pad, then start the alternating stim.
      if (millis() - shutterPhaseStart >= shutterLeadMs) {
        stimulationPending = true;   // frame machinery starts the stim on the next odd frame
        shutterPhase = SHUTTER_STIM;
        shutterPhaseStart = millis();
        optoArmed = true;            // shutter has settled CLOSED after the lead pad -> opto may fire
        // Scale the "stim never started" watchdog to the measured frame period; floor covers no-frames.
        unsigned long framePeriodMs = framePeriod / 1000UL;
        stimStartWatchdogRunMs = stimStartWatchdogFloorMs;
        if (4UL * framePeriodMs > stimStartWatchdogRunMs) stimStartWatchdogRunMs = 4UL * framePeriodMs;
        // Per-frame alternation only if an imaging window remains after the shutter closes/opens each frame.
        // framePeriodMs == 0 means no period has been measured yet -- distinct from "frames too fast".
        alternatingShutterActive = useAlternatingShutter && (framePeriodMs > 0) &&
            (framePeriodMs >= (2UL * shutterActuationMs + minImagingWindowMs));
        if (useAlternatingShutter && !alternatingShutterActive) {
          if (framePeriodMs == 0) {
            Serial.println("Alternate-shutter: no frame period measured yet -- shutter stays CLOSED for the whole stim.");
          } else {
            Serial.println("Alternate-shutter: frames too fast -- shutter stays CLOSED for the whole stim.");
          }
        }
        lastShutterFrame = frameCounter;
        shutterFrameCloseAt = 0;
        optoArmAt = 0;
      }
      break;
    case SHUTTER_STIM:
      // Per-frame shutter alternation (if enabled and the frame rate allows it).
      if (alternatingShutterActive && stimulationActive) {
        driveShutterPerFrame();
      }
      // Stim fully complete (both flags clear) -> disarm opto, ensure CLOSED, then run the lag pad.
      if (!stimulationPending && !stimulationActive) {
        forceOptoOff();
        alternatingShutterActive = false;
        shutterFrameCloseAt = 0;
        optoArmAt = 0;
        setShutter(true);            // ensure CLOSED entering the lag pad (per-frame may have opened it)
        shutterPhase = SHUTTER_LAG;
        shutterPhaseStart = millis();
      }
      // Safety watchdog: if no frame ever started the stim, do not hold the shutter closed forever.
      // Cancel the pending start atomically w.r.t. the frame ISR so we can't reopen just as a stim begins.
      else if (stimulationPending && (millis() - shutterPhaseStart >= stimStartWatchdogRunMs)) {
        noInterrupts();
        bool startedInTime = stimulationActive;
        if (!startedInTime) stimulationPending = false;
        interrupts();
        if (!startedInTime) {
          optoArmed = false;
          alternatingShutterActive = false;
          setShutter(false);
          shutterPhase = SHUTTER_IDLE;
          Serial.println("Shutter sequencer: no frames started the stim -- shutter reopened (check frame source).");
        }
      }
      break;
    case SHUTTER_LAG:
      // Hold the shutter closed for the lag pad, then reopen.
      if (millis() - shutterPhaseStart >= shutterLagMs) {
        setShutter(false);           // OPEN shutter -- normal imaging
        shutterPhase = SHUTTER_IDLE;
        Serial.println("Shutter OPEN (pin 37 LOW); sequence complete.");
      }
      break;
    case SHUTTER_IDLE:
    default:
      break;
  }

  // Service the non-blocking opto pulse train (uses the latest optoArmed / stim state set above).
  driveOpto();
}

// ---- Frame sources --------------------------------------------------------------------------------
// Per-frame stim bookkeeping, called once per new frame by both frame sources. Opto emission itself is
// handled in loop() (driveOpto), so this stays short (safe inside an ISR / timer callback).
void onNewFrame() {
  frameCounter++;
  if (stimulationPending && (frameCounter % 2 != 0)) {
    stimulationPending = false;
    stimulationActive = true;
    stimulationStartTime = millis();
  }
}

// External 2P frame sync (CHANGE on pin 31). Counts a frame on each rising edge and measures the frame
// period (high + low) on the falling edge.
void measureFramePeriod() {
  unsigned long currentTime = micros();
  if (digitalRead(framePeriodPin) == HIGH) {        // rising edge = start of a new frame
    if (!measuringHigh) {
      frameLowDuration = currentTime - lastFrameRiseTime;
      measuringHigh = true;
    }
    lastFrameRiseTime = currentTime;
    onNewFrame();
  } else {                                          // falling edge
    if (measuringHigh) {
      frameHighDuration = currentTime - lastFrameRiseTime;
      measuringHigh = false;
      framePeriod = frameHighDuration + frameLowDuration;
    }
    lastFrameRiseTime = currentTime;
  }
}

// Internal frame clock (training / no 2P). Ticks a frame every internalFramePeriodUs with a known period.
void internalFrameTick() {
  framePeriod = internalFramePeriodUs;
  onNewFrame();
}

// ---- Non-blocking opto pulse generator ------------------------------------------------------------
// Driven from loop() every iteration (loop never blocks in alternating mode, so edges land within a few
// microseconds of target). Emits the 5/45 ms carrier ONLY while a stim is active, the shutter is confirmed
// closed (optoArmed), and we are on an odd (stim) frame. optoArmed==true implies the shutter was commanded
// CLOSED >= shutterActuationMs ago, so the LED never fires while the shutter is open or still moving. It runs
// in loop() thread context alongside the shutter writes, so there is no ISR race; when disabled it leaves
// optoPin LOW and never fights bypass mode's direct writes (which run while loop() is blocked in generateShutteredStim).
void driveOpto() {
  unsigned long now = micros();
  if (optoState) {                                  // LED currently ON -> end the pulse after pulseOnUs
    if (now - optoEdgeUs >= pulseOnUs) {
      digitalWrite(optoPin, LOW);
      optoState = false;
      optoEdgeUs = now;
    }
  } else {                                          // LED off -> start a pulse if enabled and off long enough
    if (stimulationActive && optoArmed && (frameCounter % 2 != 0) && (now - optoEdgeUs >= pulseOffUs)) {
      digitalWrite(optoPin, HIGH);
      optoState = true;
      optoEdgeUs = now;
    }
  }
}

// ---- Per-frame shutter driver ---------------------------------------------------------------------
// Alternating mode with a feasible frame rate: open the PMT shutter on non-stim (even) frames for imaging
// and close it before each stim (odd) frame. optoArmed is armed only after a close has had shutterActuationMs
// to settle, and forceOptoOff() guarantees the LED is off before every open.
void driveShutterPerFrame() {
  unsigned long fc = frameCounter;                  // snapshot once (avoid TOCTOU across compare/store/parity)
  if (fc != lastShutterFrame) {                     // a new frame edge arrived
    lastShutterFrame = fc;
    unsigned long framePeriodMs = framePeriod / 1000UL;
    if (fc % 2 == 0) {
      // Imaging (even) frame: force the LED off (also disarms), open the shutter, and schedule the close
      // so the shutter is shut shutterActuationMs before the next stim frame begins.
      forceOptoOff();
      optoArmAt = 0;
      setShutter(false);                            // OPEN for imaging
      shutterFrameCloseAt = millis() +
          (framePeriodMs > shutterActuationMs ? framePeriodMs - shutterActuationMs : 0);
    } else if (shutterFrameCloseAt != 0) {
      // Stim (odd) frame but the scheduled close hasn't run yet (frame arrived early): close now and arm
      // opto only after it settles (+ margin). Opto may miss this frame -- the safe direction.
      setShutter(true);
      shutterFrameCloseAt = 0;
      optoArmAt = millis() + shutterActuationMs + shutterSettleMarginMs;
    }
  }
  // Execute a scheduled close before the upcoming stim frame.
  if (shutterFrameCloseAt != 0 && millis() >= shutterFrameCloseAt) {
    setShutter(true);                               // CLOSE before the upcoming stim frame
    shutterFrameCloseAt = 0;
    optoArmAt = millis() + shutterActuationMs + shutterSettleMarginMs;  // arm after close settles (+ margin)
  }
  // Arm opto after the shutter has settled closed.
  if (optoArmAt != 0 && millis() >= optoArmAt) {
    optoArmed = true;
    optoArmAt = 0;
  }
}

// ---- Bypass mode ----------------------------------------------------------------------------------
// useAlternatingFrames == false: a single blocking, shutter-wrapped continuous train. Shutter CLOSED ->
// lead -> continuous opto carrier for optoDurationSec -> lag -> shutter OPEN. Needs no frame clock. The
// loop-driven opto state machine (driveOpto) is not reached while this blocks, and stimulationActive is
// false, so it never fights these direct optoPin writes.
void generateShutteredStim() {
  // Whole opto periods that fit in the stim (floors; a partial final cycle is fine for the continuous
  // bypass train). At 2 s / 20 Hz this is 40.
  const unsigned long numPulses = ((unsigned long)optoDurationSec * 1000000UL) / optoPeriodUs;

  Serial.println("Bypass stim: shutter CLOSED, lead, continuous 5/45 ms train, lag, shutter OPEN.");
  setShutter(true);
  delay(shutterLeadMs);
  for (unsigned long i = 0; i < numPulses; i++) {
    digitalWrite(optoPin, HIGH);
    delayMicroseconds(pulseOnUs);
    digitalWrite(optoPin, LOW);
    delayMicroseconds(pulseOffUs);
  }
  digitalWrite(optoPin, LOW);
  delay(shutterLagMs);
  setShutter(false);
  Serial.println("Bypass stim: shutter OPEN (pin 37 LOW). Done.");
}
