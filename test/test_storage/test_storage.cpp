#include <unity.h>
#include "../test_utils.h"
#include "storage.h"
#include "pages.h"
#include "sensors.h"
#include "fake_storage.h"

constexpr char WRITE_MARKER = 'X';
constexpr char BUFFER_MARKER = 'A';

static void test_saveAllPages(void)
{
    setStorageAPI(getOneByteStorageApi(8192, 8192, BUFFER_MARKER));
    saveAllPages();
    TEST_ASSERT_FALSE(isEepromWritePending());

    setStorageAPI(getOneByteStorageApi(8192, 16, BUFFER_MARKER));
    saveAllPages();
    TEST_ASSERT_TRUE(isEepromWritePending());
}

static void assert_entity(page_iterator_t iter, char expectedContent)
{
    for (uint16_t offset=0; offset<iter.entity.size; ++offset)
    {
        char szMsg[64];
        snprintf(szMsg, _countof(szMsg)-1, "Page %" PRIu8 ", Offset %" PRIu16, iter.location.page, offset);
        TEST_ASSERT_EQUAL_MESSAGE(expectedContent, getPageValue(iter.location.page, offset), szMsg);
    }
}
static void assert_page(uint8_t pageNum, char expectedContent)
{
    page_iterator_t pageIter = page_begin(pageNum);
    while (pageIter.entity.type!=EntityType::End)
    {
        assert_entity(pageIter, expectedContent);
        pageIter = advance(pageIter);
    }
}

static void test_loadAllPages(void)
{
    setStorageAPI(getOneByteStorageApi(8192, 8192, BUFFER_MARKER));
    loadAllPages();
    for (uint8_t page = MIN_PAGE_NUM; page<MAX_PAGE_NUM; ++page) {
        // Page 16 (long-term trims) is deliberately NOT loaded by loadAllPages(). It lives
        // above the core tune block, carries its own magic/version/CRC header, and must be
        // able to refuse to load when the storage backend is too small to contain it. That
        // decision belongs to lttInit() (see speeduino/ltt.cpp), which re-loads page 16 only
        // after validating the header. Loading it here as well would apply unvalidated trim
        // data before the CRC had been checked.
        if (page==lttPage) {
            continue;
        }
        // Page 17 (race-time framework) is excluded for the same reason as page 16, and by the
        // same mechanism: it lives above the core tune block with its own magic/version/CRC
        // header, and initialiseRaceTime() (see speeduino/raceTime.cpp) is what decides whether
        // to load it, based on the storage backend actually being large enough and the header
        // actually matching. Loading it here would apply an unvalidated curve.
        if (page==raceTimePage) {
            continue;
        }
        // Pages 18, 19 and 20 (launch control and launch boost) are excluded for the same
        // reason and by the same mechanism as pages 16 and 17: they sit above the core tune block
        // with their own magic/version/CRC headers, and initialisePpLaunch() (see
        // speeduino/ppLaunch.cpp) is what decides whether to load them. Loading them here would
        // apply an unvalidated launch curve - i.e. unvalidated ignition retard and a rev limit.
        if ((page==ppLaunchPage) || (page==ppLaunchRpmPage) || (page==boostLaunchPage)) {
            continue;
        }
        assert_page(page, BUFFER_MARKER);
    }
}


template <typename axis_t, typename value_t, uint8_t sizeT>
static void assert_2dtable(const table2D<axis_t, value_t, sizeT> &table, axis_t expectedAxis, value_t expectedValue)
{
    for (uint16_t index=0; index<sizeT; ++index)
    {
        TEST_ASSERT_EQUAL(expectedAxis, table.axis[index]);
        TEST_ASSERT_EQUAL(expectedValue, table.values[index]);
    }
}

static void test_loadAllCalibrationTables(void)
{
    setStorageAPI(getOneByteStorageApi(8192, 8192, BUFFER_MARKER));
    loadAllCalibrationTables();
    uint16_t U16_MARKER = word(BUFFER_MARKER, BUFFER_MARKER);
    constexpr uint8_t U8_MARKER = BUFFER_MARKER;

    assert_2dtable(o2CalibrationTable, U16_MARKER, U8_MARKER);
    assert_2dtable(iatCalibrationTable, U16_MARKER, U8_MARKER);
    assert_2dtable(cltCalibrationTable, U16_MARKER, U8_MARKER);
}

template <typename axis_t, typename value_t, uint8_t sizeT>
static void load_2dtable(table2D<axis_t, value_t, sizeT> &table, axis_t axis, value_t value)
{
    for (uint16_t index=0; index<sizeT; ++index)
    {
        table.axis[index] = axis;
        table.values[index] = value;
    }
}

static void test_saveAllCalibrationTables(void)
{
    uint16_t U16_MARKER = word(WRITE_MARKER, WRITE_MARKER);
    constexpr uint8_t U8_MARKER = WRITE_MARKER;

    load_2dtable(o2CalibrationTable, U16_MARKER, U8_MARKER);
    load_2dtable(iatCalibrationTable, U16_MARKER, U8_MARKER);
    load_2dtable(cltCalibrationTable, U16_MARKER, U8_MARKER);

    setStorageAPI(getOneByteStorageApi(8192, 8192, BUFFER_MARKER));
    saveAllCalibrationTables();
    TEST_ASSERT_GREATER_THAN(0, oneByteEeprom.readCount);
    TEST_ASSERT_GREATER_THAN(0, oneByteEeprom.writeCount);

    setStorageAPI(getOneByteStorageApi(8192, 8192, BUFFER_MARKER));
    saveCalibrationTable((SensorCalibrationTable)-1);
    TEST_ASSERT_EQUAL(0, oneByteEeprom.readCount);
    TEST_ASSERT_EQUAL(0, oneByteEeprom.writeCount);
}

void test_storage(void) {
    SET_UNITY_FILENAME() {     
        RUN_TEST_P(test_saveAllPages);
        RUN_TEST_P(test_loadAllPages);
        RUN_TEST_P(test_loadAllCalibrationTables);
        RUN_TEST_P(test_saveAllCalibrationTables);
    }
}