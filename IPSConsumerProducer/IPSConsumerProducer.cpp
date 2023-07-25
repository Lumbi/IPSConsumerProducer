// IPSConsumerProducer.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <Windows.h>
#include <exception>
#include <span>
#include <cstddef>  
#include <array>
#include <cassert>
#include <memory>
#include <atomic>

constexpr DWORD SHARED_MEMORY_SIZE = 256;
constexpr std::size_t RING_BUFFER_SIZE = 100;
constexpr LPCWSTR SHARED_MEMORY_NAME = L"IPSConsumerProducerSharedMemory";
constexpr LPCWSTR PRODUCER_DID_FINISH = L"ProducerDidFinish";
constexpr LPCWSTR CONSUMER_DID_FINISH = L"ConsumerDidFinish";
constexpr LPCWSTR RING_BUFFER_MUTEX = L"RingBufferMutex"; // Unused
constexpr LPCWSTR RING_BUFFER_SEMA_EMPTY = L"RingBufferSemaphoreEmpty";
constexpr LPCWSTR RING_BUFFER_SEMA_FILL = L"RingBufferSemaphorFill";
std::atomic<long long> sleepDuration(1000);

void CheckResult(BOOL result)
{
    if (!result)
    {
        std::cout << "Error: " << GetLastError() << std::endl;
    }
}

void CheckResult(DWORD result)
{
    switch (result)
    {
    case WAIT_FAILED: std::cout << "Wait Failed: " << GetLastError() << std::endl; break;
    case WAIT_ABANDONED: std::cout << "Wait Abandoned: " << GetLastError() << std::endl; break;
    default: break;
    }
}

void CheckHandle(HANDLE handle)
{
    if (!handle)
    {
        std::cout << "Handle Error: " << GetLastError() << std::endl;
    }
}

class SharedMemoryException: public std::exception
{
public:
    SharedMemoryException(std::string &&message) 
        : message(message) 
    {}

    virtual const char* what() const noexcept { return message.c_str(); };

private:
    std::string message;
};

class SharedMemory
{
public:
    using Name = LPCWSTR;
    using Size = DWORD;

    enum class Mode
    {
        CREATE,
        OPEN,
    };

    SharedMemory(Name name, Size size, Mode mode)
        : size(size)
    {
        switch (mode)
        {
        case Mode::CREATE: 
        {
            handle = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                NULL,
                PAGE_READWRITE,
                0,
                size,
                name
            );

            if (handle == nullptr)
                throw SharedMemoryException("Failed to create shared memory mapping.");
        }
        case Mode::OPEN:
        {
            handle = OpenFileMappingW(
                FILE_MAP_ALL_ACCESS,
                FALSE,
                name
            );

            if (handle == nullptr)
                throw SharedMemoryException("Failed to open shared memory mapping.");
        }
        }

        pointer = MapViewOfFile(
            handle,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            size
        );

        if (pointer == nullptr)
            throw SharedMemoryException("Failed to create view to shared memory.");
    }

    SharedMemory(SharedMemory&) = delete;
    SharedMemory& operator=(SharedMemory&) = delete;

    ~SharedMemory()
    {
        if (pointer != nullptr)
        {
            UnmapViewOfFile(pointer);
        }

        if (handle != nullptr)
        {
            CloseHandle(handle);
        }
    }

public:
    LPVOID data()
    {
        return pointer;
    }

    void read(std::ptrdiff_t offset, std::span<std::byte> &buffer)
    {
        CopyMemory(buffer.data(), static_cast<std::byte*>(pointer) + offset, buffer.size_bytes());
    }

    void write(std::ptrdiff_t offset, const std::span<const std::byte> &&bytes)
    {
        CopyMemory(static_cast<std::byte*>(pointer) + offset, bytes.data(), bytes.size_bytes());
    }

private:
    HANDLE handle;
    Size size;
    LPVOID pointer;
};

template<std::uint32_t size>
class RingBuffer
{
public:
    RingBuffer()
        : start(0),
          end(0),
          elements{}
    {
    }

    RingBuffer(RingBuffer&) = delete;
    RingBuffer& operator=(RingBuffer&) = delete;

