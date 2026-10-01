#include <sys/mman.h>
#include <unistd.h>

#include "data-buffer.hpp"

DataBuffer::DataBuffer(void *memory, int fd, size_t size)
{
    this->memory = static_cast<std::byte *>(memory);
    this->fd = fd;
    this->size = size;
}

DataBuffer::~DataBuffer()
{
    munmap(this->memory, this->size);
    close(this->fd);
}

void *DataBuffer::GetLatest()
{
    const DataBufferHeader *header = reinterpret_cast<const DataBufferHeader *>(this->memory);

    int writeIndex = header->writeIndex;
    if (writeIndex < 0 || static_cast<unsigned int>(writeIndex) >= header->capacity)
        return nullptr;

    size_t offset = header->dataOffset + static_cast<size_t>(header->elementSize) * writeIndex;
    if (offset + header->elementSize > this->size)
        return nullptr;

    // We already returned this buffer.
    if (this->lastBufferReceived == writeIndex)
        return nullptr;

    this->lastBufferReceived = writeIndex;

    return this->memory + offset;
}
