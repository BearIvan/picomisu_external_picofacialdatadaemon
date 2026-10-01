#pragma once

#include <cstddef>

struct DataBufferHeader
{
    // Always 1
    unsigned int version;
    // Bytes per slot
    unsigned int elementSize;
    // Total slots
    unsigned int capacity;
    // Latest slot written to (-1 for no slot written)
    int writeIndex;
    // Offset to data
    unsigned int dataOffset;
};

class DataBuffer
{
public:
    DataBuffer(void *memory, int fd, size_t size);
    ~DataBuffer();
    /**
     * @returns The memory pointer to the latest written slot, or nullptr if no slot was written
     * since the previous call.
     */
    void *GetLatest();

private:
    std::byte *memory;
    int fd;
    size_t size;

    int lastBufferReceived = -1;
};
