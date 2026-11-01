#include <unity.h>
#include "../test_utils.h"
#include "storage.h"
#include "pages.h"
#include "ltt.h"
#include "raceTime.h"
#include "globals.h"
#include "fake_memory_storage.h"

/**
 * @file
 * @brief Tests for the EEPROM burn loop's termination behaviour.
 *
 * These exist because of a bug found in the field, on real hardware, with the engine off: the
 * burn indicator stayed lit continuously. The cause was a page save that ignored the storage
 * write budget and always reported success.
 *
 * The mechanism is worth spelling out, because it is not obvious from any single function:
 *
 *   speeduino.ino():
 *       if (isEepromWritePending() && ...) { saveAllPages(); }
 *
 *   saveAllPages():
 *       setEepromWritePending(false);                 // clears the flag
 *       while (page < MAX_PAGE_NUM && !pending) { savePage(page); ++page; }
 *
 *   savePage():
 *       setEepromWritePending(writesRemaining == 0U); // sets it again if anything is pending
 *
 * So the flag means "a burn is still in progress". The main loop keeps calling saveAllPages()
 * until the flag stays clear. A page that always reports "nothing pending" makes the flag clear
 * on every pass, so the loop runs forever and re-erases flash sectors continuously.
 *
 * Note the polarities, because both mistakes are possible and both are flash-fatal:
 *   - Returning "done" when work remains  => the loop restarts forever (the actual bug).
 *   - Returning "pending" when nothing remains => the loop never exits.
 *
 * These tests use the memory-backed storage fake, not the one-byte fake, because the whole
 * question is "is this already stored?" - which requires a fake that retains what is written.
 */

/** @brief Drive the burn loop to completion, with a hard iteration cap.
 *
 * The cap is the point: without it a runaway loop hangs the test host instead of failing,
 * which is far harder to diagnose than an assertion.
 */
static void run_burn_loop_to_completion(uint16_t maxIterations, uint16_t &iterations,
                                        uint16_t &writesBefore, uint16_t &writesAfter)
{
    writesBefore = memoryStorage.writeCount;
    iterations = 0U;

    while (isEepromWritePending() && iterations < maxIterations)
    {
        ++iterations;
        // Mirror speeduino.ino(): clear the request, then save pages until one reports pending.
        setEepromWritePending(false);
        saveAllPages();
    }

    writesAfter = memoryStorage.writeCount;
}

/** @brief A budget big enough for the whole tune: must terminate in a single pass. */
static void test_burn_terminates_with_large_budget(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, MEMORY_STORAGE_CAPACITY, 0xFF));
    lttResetForTest();

    setEepromWritePending(true);

    uint16_t iterations = 0U, before = 0U, after = 0U;
    run_burn_loop_to_completion(100U, iterations, before, after);

    TEST_ASSERT_FALSE_MESSAGE(isEepromWritePending(), "burn loop did not terminate");
    TEST_ASSERT_LESS_THAN_MESSAGE(100, iterations,
                                  "burn loop needed more passes than pages - it is looping");
}

/** @brief The regression case: a small budget forces the burn across many passes.
 *
 * This is the real-world shape. At RPM 0 the Teensy allows 160 writes per pass and the tune is
 * several kilobytes, so a save *must* take many passes. If any page fails to consume the budget
 * or misreports pending, the loop never ends.
 */
static void test_burn_terminates_with_small_budget_and_bounded_writes(void)
{
    constexpr uint16_t SMALL_BUDGET = 160U; // matches getMaxWriteBlockSize() at RPM 0

    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, SMALL_BUDGET, 0xFF));
    lttResetForTest();

    setEepromWritePending(true);

    uint16_t iterations = 0U, before = 0U, after = 0U;
    run_burn_loop_to_completion(5000U, iterations, before, after);

    TEST_ASSERT_FALSE_MESSAGE(isEepromWritePending(),
                              "burn loop did not terminate with a small budget");

    // Every write costs flash life, so the total must stay near the size of the data, NOT be
    // multiplied by the number of loop passes. A runaway loop shows tens of thousands here.
    const uint32_t totalBytes = 4096U + LTT_PAYLOAD_SIZE;
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(totalBytes * 2U, (uint32_t)(after - before),
                                      "burn wrote far more than the tune contains - it is rewriting in a loop");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (after - before), "burn wrote nothing");
}

