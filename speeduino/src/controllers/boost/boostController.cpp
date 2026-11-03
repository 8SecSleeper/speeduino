#include "../../pins/boardOutputPin.h"
#include "../../../globals.h"
#include "../../../unit_testing.h"
#include "../../../units.h"
#include "../../../boostLaunch.h"
#include "../../../ppLaunch.h"
#include "../../PID/integerPID_ideal.h"
#include "../../../timers.h"
#include "../../pwm/PwmOutputChannel.h"
#include "src/pins/boardOutputPin.h"
#include "src/pwm/interruptHandlers.h"

TESTABLE_STATIC PwmOutputChannel<boardOutputPin_t> boostOutput;
TESTABLE_STATIC integerPID_ideal boostPID; //This is the PID object if that algorithm is used. Needs to be global as it maintains state outside of each function call

TESTABLE_CONSTEXPR table2D_u8_s16_6 flexBoostTable(&configPage10.flexBoostBins, &configPage10.flexBoostAdj);

// Convert a percentage to half percentage (percentage * 2)
static constexpr uint16_t percentToHalfPct(uint16_t percent)
{
  return percent * 2U;
}

// Convert a percentage to duty cycle (percentage * 100)
static constexpr uint16_t percentToDuty(uint16_t percent)
{
  return percent * 100U;
}

static __attribute__((optimize("Os"))) void setBoostPidTunings(const config2 &page2, const config6 &page6, const config10 &page10) noexcept
{
  if(page6.boostMode == BOOST_MODE_SIMPLE)
  {
    boostPID.setTunings(PidTuningParameters());
  }
  else
  {
    boostPID.setTunings(PidTuningParameters(page6.boostKP, page6.boostKI, page6.boostKD));
  }
  boostPID.setOutputLimits(page2.boostMinDuty, page2.boostMaxDuty);
  boostPID.setSampleTime(millis(), page10.boostIntv);
  boostPID.setSensitivity(page10.boostSens);
}

__attribute__((optimize("Os"))) void initialiseBoost(statuses &current, const config2 &page2, const config6 &page6, const config10 &page10, const pinNumbers_t &pins)
{
  boostOutput = PwmOutputChannel<boardOutputPin_t>(pins.pinBoost, FREQUENCY.toUser(page6.boostFreq));
  setBoostPidTunings(page2, page6, page10);
  current.boostDuty = 0;
}

static uint16_t getBoostByGearFactor(const statuses &current, const config9 &page9) noexcept
{
  if ((current.gear>0U) && (current.gear-1U)<_countof(page9.boostByGear))
  {
    return PRESSURE.toUser(page9.boostByGear[current.gear-1U]);
  }
  return 0U;
}

enum class BoostByGearMode : uint8_t
{
  Off,
  Percent,
  Constant
};

static inline BoostByGearMode getBoostByGearMode(const config2 &page2, const config9 &page9)
{
  if( (page9.boostByGearEnabled == BOOST_BY_GEAR_PERCENT) && isExternalVssMode(page2) )
  {
    return BoostByGearMode::Percent;
  }
  else if( (page9.boostByGearEnabled == BOOST_BY_GEAR_CONSTANT) && isExternalVssMode(page2) )
  {
    return BoostByGearMode::Constant;
  }
  else
  {
    return BoostByGearMode::Off;
  }
}

static inline uint16_t lookupBoostTable(const statuses &current) noexcept
{
  // In open loop mode, the values in this table are duty cycle %
  // In closed loop mode, the values in this table are boost targets in kPa
  // In both cases, the values are stored in the table as kPa/2 (i.e. 1kPa = 2 in the table)
  return PRESSURE.toUser(get3DTableValue(&boostTable, percentToHalfPct(current.TPS), current.RPM));
}

static uint16_t getBoostDuty(const statuses &current, const config2 &page2, const config9 &page9)
{
  uint16_t duty = 0;
  if (getBoostByGearMode(page2, page9) == BoostByGearMode::Percent)
  {
    duty = getBoostByGearFactor(current, page9) * lookupBoostTable(current);
  }
  else if (getBoostByGearMode(page2, page9) == BoostByGearMode::Constant)
  {
    duty = percentToDuty(getBoostByGearFactor(current, page9));
  }
  else
  {
    duty = percentToDuty(lookupBoostTable(current));
  }
  return clamp(duty, (uint16_t)0, percentToDuty(100U));
}

/**
 * @brief The BASE boost target, before any runtime modifier is applied.
 *
 * This is separated from the modifier chain below on purpose. A feature that needs to substitute
 * the target - launch boost, and later traction control - must supply the BASE, so that flex-fuel
 * compensation and everything else still stacks on top of it. Overwriting the FINAL target would
 * silently disable flex and every future modifier, and the tuner would see a target that ignored
 * half their tune with nothing in the log explaining why.
 */
