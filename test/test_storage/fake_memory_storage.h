#pragma once

#include "storage_api.h"

#include <stdint.h>

/**
 * @file
 * @brief A memory-backed fake storage API that actually retains what is written.
 *
 * The older `getOneByteStorageApi()` counts writes and reads but never stores anything: reads
 * always return a fixed filler byte. That is fine for asserting "N bytes were written", but it
 * makes any "has this been persisted yet?" logic impossible to test, because a read-back can
 * never match what was written.
 *
 * That distinction matters more than it sounds. The burn loop's termination depends on being
 * able to tell "this page is already stored" from "this page still needs writing", and the
 * long-term trim block answers that by comparing RAM against storage. With a fake that never
 * retains, every comparison says "differs", so the block asks to be written again forever - a
 * perfect reproduction of the flash-wear bug, and exactly what these tests need to exercise.
 */

constexpr uint16_t MEMORY_STORAGE_CAPACITY = 32768U;

struct memory_storage_t
{
    uint8_t data[MEMORY_STORAGE_CAPACITY];
    uint16_t length;      ///< Addressable size exposed to the firmware
    uint16_t blockSize;   ///< Write budget returned by getMaxWriteBlockSize
    uint16_t readCount;
    uint16_t writeCount;  ///< Number of bytes actually written (i.e. that differed)
};

extern memory_storage_t memoryStorage;

/** @brief Install a memory-backed storage API, pre-filled with @p fill. */
storage_api_t getMemoryStorageApi(uint16_t length, uint16_t blockSize, uint8_t fill);