    void push(std::byte byte)
    {        
        assert(!full());
        elements[end] = byte;
        end = (end + 1) % (size + 1);
    }

    std::byte front()
    {
        return elements[start];
    }

    void pop()
    {
        assert(!empty());
        start = (start + 1) % (size + 1);
    }

    // Not legit: you have different processes writing to start and end concurrently,
    // it's not meaningful to read those values and compare them.
    bool empty() { return start == end; }

    bool full() { return end == start - 1; }

private:
    // It doesn't make sense to have those in the shared memory since there is
    // currently no way for them to be accessed safely by both processes.
    std::uint32_t start;
    std::uint32_t end;
    std::array<std::byte, size + 1> elements; // Ideally you don't need the +1
};

void producer()
{
    std::cout << "Running as Producer..." << std::endl;

    SharedMemory memory(SHARED_MEMORY_NAME, SHARED_MEMORY_SIZE, SharedMemory::Mode::CREATE);
    auto buffer = new (memory.data()) RingBuffer<RING_BUFFER_SIZE>();
    unsigned char counter = 0;

    HANDLE ringBufferFillCount = CreateSemaphoreW(
        NULL,
        0, // initial
        RING_BUFFER_SIZE, // max
        RING_BUFFER_SEMA_FILL
    );
    CheckHandle(ringBufferFillCount);

    HANDLE ringBufferEmptyCount = CreateSemaphoreW(
        NULL,
        RING_BUFFER_SIZE, // initial
        RING_BUFFER_SIZE, // max
        RING_BUFFER_SEMA_EMPTY
    );
    CheckHandle(ringBufferEmptyCount);

    while (true)
    {
        CheckResult(WaitForSingleObject(ringBufferEmptyCount, INFINITE));

        assert(!buffer->full());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepDuration));
        ++counter;
        buffer->push(std::byte { counter });
        std::cout << "Produced: " << static_cast<int>(counter) << std::endl;

        CheckResult(ReleaseSemaphore(ringBufferFillCount, 1, NULL));
    }

    CloseHandle(ringBufferFillCount);
    CloseHandle(ringBufferEmptyCount);
}

void consumer()
{
    std::cout << "Running as Consumer..." << std::endl;
 
    SharedMemory memory(SHARED_MEMORY_NAME, SHARED_MEMORY_SIZE, SharedMemory::Mode::OPEN);

    auto buffer = static_cast<RingBuffer<RING_BUFFER_SIZE>*>(memory.data());

    HANDLE ringBufferFillCount = OpenSemaphoreW(
        SYNCHRONIZE, // Nicely specific
        FALSE, // inherit handle
        RING_BUFFER_SEMA_FILL
    );

    HANDLE ringBufferEmptyCount = OpenSemaphoreW(
        SEMAPHORE_MODIFY_STATE,
        FALSE, // inherit handle
        RING_BUFFER_SEMA_EMPTY
    );

    while (true)
    {
        CheckResult(WaitForSingleObject(ringBufferFillCount, INFINITE));

        assert(!buffer->empty());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepDuration));
        auto next = buffer->front();
        buffer->pop();
        std::cout << "Consumed: " << std::to_integer<int>(next) << std::endl;

        CheckResult(ReleaseSemaphore(ringBufferEmptyCount, 1, NULL));
    }
}

enum class Mode
{
    PRODUCER,
    CONSUMER,
    UNKNOWN,
};

int main()
{
    std::cout << "Start as a 'Producer' or 'Consumer'? (p/c): ";
    
    Mode mode = Mode::UNKNOWN;
    while (mode == Mode::UNKNOWN)
    {
        int input = std::cin.get();
        switch (input)
        {
        case 'p': mode = Mode::PRODUCER; break;
        case 'c': mode = Mode::CONSUMER; break;
        default: continue;
        }
    }

    std::thread worker;

    switch (mode)
    {
    case Mode::PRODUCER: worker = std::thread(producer); break;
    case Mode::CONSUMER: worker = std::thread(consumer); break;
    default: break;
    }
    
    while (true)
    {
        long long input;
        std::cin >> input;
        sleepDuration = input;
        std::cout << "Sleep: " << sleepDuration << std::endl;
    }
}
