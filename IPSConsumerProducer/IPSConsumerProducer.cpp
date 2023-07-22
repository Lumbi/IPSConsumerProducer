// IPSConsumerProducer.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <Windows.h> // Protip: windows.h is nicer with https://github.com/microsoft/wil (started as Windows Internal Libraries)
#include <exception>
#include <span>
#include <cstddef>  
#include <array>
#include <cassert>
#include <memory>

constexpr DWORD SHARED_MEMORY_SIZE = 256;
constexpr std::size_t RING_BUFFER_SIZE = 100;
// LPCWSTR is always going to be wchar_t*, but TEXT would make your string a char* if compiling as ANSI. Better use L"Foo".
constexpr LPCWSTR SHARED_MEMORY_NAME = TEXT("IPSConsumerProducerSharedMemory");
constexpr LPCWSTR PRODUCER_DID_FINISH = TEXT("ProducerDidFinish");
constexpr LPCWSTR CONSUMER_DID_FINISH = TEXT("ConsumerDidFinish");
constexpr LPCWSTR RING_BUFFER_MUTEX = TEXT("RingBufferMutex");
int elementsPerTick = 1;
constexpr long long sleepDuration = 1;

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
            // Use CreateFileMappingW since your argument is a LPCWSTR or it won't compile in ANSI
            handle = CreateFileMapping(
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
            handle = OpenFileMapping(
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

    // RValue reference doesn't make sense here, that says that you'll be destroying the caller's span. Byvalue should be fine.
    void read(std::ptrdiff_t offset, std::span<std::byte> &&buffer)
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

// Unconstrained element is probably a bad idea,
// because you won't be running constructors and
// destructors on both sides, and internal pointers
// will be in the wrong address space.
template<typename Element, std::size_t size>
class RingBuffer
{
public:
    RingBuffer()
        : start(0),
          end(0),
          count(0),
          elements{}
    {
    }

    RingBuffer(RingBuffer&) = delete;
    RingBuffer& operator=(RingBuffer&) = delete;

    ~RingBuffer()
    {
        std::cout << "~RingBuffer" << std::endl;
        for (auto element : elements)
            element.~Element();
    }

    void push(Element &element)
    {        
        assert(count < size);
        ++count;

        std::copy(
            &element,
            &element + 1,
            &elements[end]
        );

        end = (end + 1) % size;
        assert(end != start);
    }

    Element& front()
    {
        return elements[start];
    }

    void pop()
    {
        assert(count > 0);
        --count;

        elements[start].~Element();

        start = (start + 1) % size;
    }

    bool empty() { return count == 0; }

    bool full() { return count == size; }

private:
    // Let's hope that both of your processes have been
    // compiled for the same architecture!
    std::size_t start;
    std::size_t end;
    std::size_t count;
    std::array<Element, size> elements;
};

void producer()
{
    std::cout << "Running as Producer..." << std::endl;

    HANDLE producerDidFinish = CreateEvent(
        NULL,
        FALSE, // manual-reset
        FALSE, // initial state
        PRODUCER_DID_FINISH
    );
    CheckHandle(producerDidFinish);

    HANDLE consumerDidFinish = CreateEvent(
        NULL,
        FALSE, // manual-reset
        FALSE, // initial state
        CONSUMER_DID_FINISH
    );
    CheckHandle(consumerDidFinish);

    HANDLE ringBufferMutex = CreateMutex(
        NULL,
        TRUE, // initially owner
        RING_BUFFER_MUTEX
    );
    CheckHandle(ringBufferMutex);

    SharedMemory memory(SHARED_MEMORY_NAME, SHARED_MEMORY_SIZE, SharedMemory::Mode::CREATE);
    // Tricky design since this has a destructor and the size isn't obviously related to SHARED_MEMORY_SIZE
    auto buffer = new (memory.data()) RingBuffer<int, RING_BUFFER_SIZE>();
    int counter = 0;

    CheckResult(ReleaseMutex(ringBufferMutex));

    while (true)
    {
        CheckResult(WaitForSingleObject(consumerDidFinish, INFINITE));

        // That's cheap! You can't race against anything if you lock the whole ring buffer.
        // The idea was to support concurrent producing/consuming, and I think
        // you will want semaphores for that.
        CheckResult(WaitForSingleObject(ringBufferMutex, INFINITE));

        // Per standard you might never see the new value of elementsPerTick
        // since it is set on another thread. This would have to be an std::atomic.
        for (int n = 0; n < elementsPerTick && !buffer->full(); ++n)
        {
            ++counter;
            buffer->push(counter);
            std::cout << "Sent: " << counter << std::endl;

            // The doesn't matter because of the mutex, there's no concurrency :(
            std::this_thread::sleep_for(std::chrono::seconds(sleepDuration));
        }

        CheckResult(ReleaseMutex(ringBufferMutex));
        CheckResult(SetEvent(producerDidFinish));
    }

    CloseHandle(producerDidFinish);
    CloseHandle(consumerDidFinish);
    CloseHandle(ringBufferMutex);
}

void consumer()
{
    std::cout << "Running as Consumer..." << std::endl;
 
    SharedMemory memory(SHARED_MEMORY_NAME, SHARED_MEMORY_SIZE, SharedMemory::Mode::OPEN);

    auto buffer = static_cast<RingBuffer<int, RING_BUFFER_SIZE>*>(memory.data());

    HANDLE producerDidFinish = OpenEvent(
        SYNCHRONIZE,
        FALSE, // inherit handle
        PRODUCER_DID_FINISH
    );

    HANDLE consumerDidFinish = OpenEvent(
        EVENT_MODIFY_STATE,
        FALSE, // inherit handle
        CONSUMER_DID_FINISH
    );

    HANDLE ringBufferMutex = OpenMutex(
        SYNCHRONIZE,
        FALSE, // inherit handle
        RING_BUFFER_MUTEX
    );

    CheckResult(SetEvent(consumerDidFinish));

    while (true)
    {
        CheckResult(WaitForSingleObject(producerDidFinish, INFINITE));
        CheckResult(WaitForSingleObject(ringBufferMutex, INFINITE));

        for (int n = 0; n < elementsPerTick && !buffer->empty(); ++n) 
        {
            auto next = buffer->front();
            buffer->pop();
            std::cout << "Received: " << next << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(sleepDuration));
        }

        CheckResult(SetEvent(consumerDidFinish));
        CheckResult(ReleaseMutex(ringBufferMutex));
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
        std::cin >> elementsPerTick;
        std::cout << "Changed elements per tick to: " << elementsPerTick << std::endl;
    }
}
