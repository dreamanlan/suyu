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
        m_lock.lockReader();
    }

    ~ReadLockGuardT() {
        m_lock.unlockReader();
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
        m_lock.lockWriter();
    }

    ~WriteLockGuardT() {
        m_lock.unlockWriter();
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

    void lockReader()
    {
        Status oldStatus = m_status.load(std::memory_order_relaxed);
        Status newStatus;
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
        while (!m_status.compare_exchange_weak(oldStatus, newStatus,
                                               std::memory_order_acquire, std::memory_order_relaxed));

        if (oldStatus.writers > 0)
        {
            m_readSema.wait();
        }
    }

    void unlockReader()
    {
        Status oldStatus = m_status.fetch_sub(1, std::memory_order_release);
        assert(oldStatus.readers > 0);
        if (oldStatus.readers == 1 && oldStatus.writers > 0)
        {
            m_writeSema.signal();
        }
    }

    void lockWriter()
    {
        Status oldStatus = m_status.fetch_add(1, std::memory_order_acquire);
        assert(oldStatus.writers + 1 <= kMaxWritersCount);
        if (oldStatus.readers > 0 || oldStatus.writers > 0)
        {
            m_writeSema.wait();
        }
    }

    void unlockWriter()
    {
        Status oldStatus = m_status.load(std::memory_order_relaxed);
        Status newStatus;
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
        while (!m_status.compare_exchange_weak(oldStatus, newStatus,
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

    union Status {
        struct {
            long long readers : kReadersBits;
            long long waitingReaders : kReadersBits;
            long long writers : kWritersBits;
        };
        long long data;
    };

    std::atomic<long long> m_status;
    DefaultSemaphoreType m_readSema;
    DefaultSemaphoreType m_writeSema;
};

#endif // __CPP11OM_RWLOCK_H__
