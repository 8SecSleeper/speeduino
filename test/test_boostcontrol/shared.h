#pragma once

#include "src/controllers/boost/boostController.h"

struct test_context_t
{
    statuses current;
    pinNumbers_t pins;
    config2 page2;
    config4 page4;
    config6 page6;
    config9 page9;
    config10 page10;
    config15 page15;

    void initialise(void)
    {
        initialiseBoost(current, page2, page6, page10, pins);
    }

    void boostControl(void)
    {
        extern void boostControlCore(statuses &current, const config2 &page2, const config4 &page4, const config6 &page6, const config9 &page9, const config10 &page10, const config15 &page15);
        boostControlCore(current, page2, page4, page6, page9, page10, page15);
    }

    void setup_boost_enabled(void)
    {
        current.rotationStatus = EngineRotationStatus::Running;
        // All timer bits at once. NOTE: test_context_t::boostControl() calls boostControlCore()
        // directly, so it bypasses the rate gate entirely - which is exactly why the old nested
        // 30Hz/10Hz bug in boostControl() (roadmap §6B P3) was invisible to this suite. The gate
        // itself is covered separately by test_boostControl_gate_runs_on_200Hz in test_boostcontrol.cpp.
        current.LOOP_TIMER = 0xFF;
        current.MAP = 50;
        page6.boostEnabled = true;
    }

};

test_context_t setup_boost_tune(bool fullPid, uint8_t vssMode, uint8_t boostType, uint8_t gearMode);