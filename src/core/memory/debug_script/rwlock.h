//---------------------------------------------------------
// For conditions of distribution and use, see
// https://github.com/preshing/cpp11-on-multicore/blob/master/LICENSE
//---------------------------------------------------------

#ifndef __CPP11OM_RWLOCK_H__
#define __CPP11OM_RWLOCK_H__

#include <cassert>
#include <atomic>
#include <random>
#include "sema.h"

//---------------------------------------------------------
// ReadLockGuard
//---------------------------------------------------------
template <class LockType>
class ReadLockGuardT {
private:
    LockType& m_lock;

public:
    ReadLockGuardT(LockType& lock) : m_lock(lock) {
        m_lock.ReadLock();
    }

    ~ReadLockGuardT() {
        m_lock.ReadUnlock();
    }
};

//---------------------------------------------------------
// WriteLockGuard
//---------------------------------------------------------
template <class LockType>
class WriteLockGuardT {
private:
    LockType& m_lock;

public:
    WriteLockGuardT(LockType& lock) : m_lock(lock) {
        m_lock.WriteLock();
    }

    ~WriteLockGuardT() {
        m_lock.WriteUnlock();
    }
};

//---------------------------------------------------------
// ReadWriteLock
//---------------------------------------------------------
class ReadWriteLock
{
public:
    typedef ReadLockGuardT<ReadWriteLock> AutoReadLock;
    typedef WriteLockGuardT<ReadWriteLock> AutoWriteLock;

public:
    ReadWriteLock() : m_status(0) {}

    void ReadLock()
    {
        long long data = m_status.load(std::memory_order_relaxed);
        Status oldStatus;
        Status newStatus;
        oldStatus.data = data;
        do
        {
            newStatus = oldStatus;
            if (oldStatus.writers > 0)
            {
                newStatus.waitToRead++;
            }
            else
            {
                newStatus.readers++;
            }
            // CAS until successful. On failure, oldStatus will be updated with the latest value.
        }
        while (!m_status.compare_exchange_weak(oldStatus.data, newStatus.data,
                                               std::memory_order_acquire, std::memory_order_relaxed));

        if (oldStatus.writers > 0)
        {
            m_readSema.wait();
        }
    }

    void ReadUnlock()
    {
        long long data = m_status.fetch_sub(kReadersOne, std::memory_order_release);
        Status oldStatus;
        oldStatus.data = data;
        assert(oldStatus.readers > 0);
        if (oldStatus.readers == 1 && oldStatus.writers > 0)
        {
            m_writeSema.signal();
        }
    }

    void WriteLock()
    {
        long long data = m_status.fetch_add(kWritersOne, std::memory_order_acquire);
        Status oldStatus;
        oldStatus.data = data;
        assert(oldStatus.writers + 1 <= kMaxWritersCount);
        if (oldStatus.readers > 0 || oldStatus.writers > 0)
        {
            m_writeSema.wait();
        }
    }

    void WriteUnlock()
    {
        long long data = m_status.load(std::memory_order_relaxed);
        Status oldStatus;
        Status newStatus;
        oldStatus.data = data;
        uint32_t waitToRead = 0;
        do
        {
            assert(oldStatus.readers == 0);
            newStatus = oldStatus;
            newStatus.writers--;
            waitToRead = oldStatus.waitToRead;
            if (waitToRead > 0)
            {
                newStatus.waitToRead = 0;
                newStatus.readers = waitToRead;
            }
            // CAS until successful. On failure, oldStatus will be updated with the latest value.
        }
        while (!m_status.compare_exchange_weak(oldStatus.data, newStatus.data,
                                               std::memory_order_release, std::memory_order_relaxed));

        if (waitToRead > 0)
        {
            m_readSema.signal(waitToRead);
        }
        else if (oldStatus.writers > 1)
        {
            m_writeSema.signal();
        }
    }

private:
    enum {
        kAtomicWordBits = sizeof(long long) * 8,

        kReadersBits = (kAtomicWordBits - kAtomicWordBits / 3) / 2,
        kWritersBits = kAtomicWordBits - (kReadersBits * 2),

        kMaxReadersCount = (1 << kReadersBits) - 1,
        kMaxWritersCount = (1 << kWritersBits) - 1
    };
    static_assert(kWritersBits + kReadersBits * 2 == kAtomicWordBits,
                  "ReadWriteLock readers and writers counters should fit long long");

    static const long long kReadersOne = 1;
    static const long long kWritersOne = static_cast<long long>(1) << (kReadersBits + kReadersBits);

    union Status {
        struct {
            long long readers : kReadersBits;
            long long waitToRead : kReadersBits;
            long long writers : kWritersBits;
        };
        long long data;
    };

    std::atomic<long long> m_status;
    DefaultSemaphoreType m_readSema;
    DefaultSemaphoreType m_writeSema;
};

#endif // __CPP11OM_RWLOCK_H__
