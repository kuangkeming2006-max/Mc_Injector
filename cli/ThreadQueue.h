#pragma once

#include <windows.h>

#include <deque>
#include <mutex>
#include <utility>

namespace cli {

// Small thread-safe queue with a waitable HANDLE so the console main loop can
// integrate producers (stdin reader, agent pipe reader, HTTP workers) through
// MsgWaitForMultipleObjectsEx. push() signals the event; pop() drains one
// item and resets the event only while holding the queue lock, which keeps
// the event-set/event-reset sequence race-free.
template <typename T>
class WaitQueue
{
public:
    WaitQueue() : m_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~WaitQueue()
    {
        if (m_event != nullptr)
            ::CloseHandle(m_event);
    }

    WaitQueue(const WaitQueue &) = delete;
    WaitQueue &operator=(const WaitQueue &) = delete;

    [[nodiscard]] HANDLE event() const noexcept { return m_event; }

    void push(T item)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_items.push_back(std::move(item));
        }
        ::SetEvent(m_event);
    }

    // Returns false when the queue was empty. The event is reset only under
    // the lock after an empty queue is observed, so a concurrent push that
    // lands after the check still re-signals it.
    bool pop(T &item)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_items.empty()) {
            ::ResetEvent(m_event);
            return false;
        }
        item = std::move(m_items.front());
        m_items.pop_front();
        if (m_items.empty())
            ::ResetEvent(m_event);
        return true;
    }

private:
    std::deque<T> m_items;
    std::mutex m_mutex;
    HANDLE m_event = nullptr;
};

} // namespace cli