static uint16_t getBaseBoostTarget(const statuses &current, const config2 &page2, const config9 &page9)
{
  uint16_t target = 0;
  if (getBoostByGearMode(page2, page9) == BoostByGearMode::Percent)
  {
    target = percentage(lookupBoostTable(current), getBoostByGearFactor(current, page9));
  }
  else if (getBoostByGearMode(page2, page9) == BoostByGearMode::Constant)
  {
    target = getBoostByGearFactor(current, page9);
  }
  else
  {
    // LCOV_EXCL_BR_START
    INTERNAL_TEST_ASSERT(BoostByGearMode::Off == getBoostByGearMode(page2, page9));
    // LCOV_EXCL_BR_STOP

    //Boost target table is in kpa and divided by 2
    target = lookupBoostTable(current);
  }
  return target;
}

static uint16_t getBoostTarget(const statuses &current, const config2 &page2, const config9 &page9)
{
  // ---- 1. base target -----------------------------------------------------------------
  // While the launch boost window is open it supplies the BASE. Outside that window - or with
  // the feature switched off - boostLaunchIsActive() is false and this is exactly the tune's own
  // target, with no "hold the last value" state left behind.
  uint16_t target = boostLaunchIsActive() ? boostLaunchBaseTarget()
                                          : getBaseBoostTarget(current, page2, page9);

  // ---- 2. flex fuel, and any future target modifier -----------------------------------
  // Additive and applied AFTER the base is chosen, so launch boost cannot disable it. This is
  // the ordering that makes the two features compose: switching launch boost on changes where
  // the target starts from, not whether the rest of the tune still applies.
  //
  // A future traction-control target modifier belongs in this same chain, below this line.
  //
  // flexBoostCorrection is int16_t; beware of conversion under-/over-flow
  int16_t correctedTarget = (int16_t)target + current.flexBoostCorrection;

  // The launch reactions table may also trim the TARGET, in kPa. Applied here, after flex, so it
  // composes with every other target modifier rather than replacing them.
  correctedTarget = (int16_t)(correctedTarget + ppLaunchBoostTargetAdj());

  return clamp(correctedTarget, (int16_t)0, (int16_t)511);
}

static int16_t getFlexCorrection(const statuses &current, const config2 &page2)
{
  //If flex fuel is enabled, there can be an adder to the boost target based on ethanol content
  if( page2.flexEnabled )
  {
    return table2D_getValue(&flexBoostTable, current.ethanolPct);
  }
  return 0U;
}

static uint16_t convertTargetToDuty(const statuses &current, const config2 &page2, const config6 &page6, const config10 &page10)
{
  uint16_t duty = 0;
  if(current.boostTarget > 0)
  {
    // LCOV_EXCL_BR_START
    // The timer check *MUST* be a multiple of the boost control interval
    // otherwise branch will NEVER be taken.
    if( BIT_CHECK(current.LOOP_TIMER, BIT_TIMER_1HZ) )
    // LCOV_EXCL_BR_STOP
    {
      setBoostPidTunings(page2, page6, page10);
    }

    boostPID.setSetPoint(current.boostTarget);
    boostPID.setFeedForwardTerm(get3DTableValue(&boostTableLookupDuty, current.boostTarget, current.RPM) * 50U);
    // LCOV_EXCL_BR_START
    (void)boostPID.compute(millis(), current.MAP, &duty);
    // LCOV_EXCL_BR_STOP
  }

  return duty;
}

static void applyDutyToPwm(const statuses &current)
{
  // Convert boost duty (Which is a % multiplied by 100) to half percentage. I.e 0-200
  boostOutput.setTargetDuty(fast_div_closest(current.boostDuty, 50U));

  if (boostOutput.isPartialDuty())
  { 
    ENABLE_BOOST_TIMER(); //Turn on the compare unit (ie turn on the interrupt) if boost duty >0
  }
  // Check for 100% duty cycle
  else
  {
    DISABLE_BOOST_TIMER(); 
  }
}

static bool isBoostActive(const statuses &current, const config6 &page6)
{
  return (page6.boostEnabled)
      && (current.rotationStatus==EngineRotationStatus::Running)
  ;
}

static inline bool isBoostControlBaroActive(const statuses &current, const config15 &page15)
{
  return (page15.boostControlEnable == EN_BOOST_CONTROL_BARO) 
      && (current.MAP >= current.baro)
      ;
}

static inline bool isBoostControlFixedActive(const statuses &current, const config15 &page15)
{
  return (page15.boostControlEnable == EN_BOOST_CONTROL_FIXED) 
      && (current.MAP >= page15.boostControlEnableThreshold)
      ;
}

