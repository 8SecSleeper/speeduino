#include "fake_memory_storage.h"

#include <string.h>

memory_storage_t memoryStorage;

static byte memoryRead(uint16_t address)
{
    ++memoryStorage.readCount;
    if (address >= MEMORY_STORAGE_CAPACITY)
    {
        return 0xFFU;
    }
    return memoryStorage.data[address];
}

static void memoryWrite(uint16_t address, byte value)
{
    if ((address < MEMORY_STORAGE_CAPACITY) && (memoryStorage.data[address] != value))
    {
        ++memoryStorage.writeCount;
        memoryStorage.data[address] = value;
    }
}

static uint16_t memoryLength(void)
{
    return memoryStorage.length;
}

static uint16_t memoryGetMaxWriteBlockSize(const statuses &)
{
    return memoryStorage.blockSize;
}

storage_api_t getMemoryStorageApi(uint16_t length, uint16_t blockSize, uint8_t fill)
{
    memset(memoryStorage.data, fill, sizeof(memoryStorage.data));
    memoryStorage.length = length;
    memoryStorage.blockSize = blockSize;
    memoryStorage.readCount = 0U;
    memoryStorage.writeCount = 0U;

    return {
        .read = memoryRead,
        .write = memoryWrite,
        .length = memoryLength,
        .getMaxWriteBlockSize = memoryGetMaxWriteBlockSize,
    };
}