/** @brief A second identical burn must write (almost) nothing. */
static void test_repeated_burn_is_idempotent(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, MEMORY_STORAGE_CAPACITY, 0xFF));
    lttResetForTest();

    // First burn: everything is blank, so this writes the whole tune.
    setEepromWritePending(true);
    uint16_t iterations = 0U, before = 0U, after = 0U;
    run_burn_loop_to_completion(100U, iterations, before, after);
    TEST_ASSERT_FALSE(isEepromWritePending());

    const uint16_t writesAfterFirstBurn = memoryStorage.writeCount;

    // Second burn with nothing changed: must rewrite nothing at all.
    setEepromWritePending(true);
    run_burn_loop_to_completion(100U, iterations, before, after);
    TEST_ASSERT_FALSE(isEepromWritePending());

    TEST_ASSERT_EQUAL_MESSAGE(writesAfterFirstBurn, memoryStorage.writeCount,
                              "a second burn with no changes wrote data again - that is needless flash wear");
}

/** @brief Page 16 must consume the budget, not claim to be finished.
 *
 * Contract: savePage() clears the burn-pending flag only when the budget hits zero, so a page
 * with work left must report pending.
 */
static void test_ltt_page_consumes_budget(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, 16U, 0xFF));
    lttResetForTest();
    lttMarkDirty();

    // The block is 2,064 bytes, so it cannot finish within 4 writes. It must say "still pending".
    TEST_ASSERT_TRUE_MESSAGE(lttNeedsSave(), "test setup failed: block should need saving");
    savePage(lttPage);
    TEST_ASSERT_TRUE_MESSAGE(isEepromWritePending(),
                             "page 16 claimed to be finished after a 4-write budget - this is the flash-wear bug");
}

/** @brief With no changes, page 16 must cost nothing and must not ask to be called again. */
static void test_ltt_page_is_noop_when_unchanged(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, MEMORY_STORAGE_CAPACITY, 0xFF));
    lttResetForTest();

    // Make storage match RAM.
    TEST_ASSERT_TRUE(lttSave());
    TEST_ASSERT_FALSE_MESSAGE(lttNeedsSave(), "save did not clear the comparison");
    const uint16_t writesBefore = memoryStorage.writeCount;

    savePage(lttPage);
    const uint16_t writesAfter = memoryStorage.writeCount;

    TEST_ASSERT_EQUAL_MESSAGE(writesBefore, writesAfter,
                              "page 16 rewrote an unchanged block - that burns flash for nothing");
    TEST_ASSERT_FALSE_MESSAGE(isEepromWritePending(),
                              "page 16 asked to be called again despite having nothing to write");
}

/** @brief lttNeedsSave must be false once the block matches storage. */
static void test_lttNeedsSave_false_after_save(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, MEMORY_STORAGE_CAPACITY, 0xFF));
    lttResetForTest();
    TEST_ASSERT_TRUE(lttSave());
    TEST_ASSERT_FALSE(lttNeedsSave());
}

/** @brief lttNeedsSave must be true when a trim changes. */
static void test_lttNeedsSave_true_when_dirty(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, MEMORY_STORAGE_CAPACITY, 0xFF));
    lttResetForTest();
    TEST_ASSERT_TRUE(lttSave());
    TEST_ASSERT_FALSE(lttNeedsSave());

    lttMarkDirty();
    TEST_ASSERT_TRUE(lttNeedsSave());
}

/** @brief A saved unit block must contain a header the boot loader of the block can validate.
 *
 * The header is written last precisely so a torn save is detectable. This asserts the ordering
 * works: after a completed save, magic/version/CRC all agree with the payload.
 */
static void test_saved_block_is_self_consistent(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, MEMORY_STORAGE_CAPACITY, 0xFF));
    lttResetForTest();

    // Change something so the header must be refreshed.
    lttMarkDirty();
    lttGetPageMutable().trim[0][0] = 123;
    TEST_ASSERT_TRUE(lttSave());

    // A reload must accept the block: this exercises the real magic/version/CRC check.
    lttResetForTest();
    lttInit();
    TEST_ASSERT_TRUE(lttIsInitialised());
    TEST_ASSERT_EQUAL_INT16_MESSAGE(123, lttGetPage().trim[0][0],
                                    "trim did not survive a save/reload - the header did not match the payload");
}

/** @brief An unavailable backend must not keep the burn loop spinning. */
static void test_ltt_page_terminates_when_storage_too_small(void)
{
    // 4284 bytes is the internal-EEPROM fallback: it cannot reach 0x1000 at all.
    setStorageAPI(getMemoryStorageApi(4284U, 160U, 0xFF));
    lttResetForTest();

    TEST_ASSERT_FALSE_MESSAGE(lttIsAvailable(),
                              "the trim block must disable itself on a backend too small to hold it");
    TEST_ASSERT_FALSE(lttNeedsSave());

    // Page 16 must report "done" so a hopeless burn still terminates.
    savePage(lttPage);
    TEST_ASSERT_FALSE_MESSAGE(isEepromWritePending(),
                              "page 16 kept a hopeless burn pending - the loop would never end");
}