TESTABLE_STATIC void boostControlCore(statuses &current, const config2 &page2, const config4 &page4, const config6 &page6, const config9 &page9, const config10 &page10, const config15 &page15)
{
  if(isBoostActive(current, page6) )
  {
    if(page4.boostType == OPEN_LOOP_BOOST)
    {
      current.boostDuty = getBoostDuty(current, page2, page9);
    }
    else // CLOSED_LOOP_BOOST
    {
      // No inner rate gate here. There used to be a BIT_TIMER_10HZ check nested inside the outer
      // BIT_TIMER_30HZ gate in boostControl(). LOOP_TIMER is cleared every main-loop pass, and the
      // 30Hz/10Hz bits come from independent 33ms/100ms counters, so the two only ever appeared in
      // the same mask every lcm(33,100)=3300ms. In other words closed-loop boost recomputed its
      // target once every ~3.3 SECONDS, not 10x/second. The caller now owns a single 200Hz gate and
      // this runs on every call. See roadmap §6B P3.
      current.flexBoostCorrection = getFlexCorrection(current, page2);
      // Base target (launch or the tune's own) + modifiers, in that order. See getBoostTarget().
      current.boostTarget = getBoostTarget(current, page2, page9);
      current.boostDuty = convertTargetToDuty(current, page2, page6, page10);

      // Only enables boost control above baro pressure or above user defined threshold 
      // (User defined level is usually set to boost with wastegate actuator only boost level)
      if(!isBoostControlBaroActive(current, page15) && !isBoostControlFixedActive(current, page15)) 
      {
        boostPID.initialize(current.MAP); //This resets the ITerm value to prevent rubber banding
        // Boost control needs to have a high duty cycle if control is below threshold (baro or fixed value). 
        // This ensures the waste gate is closed as much as possible, this build boost as fast as possible.
        current.boostDuty = page15.boostDCWhenDisabled*100;
      } //MAP above boost + hyster
    } //Open / Cloosed loop
  }
  else { // Disable timer channel and zero the flex boost correction status
    boostPID.initialize(current.MAP); //This resets the ITerm value to prevent rubber banding
    current.boostTarget = 0;
    current.boostDuty = 0;
    current.flexBoostCorrection = 0;
  }

  // ---- 3. duty trim ---------------------------------------------------------------------
  // Applied on the single exit path so it covers all three producers of duty: open loop, the
  // closed-loop PID, and the boostDCWhenDisabled spool override. Reduction only (0..100%), so it
  // cannot raise duty and cannot fight boostMinDuty/boostMaxDuty, which the PID has already
  // applied by this point.
  //
  // Two sources, combined as the LARGEST rather than the sum - the same arbitration rule the
  // torque bus uses, and for the same reason: two systems each asking for 30% off do not mean
  // 60% off, and summing is how you end up with a solenoid commanded to nothing.
  // Sources, combined as the LARGEST magnitude rather than the sum - two systems each asking for
  // 30% off do not mean 60% off, and summing is how you end up with a solenoid commanded to
  // nothing. The reactions table is signed (it can add duty as well as remove it), so it is
  // handled separately below rather than being folded into the 0..100 trim.
  uint8_t trimPercent = boostLaunchDutyTrim();

  // A launch limit strategy that includes "reduce boost" banks a fixed reduction for as long as
  // the limit is engaged. The strategy is what the tuner selected; the reactions table is how
  // much, which is why the table can override it with something larger.
  if (ppLaunchIsLimiting() && ((ppLaunchStrategy() & PP_STRAT_BOOST_REDUCE) != 0U))
  {
    // 20% is the default magnitude: enough to be felt at the top of a launch without making the
    // car feel like it shut off. A tuner who wants more uses the reactions table.
    constexpr uint8_t kStrategyTrim = 20U;
    if (kStrategyTrim > trimPercent) { trimPercent = kStrategyTrim; }
  }

  // The reactions table's signed adjustment. Applied as a separate step because it can go either
  // way: a negative adjustment trims duty in the same manner as trimPercent, a positive one adds.
  const int16_t reactionAdj = ppLaunchBoostDutyAdj();

  if ((trimPercent > 0U) && (current.boostDuty > 0U))
  {
    current.boostDuty = (uint16_t)(((uint32_t)current.boostDuty * (100U - trimPercent)) / 100U);
  }

  // Signed reaction adjustment, applied last and clamped to the legal duty range so a large
  // positive adjustment cannot push duty past 100%.
  if (reactionAdj != 0)
  {
    int32_t adjusted = (int32_t)current.boostDuty
                     + ((int32_t)current.boostDuty * reactionAdj / 100);
    if (adjusted < 0) { adjusted = 0; }
    if (adjusted > (int32_t)percentToDuty(100U)) { adjusted = (int32_t)percentToDuty(100U); }
    current.boostDuty = (uint16_t)adjusted;
  }

  applyDutyToPwm(current);
}

// LCOV_EXCL_START
void boostControl(statuses &current, const config2 &page2, const config4 &page4, const config6 &page6, const config9 &page9, const config10 &page10, const config15 &page15)
{
  // Single 200Hz gate for both open and closed loop. The PID runs inside convertTargetToDuty(), so
  // open and closed loop now share one rate - previously closed loop was additionally throttled by a
  // nested 10Hz check that could only coincide with this one every 3.3 seconds (see §6B P3).
  if( BIT_CHECK(current.LOOP_TIMER, BIT_TIMER_200HZ) )
  {
    boostControlCore(current, page2, page4, page6, page9, page10, page15);
  }
}
// LCOV_EXCL_STOP

//The interrupt to control the Boost PWM
void boostInterrupt(void)
{
  auto setTimerCallback =[](uint16_t tickDelta) {
    SET_COMPARE(BOOST_TIMER_COMPARE, BOOST_TIMER_COUNTER + tickDelta );
  };
  pwmISR(boostOutput, setTimerCallback);
}