// ============================ Page 17: race-time block ============================
//
// The same contract as page 16, and it exists because page 16 got it wrong once and wore
// flash continuously. Page 17 is a fresh implementation of that contract, so it is tested the
// same way rather than assumed correct.

/** @brief Page 17 must terminate a hopeless burn (backend too small for the block). */
static void test_racetime_page_terminates_when_storage_too_small(void)
{
    setStorageAPI(getMemoryStorageApi(4284U, 160U, 0xFF));
    initialiseRaceTime(currentStatus);

    TEST_ASSERT_FALSE_MESSAGE(raceTimeIsAvailable(),
                              "the race-time block must disable itself on a backend too small to hold it");
    TEST_ASSERT_FALSE(raceTimeNeedsSave());

    savePage(raceTimePage);
    TEST_ASSERT_FALSE_MESSAGE(isEepromWritePending(),
                              "page 17 kept a hopeless burn pending - the loop would never end");
}

/** @brief Page 17 must consume the budget honestly and never loop forever. */
static void test_racetime_page_consumes_budget(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, 160U, 0xFF));
    initialiseRaceTime(currentStatus);
    TEST_ASSERT_TRUE_MESSAGE(raceTimeIsAvailable(), "test needs a backend large enough for the block");

    raceTimeMarkDirty();
    setEepromWritePending(true);

    uint16_t iterations = 0U, before = 0U, after = 0U;
    run_burn_loop_to_completion(200U, iterations, before, after);

    TEST_ASSERT_FALSE_MESSAGE(isEepromWritePending(), "burn loop did not terminate with page 17 present");
    TEST_ASSERT_LESS_THAN_MESSAGE(200, iterations, "burn loop needed more passes than pages - it is looping");
}

/** @brief An unchanged page 17 must not rewrite its payload on every full-tune burn. */
static void test_racetime_page_is_noop_when_unchanged(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, MEMORY_STORAGE_CAPACITY, 0xFF));
    initialiseRaceTime(currentStatus);

    // Settle it once.
    (void)raceTimeSave();
    TEST_ASSERT_FALSE(raceTimeNeedsSave());

    const uint16_t writesBefore = memoryStorage.writeCount;
    savePage(raceTimePage);
    const uint16_t writesAfter = memoryStorage.writeCount;

    TEST_ASSERT_EQUAL_MESSAGE(writesBefore, writesAfter,
                              "an unchanged page 17 rewrote its payload - that is flash wear for nothing");
}

/** @brief A torn payload (bad CRC) must be discarded, never applied. */
static void test_racetime_block_is_self_consistent_after_save(void)
{
    setStorageAPI(getMemoryStorageApi(MEMORY_STORAGE_CAPACITY, MEMORY_STORAGE_CAPACITY, 0xFF));
    initialiseRaceTime(currentStatus);

    // Corrupt the payload behind the block's back, leaving the old header (and CRC) in place.
    memoryStorage.data[EEPROM_RACETIME_PAYLOAD + 5U] ^= 0xFFU;

    initialiseRaceTime(currentStatus);

    // It must have fallen back to defaults rather than loading the corrupted curve.
    TEST_ASSERT_EQUAL_UINT8(RT_DISABLED, raceTimeGetPage().enable);
}

void testBurnLoop(void)
{
    SET_UNITY_FILENAME() {
        RUN_TEST(test_burn_terminates_with_large_budget);
        RUN_TEST(test_burn_terminates_with_small_budget_and_bounded_writes);
        RUN_TEST(test_repeated_burn_is_idempotent);
        RUN_TEST(test_ltt_page_consumes_budget);
        RUN_TEST(test_ltt_page_is_noop_when_unchanged);
        RUN_TEST(test_lttNeedsSave_false_after_save);
        RUN_TEST(test_lttNeedsSave_true_when_dirty);
        RUN_TEST(test_saved_block_is_self_consistent);
        RUN_TEST(test_ltt_page_terminates_when_storage_too_small);
        RUN_TEST(test_racetime_page_terminates_when_storage_too_small);
        RUN_TEST(test_racetime_page_consumes_budget);
        RUN_TEST(test_racetime_page_is_noop_when_unchanged);
        RUN_TEST(test_racetime_block_is_self_consistent_after_save);
    }
}
