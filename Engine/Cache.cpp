/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Cache.h"

#include <cassert>
#include <stdexcept>
#include <set>
#include <list>

#ifdef __NATRON_UNIX__
#include <time.h>
#endif

#include <QMutex>
#include <QDir>
#include <QWaitCondition>
#include <QDebug>
#include <QReadWriteLock>

GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
GCC_DIAG_OFF(unused-parameter)
#include <boost/unordered_set.hpp>
#include <boost/format.hpp>
#include <boost/interprocess/containers/vector.hpp>
#include <boost/interprocess/containers/map.hpp>

// http://www.boost.org/doc/libs/1_59_0/doc/html/container/non_standard_containers.html#container.non_standard_containers.flat_xxx
#include <boost/interprocess/containers/flat_map.hpp>


#include <boost/interprocess/sync/interprocess_mutex.hpp> // IPC regular mutex
#include <boost/interprocess/sync/interprocess_recursive_mutex.hpp> // IPC recursive mutex
#include <boost/interprocess/sync/scoped_lock.hpp> // IPC  scoped lock a regular mutex
#include <boost/interprocess/sync/interprocess_upgradable_mutex.hpp> // IPC  r-w mutex that can upgrade read right to write
#include <boost/interprocess/sync/interprocess_sharable_mutex.hpp> // IPC  r-w mutex
#include <boost/interprocess/sync/sharable_lock.hpp> // IPC  scoped lock a r-w mutex
#include <boost/interprocess/sync/upgradable_lock.hpp> // IPC  scope lock a r-w upgradable mutex
#include <boost/interprocess/sync/interprocess_condition_any.hpp> // IPC  wait cond with a r-w mutex
#include <boost/interprocess/sync/file_lock.hpp> // IPC  file lock
#include <boost/interprocess/sync/named_semaphore.hpp> // IPC  named semaphore
#include <boost/thread/mutex.hpp> // local mutex
#include <boost/thread/recursive_mutex.hpp> // local mutex
#include <boost/thread/shared_mutex.hpp> // local r-w mutex
#include <boost/thread/locks.hpp>
#include <boost/thread/condition_variable.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)


#include <SequenceParsing.h>

#include "Global/GlobalDefines.h"
#include "Global/StrUtils.h"
#include "Global/QtCompat.h"

#include "Engine/AppManager.h"
#include "Engine/StorageDeleterThread.h"
#include "Global/FStreamsSupport.h"
#include "Engine/MemoryFile.h"
#include "Engine/MemoryInfo.h"
#include "Engine/Settings.h"
#include "Engine/StandardPaths.h"
#include "Engine/RamBuffer.h"
#include "Engine/Timer.h"
#include "Engine/ThreadPool.h"


// The number of buckets. This must be a power of 16 since the buckets will be identified by a digit of a hash
// which is an hexadecimal number.
#define NATRON_CACHE_BUCKETS_N_DIGITS 2
#define NATRON_CACHE_BUCKETS_COUNT 256

// Grow the bucket ToC shared memory by 512Kb at once
#define NATRON_CACHE_BUCKET_TOC_FILE_GROW_N_BYTES 524288 // = 512 * 1024

// Used to prevent loading older caches when we change the serialization scheme
#define NATRON_CACHE_SERIALIZATION_VERSION 5

// If we change the MemorySegmentEntryHeader struct, we must increment this version so we do not attempt to read an invalid structure.
#define NATRON_MEMORY_SEGMENT_ENTRY_HEADER_VERSION 1


// After this amount of milliseconds, if a thread is not able to access a mutex, the cache is assumed to be inconsistent
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
#define NATRON_CACHE_INTERPROCESS_MUTEX_TIMEOUT_MS 10000
#endif

// Each file is 1GB
// Note that we don't split the free tiles list across each buckets, otherwise 1 bucket could have only 256 tiles available per 1GiB of memory.
// But a single HD image could already take up a full bucket worth of tiles, hence we group the free tiles list of all buckets into the free tiles list
// of the first bucket.
#define NATRON_NUM_TILES_PER_BUCKET_FILE 256
#define NATRON_NUM_TILES_PER_FILE (NATRON_NUM_TILES_PER_BUCKET_FILE * NATRON_CACHE_BUCKETS_COUNT)
#define NATRON_TILE_STORAGE_FILE_SIZE (NATRON_TILE_SIZE_BYTES * NATRON_NUM_TILES_PER_FILE)

//#define CACHE_TRACE_ENTRY_ACCESS
//#define CACHE_TRACE_TIMEOUTS
//#define CACHE_TRACE_FILE_MAPPING
//#define CACHE_TRACE_TILES_ALLOCATION

namespace bip = boost::interprocess;

#define kNatronIPCPropertyHash "NatronIPCPropertyHash"


NATRON_NAMESPACE_ENTER;



// Cache integrity when NATRON_CACHE_INTERPROCESS_ROBUST is defined:
// ------------------------------------------------------------------
//
// Exposing the cache to multiple process can be harmful in multiple ways: a process can die
// in any instruction and may leave the program in an incoherent state. Other processes have to deal
// with that. Hopefully this kind of situation is rare.
// E.G:
// A Natron process could very well crash whilst an interprocess mutex is taken: any subsequent attempt to lock
// the mutex would deadlock because of an abandonned mutex.
//
// Databases generally overcome this issue by using a file lock instead of a named mutex.
// The file lock has the interesting property that it lives as long as the process lives:
// From: http://www.boost.org/doc/libs/1_63_0/doc/html/interprocess/synchronization_mechanisms.html#interprocess.synchronization_mechanisms.file_lock.file_lock_whats_a_file_lock
/*A file locking is a class that has process lifetime.
 This means that if a process holding a file lock ends or crashes,
 the operating system will automatically unlock it.
 This feature is very useful in some situations where we want to assure
 automatic unlocking even when the process crashes and avoid leaving blocked resources in the system.
 */
//
// Databases generally also use a journal to log each action operated on the database and rollback
// the journal if in an incoherent state.
//
// In our case we have a cache split up in 256 buckets to lower concurrent accesses.
// That means 256 mutex: one for each bucket.
//
// Since the cache is accessed largely over thousands of times per second, using I/O based solution
// to ensure thread/process safety of the cache is probably overkill thus we did not explore this solution.
// However if in the future we want it to be shared accross a network, we would need to
// fallback on the file lock solution.
//
// Instead we use 256 interprocess mutex, all of them embedded in a single shared memory segment.
// The trick is then to detect any abandonned mutex and to recover from it.
// Instead of locking a mutex and wait until it is obtained, any mutex in the cache is taken by doing a
// timed lock: after a certain amount of time, if the lock could not be taken, we assume the mutex was
// abandonned.
//
// In a situation of abandonnement, we cannot assume any state on the cache, thus we wipe it and recreate it.
// Since we don't hold any information as precious as a database would, we are safe to do so anyway.
//
// Algorithm to detect and recover from abandonnement in a inter process cache:
//
// In addition to the 256 interprocess mutex, we add a global file lock to monitor process access to the cache.
//
// When starting up a new Natron process: globalFileLock.try_lock()
//      - If it succeeds, that means no other process is active: We remove the globalMemorySegment shared memory segment
//        and create a new one, to ensure no lock was left in a bad state. Then we release the file lock
//      - If it fails, another process is still actively using the globalMemorySegment shared memory: it must still be valid
//
// We then take the file lock in read mode, indicating that we use the shared memory:
//      globalFileLock.lock_sharable()
//
// Any operation taking the segmentMutex in the shared memory, must do so with a timeout so we can avoid deadlocks:
// If a process crashes whilst the segmentMutex is taken, the file lock is ensured to be released but the
// segmentMutex will remain taken, deadlocking any other process.
//
// To overcome the deadlock, we add 2 named semaphores (nSHMValid, nSHMInvalid).
//
// If the segmentMutex times out, we apply the following steps:
//
// 0 - Lock the nThreadsTimedOutFailedMutex mutex to ensure only 1 thread in this process
//     does the following operations.
//     ++nThreadsTimedOutFailed
//     If nThreadsTimedOutFailed == 1, do steps 1 to 9 (included), otherwise
//     skip directly to step 10
// 1 - unmap the globalMemorySegment shared memory
//
// 2 - nSHMInvalid.post() --> The mapping for this process is no longer invalid
//
// 3 - We release the read lock taken on the globalFileLock: globalFileLock.unlock()
//
// 4 - We take the file lock in write mode: globalFileLock.lock():
//   The lock is guaranteed to be taken at some point since any active process will eventually timeout on the segmentMutex and release
//   their read lock on the globalFileLock in step 3. We are sure that when the lock is taken, nobody else is still in step 3.
//
//  Now that we have the file lock in write mode, we may not be the first process to have it:
//     5 -  nSHMValid.try_wait() --> If this returns false, we are the first process to take the write lock.
//                               We know at this point that any other process has released its read lock on the globalFileLock
//                               and that the globalMemorySegment is no longer mapped anywhere.
//                               We thus remove the globalMemorySegment and re-create it and remap it.
//
//                           --> If this returns true, we are not the first process to take the write lock, hence the globalMemorySegment
//                               has been re-created already, so just map it.
//
//      6 - nSHMValid.post() --> Indicate that we mapped the shared memory segment
//
//      7 - nSHMInvalid.wait() --> Decrement the post() that we made earlier
//
//      8 - Release the write lock: globalFileLock.unlock()
//
// 9 - When the write lock is released we cannot take the globalFileLock in read mode yet, we could block other processes that
// are still waiting for the write lock in 4.
// We must wait that every other process has a valid mapping:
//
//
//      while(nSHMInvalid.try_wait()) {
//          nSHMInvalid.post()
//      }
//
//  nSHMInvalid.try_wait() will return false when all processes have been remapped.
//  If it returns true, that means another process is still in-between steps 4 and 8, thus we post
//  what we decremented in try_wait and re-try again.
//
// 10 - Now wait that all timed out threads are finished
//      --nThreadsTimedOutFailed
//      while(nThreadsTimedOutFailed > 0) {
//          nThreadsTimedOutFailedCond.wait(&nThreadsTimedOutFailedMutex)
//      }


// A process local storage holder
typedef RamBuffer<char> ProcessLocalBuffer;
typedef boost::shared_ptr<ProcessLocalBuffer> ProcessLocalBufferPtr;

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST

class SharedMemoryProcessLocalReadLocker;

/**
 * @brief Implementation of the timed_lock which timesout after timeoutMilliseconds milliseconds.
 * The implementation is taken from boost, but we repliced the micro_clock::universal_time() by our 
 * own timestamps which bypass gmtime_r on unix systems which is horribly slow.
 **/
template <class Mutex, void(Mutex::*lock_func)(), bool(Mutex::*try_lock_func) ()>
bool timed_lock_impl(Mutex* m, std::size_t timeoutMilliseconds, double frequency)
{
    //Same as lock()
    if (timeoutMilliseconds == 0){
        (m->*lock_func)();
        return true;
    }
    //Always try to lock to achieve POSIX guarantees:
    // "Under no circumstance shall the function fail with a timeout if the mutex
    //  can be locked immediately. The validity of the abs_timeout parameter need not
    //  be checked if the mutex can be locked immediately."
    else if((m->*try_lock_func)()) {
        return true;
    } else {

        TimestampVal startTime = getTimestampInSeconds();
        TimestampVal now = startTime;

        double timeElapsedMS = 0.;
        do {
            if ((m->*try_lock_func)()) {
                return true;
            }
            timeElapsedMS = getTimeElapsed(startTime, now, frequency) * 1000.;
        } while(timeElapsedMS < timeoutMilliseconds);
    }
    return false;
} // timed_lock_impl

/**
 * @brief A base class for all scoped timed locks
 **/
template <class Mutex, void(Mutex::*lock_func)(), bool(Mutex::*try_lock_func) (), void(Mutex::*unlock_func)()>
class scoped_timed_lock_impl
{
public:

    typedef Mutex mutex_type;

    scoped_timed_lock_impl()
    : mp_mutex(0), m_locked(false), m_frequency(1.)
    {

    }

    // The mutex is not locked in the ctor
    scoped_timed_lock_impl(mutex_type& m, double frequency)
    : mp_mutex(&m), m_locked(false), m_frequency(frequency)
    {}

    // Unlocks the mutex if locked
    ~scoped_timed_lock_impl()
    {
        try{  if(m_locked && mp_mutex)   (mp_mutex->*unlock_func)();  }
        catch(...){}
    }

    //!Effects: If mutex() == 0 or if already locked, throws a lock_exception()
    //!   exception. Calls lock() on the referenced mutex.
    //!Postconditions: owns() == true.
    //!Notes: The scoped_lock changes from a state of not owning the mutex, to
    //!   owning the mutex, blocking if necessary.
    void lock()
    {
        assert(mp_mutex && !m_locked);
        mp_mutex->lock();
        m_locked = true;
    }

    // Try to take the lock and bail out if it failed to do
    // so after timeoutMilliseconds milliseconds.
    // If timeoutMilliseconds is 0, this is the same as lock()
    bool timed_lock(std::size_t timeoutMilliseconds = NATRON_CACHE_INTERPROCESS_MUTEX_TIMEOUT_MS)
    {
        assert(mp_mutex && !m_locked);
        m_locked = timed_lock_impl<Mutex, lock_func, try_lock_func>(mp_mutex, timeoutMilliseconds, m_frequency);
        return m_locked;
    }

    void unlock()
    {
        assert(mp_mutex && m_locked);
        mp_mutex->unlock();
        m_locked = false;
    }

    bool owns() const
    {
        return m_locked && mp_mutex;
    }

    operator bool() const
    {  return m_locked;   }


    mutex_type* mutex() const
    {  return  mp_mutex;  }

    mutex_type* release()
    {
        mutex_type *mut = mp_mutex;
        mp_mutex = 0;
        m_locked = false;
        return mut;
    }

protected:
    mutex_type *mp_mutex;
    bool m_locked;
    double m_frequency;
};

/**
 * @brief Base class for all locks that can be shared (read lock)
 **/
template <class Mutex>
class scoped_timed_sharable_lock : public scoped_timed_lock_impl<Mutex, &Mutex::lock_sharable, &Mutex::try_lock_sharable, &Mutex::unlock_sharable>
{
public:
    scoped_timed_sharable_lock(Mutex& m, double frequency)
    : scoped_timed_lock_impl<Mutex, &Mutex::lock_sharable, &Mutex::try_lock_sharable, &Mutex::unlock_sharable>(m, frequency)
    {

    }
};

typedef scoped_timed_sharable_lock<bip::interprocess_upgradable_mutex> Upgradable_ReadLock;
typedef scoped_timed_sharable_lock<bip::interprocess_sharable_mutex> Sharable_ReadLock;

/**
 * @brief Base class for all locks that can be upgraded (read lock)
 **/
class UpgradableLock : public scoped_timed_lock_impl<bip::interprocess_upgradable_mutex, &bip::interprocess_upgradable_mutex::lock_upgradable, &bip::interprocess_upgradable_mutex::try_lock_upgradable, &bip::interprocess_upgradable_mutex::unlock_upgradable>
{

    BOOST_MOVABLE_BUT_NOT_COPYABLE(UpgradableLock)


public:
    UpgradableLock(bip::interprocess_upgradable_mutex& m, double frequency)
    : scoped_timed_lock_impl<bip::interprocess_upgradable_mutex, &bip::interprocess_upgradable_mutex::lock_upgradable, &bip::interprocess_upgradable_mutex::try_lock_upgradable, &bip::interprocess_upgradable_mutex::unlock_upgradable>(m, frequency)
    {

    }
};


/**
 * @brief Base class for all locks that are exclusive
 **/
template <class Mutex>
class scoped_timed_lock : public scoped_timed_lock_impl<Mutex, &Mutex::lock, &Mutex::try_lock, &Mutex::unlock>
{


public:

    scoped_timed_lock()
    : scoped_timed_lock_impl<Mutex, &Mutex::lock, &Mutex::try_lock, &Mutex::unlock>()
    {
    }

    scoped_timed_lock(Mutex& m, double frequency)
    : scoped_timed_lock_impl<Mutex, &Mutex::lock, &Mutex::try_lock, &Mutex::unlock>(m, frequency)
    {
    }
};

typedef bip::interprocess_sharable_mutex SharedMutex;
typedef bip::interprocess_upgradable_mutex UpgradableMutex;
typedef bip::interprocess_mutex ExclusiveMutex;
typedef bip::interprocess_recursive_mutex RecursiveExclusiveMutex;

typedef scoped_timed_lock<UpgradableMutex> Upgradable_WriteLock;
typedef scoped_timed_lock<SharedMutex> Sharable_WriteLock;
typedef scoped_timed_lock<ExclusiveMutex> ExclusiveLock;

typedef bip::interprocess_condition_any ConditionVariable;

#define scoped_lock_type scoped_timed_lock


/**
 * @brief A kind of scoped_timed_lock that is constructed from an upgradable lock
 **/
class scoped_upgraded_lock : public scoped_timed_lock<bip::interprocess_upgradable_mutex>
{
public:


    typedef bip::interprocess_upgradable_mutex mutex_type;


    //!Effects: If upgr.owns() then calls unlock_upgradable_and_lock() on the
    //!   referenced mutex. upgr.release() is called.
    //!Postconditions: mutex() == the value upgr.mutex() had before the construction.
    //!   upgr.mutex() == 0. owns() == upgr.owns() before the construction.
    //!   upgr.owns() == false after the construction.
    //!Notes: If upgr is locked, this constructor will lock this scoped_lock while
    //!   unlocking upgr. If upgr is unlocked, then this scoped_lock will be
    //!   unlocked as well. Only a moved upgradable_lock's will match this
    //!   signature. An non-moved upgradable_lock can be moved with
    //!   the expression: "boost::move(lock);" This constructor may block if
    //!   other threads hold a sharable_lock on this mutex (sharable_lock's can
    //!   share ownership with an upgradable_lock).
    explicit scoped_upgraded_lock(BOOST_RV_REF(UpgradableLock) upgr)
    : scoped_timed_lock<bip::interprocess_upgradable_mutex>()
    {
        UpgradableLock &u_lock = upgr;
        if (u_lock.owns()) {
            u_lock.mutex()->unlock_upgradable_and_lock();
            m_locked = true;
        }
        mp_mutex = u_lock.release();
    }
};

class SharedMemoryProcessLocalReadLocker;
typedef boost::scoped_ptr<SharedMemoryProcessLocalReadLocker> SHMReadLockerPtr;


#else // !NATRON_CACHE_INTERPROCESS_ROBUST

typedef boost::shared_mutex SharedMutex;
typedef boost::upgrade_mutex UpgradableMutex;
typedef boost::mutex ExclusiveMutex;
typedef boost::recursive_mutex RecursiveExclusiveMutex;

typedef boost::shared_lock<SharedMutex> Sharable_ReadLock;
typedef boost::shared_lock<UpgradableMutex> Upgradable_ReadLock;
typedef boost::upgrade_lock<UpgradableMutex> UpgradableLock;

typedef boost::unique_lock<UpgradableMutex> scoped_upgraded_lock;
typedef boost::unique_lock<UpgradableMutex> Upgradable_WriteLock;
typedef boost::unique_lock<SharedMutex> Sharable_WriteLock;
typedef boost::unique_lock<ExclusiveMutex> ExclusiveLock;

typedef boost::condition_variable_any ConditionVariable;

#define scoped_lock_type boost::unique_lock

#endif // NATRON_CACHE_INTERPROCESS_ROBUST


/**
 * @brief An exception thrown when a mutex used in the cache implementation is abandonned
 **/
class AbandonnedLockException : public std::exception
{

public:

    AbandonnedLockException()
    {
    }

    virtual ~AbandonnedLockException() throw()
    {
    }

    virtual const char * what () const throw ()
    {
        return "Abandonned lock!";
    }
};

/**
 * @brief An exception thrown when the cache is detected to be inconsistent
 **/
class CorruptedCacheException : public std::exception
{

public:

    CorruptedCacheException()
    {
    }

    virtual ~CorruptedCacheException() throw()
    {
    }

    virtual const char * what () const throw ()
    {
        return "Corrupted cache";
    }
};

// Maintain the lru with a list of hash: more recents hash are inserted at the end of the list
// The least recently used hash is the first item of the list.

/**
 * @brief A node of the linked list used to implement the LRU.
 * We need a custom list here, because we want to be able to hold 
 * an offset_ptr of a node directly inside a MemorySegmentEntry
 * for efficiency.
 **/
struct PersistentMemorySegmentEntryHeader;
struct NonPersistentMemorySegmentEntryHeader;
struct LRUListNode;


typedef bip::offset_ptr<LRUListNode> LRUListNodePtr;

struct LRUListNode
{
    LRUListNodePtr prev, next;
    U64 hash;

    LRUListNode()
    : prev(0)
    , next(0)
    , hash(0)
    {

    }

};


// Typedef our interprocess types
typedef bip::allocator<U64, ExternalSegmentType::segment_manager> U64_Allocator_ExternalSegment;

// The unordered set of free tiles indices in a bucket
typedef bip::set<U64, std::less<U64>, U64_Allocator_ExternalSegment> U64_Set;

typedef boost::interprocess::allocator<U64, ExternalSegmentType::segment_manager> ExternalSegmentTypeULongLongAllocator;
typedef boost::interprocess::list<U64, ExternalSegmentTypeULongLongAllocator> ExternalSegmentTypeULongLongList;


LRUListNode* getRawPointer(LRUListNodePtr& ptr)
{
    return ptr.get();
}

inline
void disconnectLinkedListNode(const LRUListNodePtr& node)
{
    // Remove from the LRU linked list:

    // Make the previous item successor point to this item successor
    if (node->prev) {
        node->prev->next = node->next;
    }

    node->prev = 0;


    // Make the next item predecessor point to this item predecessor
    if (node->next) {
        node->next->prev = node->prev;
    }
    node->next = 0;
}

inline
void insertLinkedListNode(const LRUListNodePtr& node, const LRUListNodePtr& prev, const LRUListNodePtr& next)
{
    assert(node);
    if (prev) {
        prev->next = node;
        assert(prev->next);
    }
    node->prev = prev;

    if (next) {
        next->prev = node;
        assert(next->prev);
    }
    node->next = next;
}

/**
 * @brief This struct represents the minimum required data for a cache entry in the global bucket memory segment.
 * It is associated to a hash in the LRU linked list.
 * This struct lives in the ToC memory mapped file
 **/
struct MemorySegmentEntryHeaderBase
{
    // The size of the memorySegmentPortion, in bytes. This is stored in the main cache memory segment.
    U64 size;

    enum EntryStatusEnum
    {
        // The entry is ready (i.e: it was computed) and can be safely retrieved by other processes/threads
        eEntryStatusReady,

        // The entry is in the main memory segment of the cache bucket but has not been computed and not thread
        // is still computing it.
        eEntryStatusNull,

        // The entry is in the main memory segment of the cache bucket but still being computed by a thread
        // The caller should wait in statusCond before it gets ready.
        eEntryStatusPending
    };

    // The status of the entry
    EntryStatusEnum status;

    // A magic number identifying the thread that is computing the entry.
    // This enables to detect immediate recursion in case a thread is computing an entry
    // but is trying to access the cache again for the same entry in the meantime.
    U64 computeThreadMagic;

    // The corresponding node in the LRU list
    LRUListNode lruNode;

    // List of tile indices allocated for this entry
    ExternalSegmentTypeULongLongList tileIndices;

    MemorySegmentEntryHeaderBase(const void_allocator& allocator)
    : size(0)
    , status(eEntryStatusNull)
    , computeThreadMagic(0)
    , lruNode()
    , tileIndices(allocator)
    {}

};

template <bool persistent>
struct MemorySegmentEntryHeader : public MemorySegmentEntryHeaderBase {};

template <>
struct MemorySegmentEntryHeader<true> : public MemorySegmentEntryHeaderBase
{

    typedef MemoryFile StorageType;

    // The ID of the plug-in holding this entry
    String_ExternalSegment pluginID;

    // Serialized data from the derived class of CacheEntryBase
    IPCPropertyMap properties;

    MemorySegmentEntryHeader(const void_allocator& allocator)
    : MemorySegmentEntryHeaderBase(allocator)
    , pluginID(allocator)
    , properties(allocator)
    {

    }

};

template <>
struct MemorySegmentEntryHeader<false> : public MemorySegmentEntryHeaderBase
{
    typedef ProcessLocalBuffer StorageType;

    // The ID of the plug-in holding this entry
    std::string pluginID;

    // When not persistent, just hold a pointer to the process local entry
    CacheEntryBasePtr nonPersistentEntry;

    MemorySegmentEntryHeader(const void_allocator& allocator)
    : MemorySegmentEntryHeaderBase(allocator)
    , nonPersistentEntry()
    {

    }

};

/**
 * @brief An enum indicating the state of the bucket. This enables corrupted cache detection in case
 * NATRON_CACHE_INTERPROCESS_ROBUST is not defined. If NATRON_CACHE_INTERPROCESS_ROBUST is defined,
 * the inconsistency of the cache is detected by a mutex timeout.
 **/
enum BucketStateEnum
{
    // Nothing is happening currently, we can safely operate
    eBucketStateOk,

    // An operation is under progress, when entering a public function
    // if we find this status, this means the bucket is inconsistent.
    eBucketStateInconsistent
};



template <int level>
int getBucketStorageIndex(U64 hash)
{
    // The 64 bit hash is composed of 16 hexadecimal digits, each of them spanning 4 bits.
    // A bucket "level" is composed of 2 hexa decimal digits, hence 2 * 4 = 8 bits

    // First clear the mask digits on the left with a zero-fill right shift
    U64 mask = 0xffffffffffffffff >> NATRON_CACHE_BUCKETS_N_DIGITS * level * 4;
    U64 index = hash & mask;

    // Now right shift by a multiple of the level offset to get the index such as 0 <= index <= 255
    index >>= (64 - NATRON_CACHE_BUCKETS_N_DIGITS * (level + 1) * 4);
    assert(index >= 0 && index < NATRON_CACHE_BUCKETS_COUNT);
    return index;
}



/**
 * @brief All IPC data that are shared accross processes for this bucket. This object lives in the ToC memory mapped file.
 **/
template <bool persistent>
struct CacheBucketIPCData
{
    typedef MemorySegmentEntryHeader<persistent> EntryType;
    typedef bip::offset_ptr<EntryType> EntryTypePtr;
    typedef std::pair<const U64, EntryTypePtr > EntriesMapValueType;
    typedef bip::allocator<EntriesMapValueType, ExternalSegmentType::segment_manager> EntriesMapValueType_Allocator;
    typedef bip::map<U64, EntryTypePtr, std::less<U64>, EntriesMapValueType_Allocator> EntriesMap;

    // Pointers in shared memory to the lru list from node and back node
    // Protected by lruListMutex
    LRUListNodePtr lruListFront, lruListBack;

    // A version indicator for the serialization. If the cache version doesn't correspond
    // to NATRON_MEMORY_SEGMENT_ENTRY_HEADER_VERSION, we wipe it.
    // Never changes, thread-safe
    unsigned int version;

    // What operation is done on the bucket. When obtaining a write lock on the bucket,
    // if the state is other than eBucketStateOk we detected an inconsistency.
    // The bucket state is protected by the bucketMutex
    BucketStateEnum bucketState;

    // The number of bytes taken by the bucket
    // Protected by bucketMutex
    std::size_t size;

    // The entries storage, accessed directly by the hash bits
    // Protected by bucketMutex
    EntriesMap entriesMap;

    // Indices of the chunks of memory available in the tileAligned memory-mapped file.
    // Protected by the bucketMutex
    //
    U64_Set freeTiles;

    CacheBucketIPCData(const void_allocator& allocator)
    : lruListFront(0)
    , lruListBack(0)
    , version(NATRON_MEMORY_SEGMENT_ENTRY_HEADER_VERSION)
    , bucketState(eBucketStateOk)
    , size(0)
    , entriesMap(allocator)
    , freeTiles(allocator)
    {
    }

    CacheBucketIPCData()
    : lruListFront(0)
    , lruListBack(0)
    , version(NATRON_MEMORY_SEGMENT_ENTRY_HEADER_VERSION)
    , bucketState(eBucketStateOk)
    , size(0)
    , entriesMap()
    , freeTiles()
    {
    }

};


/**
 * @brief The cache is split up into 256 buckets. Each bucket is identified by 2 hexadecimal digits (16*16)
 * This allows concurrent access to the cache without taking a lock: threads are less likely to access to the same bucket.
 * We could also split into 4096 buckets but that's too many data structures to allocate to be worth it.
 *
 * The cache bucket is implemented using interprocess safe data structures so that it can be shared across Natron processes.
 **/
template <bool persistent>
struct CacheBucket
{
    typedef CacheBucketIPCData<persistent> IPCData;
    typedef typename IPCData::EntryType EntryType;
    typedef typename IPCData::EntryTypePtr EntryTypePtr;
    typedef typename IPCData::EntriesMapValueType EntriesMapValueType;
    typedef typename IPCData::EntriesMap EntriesMap;
    typedef typename EntryType::StorageType StorageType;
    typedef boost::shared_ptr<StorageType> StoragePtrType;

    // Weak pointer to the cache
    boost::weak_ptr<Cache<persistent> > cache;

    // A memory manager of the tocFile. It is only valid when the tocFile is memory mapped.
    boost::shared_ptr<ExternalSegmentType> tocFileManager;

    // The index of this bucket in the cache
    int bucketIndex;

    // Memory mapped file used to store interprocess table of contents (IPCData)
    // It contains for each entry:
    // - A LRUListNode
    // - A MemorySegmentEntry
    // - A memory buffer of arbitrary size
    // Any access to the file should be protected by the tocData.segmentMutex mutex located in
    // CacheIPCData::PerBucketData
    // This is only valid if the cache is persistent
    StoragePtrType tocFile;

    // Pointer to the IPC data that live in tocFile memory mapped file. This is valid
    // as long as tocFile is mapped
    IPCData *ipc;

    CacheBucket()
    : cache()
    , tocFileManager()
    , bucketIndex(-1)
    , tocFile()
    , ipc(0)
    {

    }

    /**
     * @brief Deallocates the cache entry pointed to by cacheEntryIt from the ToC memory mapped file.
     * This function assumes that tocData.segmentMutex must be taken in write mode
     * This function may take the tileData.segmentMutex in write mode.
     * @param cacheEntryIt A valid iterator pointing to the entry. It will be invalidated when returning from the function.
     * @param storage A pointer to the map containing the cacheEntryIt iterator.
     *
     * This function may throw a AbandonnedLockException
     **/
    void deallocateCacheEntryImpl(typename EntriesMap::iterator cacheEntryIt,
                                  EntriesMap* storage);

    /**
     * @brief Lookup the cache for a MemorySegmentEntry matching the hash key.
     * If found, the cacheEntry member will be set.
     * This function assumes that the tocData.segmentMutex is taken at least in read mode.
     **/
    bool tryCacheLookupImpl(U64 hash, typename EntriesMap::iterator* found, EntriesMap** storage);

    enum ShmEntryReadRetCodeEnum
    {
        eShmEntryReadRetCodeOk,
        eShmEntryReadRetCodeDeserializationFailed,
        eShmEntryReadRetCodeOutOfMemory,
        eShmEntryReadRetCodeNeedWriteLock,
    };

    /**
     * @brief Reads the cacheEntry into the processLocalEntry.
     * This function updates the status member.
     * This function assumes that the bucketLock of the bucket is taken at least in read mode.
     * @returns True if ok, false if the MemorySegmentEntry cannot be read properly.
     * it should be deallocated from the segment.
     *
     * This function may throw a AbandonnedLockException
     **/
    ShmEntryReadRetCodeEnum readFromSharedMemoryEntryImpl(EntryType* entry,
                                                          const CacheEntryBasePtr& processLocalEntry,
                                                          U64 hash,
                                                          bool hasWriteRights);

    ShmEntryReadRetCodeEnum deserializeEntry(EntryType* entry, const CacheEntryBasePtr& processLocalEntry, U64 hash, bool hasWriteRights);

    void checkToCMemorySegmentStatus(boost::scoped_ptr<Sharable_ReadLock>* tocReadLock,
                                     boost::scoped_ptr<Sharable_WriteLock>* tocWriteLock);

    /**
     * @brief Returns whether the ToC memory mapped file mapping is still valid.
     * The tocData.segmentMutex is assumed to be taken for read-lock
     **/
    bool isToCFileMappingValid() const;

    /**
     * @brief Ensures that the ToC memory mapped file mapping is still valid and re-open it if not.
     * @param tocFileLock The tocData.segmentMutex is assumed to be taken for write-lock: this is the lock currently taken
     * @param minFreeSize Indicates that the file should have at least this amount of free bytes.
     * If not, this function will call growTileFile.
     * If the file is empty and minFreeSize is 0, the file will at least be grown to a size of
     * NATRON_CACHE_BUCKET_TOC_FILE_GROW_N_BYTES
     **/
    void remapToCMemoryFile(Sharable_WriteLock& tocFileLock, std::size_t minFreeSize);

    /**
     * @brief Grow the ToC memory mapped file.
     * This will first wait all other processes accessing to the mapping before resizing.
     * Any process trying to access the mapping during resizing will wait.
     * @param tocFileLock The tocData.segmentMutex is assumed to be taken for write-lock: this is the lock currently taken
     *
     * This function is called internally by remapToCMemoryFile()
     **/
    void growToCFile(Sharable_WriteLock& tocFileLock, std::size_t bytesToAdd);

};


template <bool persistent>
struct CacheEntryLockerPrivate
{

    typedef typename CacheBucket<persistent>::EntryType EntryType;
    typedef typename CacheBucket<persistent>::EntriesMapValueType EntriesMapValueType;
    typedef typename CacheBucket<persistent>::EntryTypePtr EntryTypePtr;
    typedef typename CacheBucket<persistent>::StoragePtrType StoragePtrType;
    typedef typename CacheBucket<persistent>::EntriesMap EntriesMap;

    // Raw pointer to the public interface: lives in process memory
    CacheEntryLocker<persistent>* _publicInterface;

    // A smart pointer to the cache: lives in process memory
    boost::shared_ptr<Cache<persistent> > cache;

    // A pointer to the entry to retrieve: lives in process memory
    CacheEntryBasePtr processLocalEntry;

    // The hash of the entry
    U64 hash;

    // Holding a pointer to the bucket is safe since they are statically allocated on the cache.
    CacheBucket<persistent>* bucket;

    // The status of the entry, @see CacheEntryStatusEnum
    CacheEntryLockerBase::CacheEntryStatusEnum status;

    CacheEntryLockerPrivate(CacheEntryLocker<persistent>* publicInterface, const boost::shared_ptr<Cache<persistent> >& cache, const CacheEntryBasePtr& entry);

    // This function may throw a AbandonnedLockException
    enum LookUpRetCodeEnum
    {
        eLookUpRetCodeFound,
        eLookUpRetCodeNotFound,
        eLookUpRetCodeOutOfMemory
    };

    LookUpRetCodeEnum lookupAndSetStatusInternal(bool hasWriteRights,
                                                 bool removeIfOOM,
                                                 std::size_t* timeSpentWaiting,
                                                 std::size_t timeout);

    enum LookupAndCreateRetCodeEnum
    {
        eLookupAndCreateRetCodeCreated,
        eLookupAndCreateRetCodeOutOfToCMemory
    };

    LookupAndCreateRetCodeEnum lookupAndCreate(boost::scoped_ptr<Sharable_ReadLock> &tocReadLock,
                                               boost::scoped_ptr<Sharable_WriteLock> &tocWriteLock,
                                               std::size_t* timeSpentWaiting, std::size_t timeout);


    enum InsertRetCodeEnum
    {
        eInsertRetCodeCreated,
        eInsertRetCodeOutOfToCMemory,
        eInsertRetCodeFailed
    };

    // This function may throw a AbandonnedLockException or CorruptedCacheException
    InsertRetCodeEnum insertInternal();

    // This function may throw a AbandonnedLockException or CorruptedCacheException
    void lookupAndSetStatus(std::size_t* timeSpentWaiting, std::size_t timeout);

    void copyProcessLocalEntryFromEntry(const EntryType& entry);

    void copyProcessLocalEntryToEntry(EntryType* entry);

    InsertRetCodeEnum serializeCacheEntry(EntryType* entry);
};

struct CacheIPCData
{

    struct SharedMemorySegmentData
    {
        // Lock protecting the memory file read/write access.
        // Whenever a process/thread reads the memory segment, it takes the lock in read mode.
        // Whenever a process/thread needs to write to or grow or shrink the memory segment, it takes this lock
        // in write mode.
        SharedMutex segmentMutex;

        // True whilst the mapping is valid.
        // Any time the memory mapped file needs to be accessed, the caller
        // is supposed to call isTileFileMappingValid() to check if the mapping is still valid.
        // If this function returns false, the caller needs to take a write lock and call
        // remapToCMemoryFile or remapTileMemoryFile depending on the memory file accessed.
        bool mappingValid;

        // The number of processes with a valid mapping: use to count processes with a valid mapping
        // See pseudo code below, this is used in combination with the wait conditions.
        int nProcessWithMappingValid;

        // Threads wait on this condition whilst the mappingValid flag is false
        ConditionVariable mappingInvalidCond;

        // The thread that wants to grow the memory portion just waits in this condition
        // until nProcessWithToCMappingValid is 0.
        //
        // To resize the segment portion, it does as follow:
        //
        // writeLock(segmentMutex);
        // mappingValid = false;
        // segment.unmap();
        // --nProcessWithMappingValid;
        // while(nProcessWithMappingValid > 0) {
        //      mappedProcessesNotEmpty.wait(segmentMutex);
        // }
        // segment.grow();
        // segment.remap();
        // ++nProcessWithMappingValid;
        // mappingValid = true;
        // mappingInvalidCond.notifyAll();
        //
        // In other threads, before accessing the mapped region:
        //
        // thisMappingValid = true;
        // {
        //  readLock(segmentMutex);
        //  if (!mappingValid) {
        //      thisMappingValid = false;
        //  }
        // }
        // if (!thisMappingValid) {
        //    writeLock(segmentMutex);
        //    if (!mappingValid) {
        //          segment.unmap()
        //          --nProcessWithMappingValid;
        //          mappedProcessesNotEmpty.notifyOne();
        //          while(!mappingValid) {
        //              mappingInvalidCond.wait(segmentMutex);
        //          }
        //
        //          segment.remap();
        //          ++nProcessWithMappingValid;
        //    }
        // }
        ConditionVariable mappedProcessesNotEmpty;

        SharedMemorySegmentData()
        : segmentMutex()
        , mappingValid(true)
        , nProcessWithMappingValid(0)
        , mappingInvalidCond()
        , mappedProcessesNotEmpty()
        {

        }
    };
    struct PerBucketData
    {

        // Data related to the table of content memory mapped file
        SharedMemorySegmentData tocData;

        // Protects the bucket data structures except the LRU linked list
        SharedMutex bucketMutex;

        // Protects the LRU list (lruListFront & lruListBack) in the toc memory file.
        // This is separate mutex because even if we just access
        // the cache in read mode (in the get() function) we still need to update the LRU list, thus
        // protect it from being written by multiple concurrent threads.
        ExclusiveMutex lruListMutex;

    };


    PerBucketData bucketsData[NATRON_CACHE_BUCKETS_COUNT];

    // Protects the tilesStorage vector, taken in read mode when somebody is reading data from the tiled aligned storage
    // and taken in write mode when a file is removed/added
    SharedMutex tilesStorageMutex;


    CacheIPCData()
    : bucketsData()
    , tilesStorageMutex()
    {

    }

};


template <bool persistent>
struct CachePrivate
{
    typedef typename CacheBucket<persistent>::EntryType EntryType;
    typedef typename CacheBucket<persistent>::StorageType StorageType;
    typedef typename CacheBucket<persistent>::StoragePtrType StoragePtrType;

    // Raw pointer to the public interface: lives in process memory
    Cache<persistent>* _publicInterface;

    // The maximum size in bytes the cache can grow to
    // This is local to the process as it does not have to be shared necessarily:
    // if different accross processes then the process with the minimum size will
    // regulate the cache size.
    std::size_t maximumSize;

    // Protects all maximumSize.
    // Since it lives in process memory, this mutex
    // only protects against threads.
    boost::mutex maximumSizeMutex;

    // Each bucket handle entries with the 2 first hexadecimal numbers of the hash
    // This allows to hopefully dispatch threads and processes in 256 different buckets so that they are less likely
    // to take the same lock.
    // Each bucket is interprocess safe by itself.
    CacheBucket<persistent> buckets[NATRON_CACHE_BUCKETS_COUNT];


    // Each memory file contains a multiple of 256 tiles. Each file is exactly 1GiB meaning
    // each bucket gets 4MiB of storage per file. Once a bucket is full, we create a new file and
    // append it to the vector.
    // If the 8bit tile size is 128x128, then 4MiB can contain exactly 256 tiles.
    std::vector<StoragePtrType> tilesStorage;


#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    // The IPC data object created in globalMemorySegment shared memory
    CacheIPCData* ipc;
#else
    // If not IPC, this lives in memory
    boost::scoped_ptr<CacheIPCData> ipc;
#endif

    // Path of the directory that should contain the cache directory itself.
    // This is controled by a Natron setting. By default it points to a standard system dependent
    // location.
    // Only valid for a persistent cache.
    std::string directoryContainingCachePath;

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    // In windows, times returned by getTimestampInSeconds() must be divided by this value
    double timerFrequency;
#endif

    // The global file lock to monitor process access to the cache.
    // Only valid if the cache is persistent.
    boost::scoped_ptr<bip::file_lock> globalFileLock;

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    // Pointer to the memory segment used to store bucket independent data accross processes.
    // This is ensured to be always valid and lives in process memory.
    // The global memory segment is of a fixed size and only contains one instance of CacheIPCData.
    // This is the only shared memory segment that has a fixed size.
    boost::scoped_ptr<bip::managed_shared_memory> globalMemorySegment;

    // Used in the implementation of ensureSharedMemoryIntegrity()
    boost::scoped_ptr<bip::named_semaphore> nSHMInvalidSem, nSHMValidSem;

    // A mutex used in the algorithm described above to lock the process local threads.
    // Protects nThreadsTimedOutFailed
    // It should be taken for reading anytime a process use an object in shared memory
    // and locked for writing when unmapping/remapping the shared memory.
    boost::shared_mutex nThreadsTimedOutFailedMutex;

    // Counts how many threads in this process timed out on the segmentMutex, to avoid
    // remapping multiple times the shared memory.
    int nThreadsTimedOutFailed;

    // Protected by nThreadsTimedOutFailedMutex
    boost::condition_variable_any nThreadsTimedOutFailedCond;

#endif // NATRON_CACHE_INTERPROCESS_ROBUST

    bool useTileStorage;

    CachePrivate(Cache<persistent>* publicInterface, bool enableTileStorage)
    : _publicInterface(publicInterface)
    , maximumSize((std::size_t)8 * 1024 * 1024 * 1024) // 8GB max by default
    , maximumSizeMutex()
    , buckets()
    , tilesStorage()
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    , ipc(0)
#else
    , ipc()
#endif
    , directoryContainingCachePath()
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    , timerFrequency(getPerformanceFrequency())
#endif
    , useTileStorage(enableTileStorage)
    {

    }

    virtual ~CachePrivate()
    {
    }


    void initializeCacheDirPath();

    void ensureCacheDirectoryExists();

    QString getBucketAbsoluteDirPath(int bucketIndex) const;

    std::string getSharedMemoryName() const;

    std::size_t getSharedMemorySize() const;

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    /**
     * @brief Unmaps the global shared memory segment holding all bucket interprocess mutex
     * and re-create it. This function ensures that all process connected to the shared memory
     * are correctly remapped when exiting this function.
     **/
    void ensureSharedMemoryIntegrity();
#endif


    // This function may throw a AbandonnedLockException
    void clearCacheBucket(int bucket_i);

    /**
     * @brief Ensure the cache returns to a correct state. Currently it wipes the cache.
     **/
    void recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                      SHMReadLockerPtr& shmReader
#endif
                                      );

    void createTileStorage();

    void freeAllocatedTiles(U64 entryHash, const std::vector<U64>& tilesToAlloc, const std::vector<std::pair<U64, void*> >& allocatedTiles);

    /**
     * @brief Scan for existing tile files. This function throws an exception if the cache is corrupted
     **/
    void reOpenTileStorage();

};


/**
 * @brief A small RAII object that should be instanciated whenever taking a write lock on the bucket
 * If the cache is corrupted, the ctor will throw a CorruptedCacheException
 **/
template <bool persistent>
class BucketStateHandler_RAII
{
    const CacheBucket<persistent>* bucket;
public:

    BucketStateHandler_RAII(const CacheBucket<persistent>* bucket)
    :  bucket(bucket)
    {

        // The bucketMutex must be taken in write mode

        if (bucket->ipc->bucketState != eBucketStateOk) {
            throw CorruptedCacheException();
        }

        bucket->ipc->bucketState = eBucketStateInconsistent;
    }


    ~BucketStateHandler_RAII()
    {
        assert(bucket->ipc->bucketState == eBucketStateInconsistent);
        bucket->ipc->bucketState = eBucketStateOk;
    }
};

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST

/**
 * @brief Small RAII style class that should be used before using anything that is in the cache global shared memory
 * segment.
 * This prevents any other threads to call ensureSharedMemoryIntegrity() whilst this object is active.
 * Since any mutex in the cache is held in the globalMemorySegment, unmapping the segment could potentially crash any process
 * so we must carefully lock the access to the globalMemorySegment
 **/
template <bool persistent>
class SharedMemoryProcessLocalReadLocker
{
    boost::scoped_ptr<boost::shared_lock<boost::shared_mutex> > processLocalLocker;
public:

    SharedMemoryProcessLocalReadLocker(CachePrivate<persistent>* imp)
    {

        // A thread may enter ensureSharedMemoryIntegrity(), thus any other threads must ensure that the shared memory mapping
        // is valid before doing anything else.
        processLocalLocker.reset(new boost::shared_lock<boost::shared_mutex>(imp->nThreadsTimedOutFailedMutex));
        while (imp->nThreadsTimedOutFailed > 0) {
            imp->nThreadsTimedOutFailedCond.wait(*processLocalLocker);
        }

    }

    ~SharedMemoryProcessLocalReadLocker()
    {
        // Release the processLocalLocker, allowing other threads to call ensureSharedMemoryIntegrity()
    }

};


#endif

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
/**
 * @brief Creates a locker object around the given process shared mutex.
 * If after some time the mutex cannot be taken it is declared abandonned and throws a AbandonnedLockException
 **/
template <typename LOCK>
void createTimedLock(CachePrivate* imp,  boost::scoped_ptr<LOCK>& lock, typename LOCK::mutex_type* mutex)
{

    lock.reset(new LOCK(*mutex, imp->timerFrequency));
    if (!lock->timed_lock()) {
        throw AbandonnedLockException();
#ifdef CACHE_TRACE_TIMEOUTS
        qDebug() << QThread::currentThread() << "Lock timeout, clearing cache since it is probably corrupted.";
#endif
    }
}
#endif // #ifdef NATRON_CACHE_INTERPROCESS_ROBUST

template <bool persistent>
CacheEntryLockerPrivate<persistent>::CacheEntryLockerPrivate(CacheEntryLocker<persistent>* publicInterface, const boost::shared_ptr<Cache<persistent> >& cache, const CacheEntryBasePtr& entry)
: _publicInterface(publicInterface)
, cache(cache)
, processLocalEntry(entry)
, hash(entry->getHashKey())
, bucket(0)
, status(CacheEntryLockerBase::eCacheEntryStatusMustCompute)
{

}
template <bool persistent>
CacheEntryLocker<persistent>::CacheEntryLocker(const boost::shared_ptr<Cache<persistent> >& cache, const CacheEntryBasePtr& entry)
: _imp(new CacheEntryLockerPrivate<persistent>(this, cache, entry))
{

}

template <bool persistent>
boost::shared_ptr<CacheEntryLocker<persistent> >
CacheEntryLocker<persistent>::create(const boost::shared_ptr<Cache<persistent> >& cache, const CacheEntryBasePtr& entry)
{
    assert(entry);
    if (!entry) {
        throw std::invalid_argument("CacheEntryLocker::create: no entry");
    }
    boost::shared_ptr<CacheEntryLocker<persistent> >  ret(new CacheEntryLocker<persistent>(cache, entry));

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    // Lock the SHM for reading to ensure all process shared mutexes and other IPC structures remains valid.
    // This will prevent any other thread from calling ensureSharedMemoryIntegrity()
    boost::scoped_ptr<SharedMemoryProcessLocalReadLocker> shmAccess(new SharedMemoryProcessLocalReadLocker(cache->_imp.get()));
#endif

    // Lookup and find an existing entry.
    // Never take over an entry upon timeout.
    std::size_t timeSpentWaiting = 0;
    ret->_imp->lookupAndSetStatus(&timeSpentWaiting, 0);

    return ret;
}

template <bool persistent>
bool
CacheBucket<persistent>::isToCFileMappingValid() const
{
    // Private - the tocData.segmentMutex is assumed to be taken for read lock
    boost::shared_ptr<Cache<persistent> > c = cache.lock();
    assert(!c->_imp->ipc->bucketsData[bucketIndex].tocData.segmentMutex.try_lock());
    return c->_imp->ipc->bucketsData[bucketIndex].tocData.mappingValid ;
}


template <typename StoragePtrType>
void ensureMappingValidInternal(Sharable_WriteLock& lock,
                                const StoragePtrType& memoryMappedFile,
                                CacheIPCData::SharedMemorySegmentData* segment);

template <>
void ensureMappingValidInternal(Sharable_WriteLock& lock,
                                const MemoryFilePtr& memoryMappedFile,
                                CacheIPCData::SharedMemorySegmentData* segment)
{
    memoryMappedFile->close();
    std::string filePath = memoryMappedFile->path();

    // Decrement nProcessWithMappingValid and notify the thread that is resizing
    if (segment->nProcessWithMappingValid > 0) {
        --segment->nProcessWithMappingValid;
    }
    segment->mappedProcessesNotEmpty.notify_one();

    // Wait until the mapping becomes valid again
    while(!segment->mappingValid) {
        segment->mappingInvalidCond.wait(lock);
    }

    memoryMappedFile->open(filePath, MemoryFile::eFileOpenModeOpenOrCreate);
    ++segment->nProcessWithMappingValid;
} // ensureMappingValidInternal

template <>
void ensureMappingValidInternal(Sharable_WriteLock& /*lock*/,
                                const ProcessLocalBufferPtr& /*memoryMappedFile*/,
                                CacheIPCData::SharedMemorySegmentData* /*segment*/) {}
template <typename StoragePtrType>
std::string getStoragePath(const StoragePtrType& storage);

template <typename StoragePtrType>
void clearStorage(const StoragePtrType& storage);

template <typename StoragePtrType>
void openStorage(const StoragePtrType& storage, const std::string& path, int flag);

template <typename StoragePtrType>
void resizeStorage(const StoragePtrType& storage, std::size_t numBytes);


template <>
std::string getStoragePath(const MemoryFilePtr& storage)
{
    return storage->path();
}

template <>
void clearStorage(const MemoryFilePtr& storage)
{
    storage->remove();
}

template <>
void openStorage(const MemoryFilePtr& storage, const std::string& path, int flag)
{
    storage->open(path, (MemoryFile::FileOpenModeEnum)flag);
}

template <>
void resizeStorage(const MemoryFilePtr& storage, std::size_t numBytes)
{
    storage->resize(numBytes, false);
}

template <>
std::string getStoragePath(const ProcessLocalBufferPtr& /*storage*/)
{
    return std::string();
}
template <>
void clearStorage(const ProcessLocalBufferPtr& storage) { storage->clear(); }
template <>
void openStorage(const ProcessLocalBufferPtr& /*storage*/, const std::string& /*path*/, int /*flag*/) {}

template <>
void resizeStorage(const ProcessLocalBufferPtr& storage, std::size_t numBytes)
{
    storage->resize(numBytes);
}


template <bool persistent>
static void reOpenToCData(CacheBucket<persistent>* bucket, bool create)
{
    // Re-create the manager on the new mapped buffer
    try {
        char* data = bucket->tocFile->getData();
        std::size_t dataNumBytes = bucket->tocFile->size();

        if (create) {
            bucket->tocFileManager.reset(new ExternalSegmentType(bip::create_only, data, dataNumBytes));
        } else {
            bucket->tocFileManager.reset(new ExternalSegmentType(bip::open_only, data, dataNumBytes));
        }
        {
            std::size_t curSize = bucket->tocFileManager->get_size();
            if (curSize < dataNumBytes) {
                std::size_t toGrow = dataNumBytes - curSize;
                bucket->tocFileManager->grow(toGrow);
            }
        }
        // The ipc data pointer must be re-fetched
        void_allocator allocator(bucket->tocFileManager->get_segment_manager());
        bucket->ipc = bucket->tocFileManager->template find_or_construct<CacheBucketIPCData<persistent> >("BucketData")(allocator);

        // If the version of the data is different than this build, wipe it and re-create it
        if (bucket->ipc->version != NATRON_MEMORY_SEGMENT_ENTRY_HEADER_VERSION) {
            std::string tileFilePath = getStoragePath(bucket->tocFile);
            clearStorage(bucket->tocFile);
            openStorage(bucket->tocFile, tileFilePath, MemoryFile::eFileOpenModeOpenTruncateOrCreate);
            resizeStorage(bucket->tocFile, NATRON_CACHE_BUCKET_TOC_FILE_GROW_N_BYTES);
            reOpenToCData(bucket, true /*create*/);
        }

    } catch (...) {
        assert(false);
        throw std::runtime_error("Not enough memory to allocate bucket table of content");
    }
}


template <typename StoragePtrType>
void resizeAndPreserve(const StoragePtrType& storage, std::size_t newSize);

template <>
void resizeAndPreserve(const MemoryFilePtr& storage, std::size_t newSize)
{
    // Save the entire file
    storage->flush(MemoryFile::eFlushTypeSync, NULL, 0);

    // we pass preserve=false since we flushed the portion we know is valid just above
    storage->resize(newSize, false /*preserve*/);
}

template <>
void resizeAndPreserve(const ProcessLocalBufferPtr& storage, std::size_t newSize)
{
    storage->resizeAndPreserve(newSize);
}


template <typename StoragePtrType>
void flushMemory(const StoragePtrType& storage, int flag, char* ptr, std::size_t numBytes);

template <>
void flushMemory(const MemoryFilePtr& storage, int flag, char* ptr, std::size_t numBytes)
{
    storage->flush((MemoryFile::FlushTypeEnum)flag, ptr, numBytes);
}

template <>
void flushMemory(const ProcessLocalBufferPtr& /*storage*/, int /*flag*/, char* /*ptr*/, std::size_t /*numBytes*/) {}


template <bool persistent>
void
CacheBucket<persistent>::remapToCMemoryFile(Sharable_WriteLock& lock, std::size_t minFreeSize)
{
    // Private - the tocData.segmentMutex is assumed to be taken for write lock
    boost::shared_ptr<Cache<persistent> > c = cache.lock();
    if (persistent) {
        if (!c->_imp->ipc->bucketsData[bucketIndex].tocData.mappingValid) {
            // Save the entire file
            flushMemory(tocFile, (int)MemoryFile::eFlushTypeSync, NULL, 0);
        }

#ifdef CACHE_TRACE_FILE_MAPPING
        qDebug() << "Checking ToC mapping:" << c->_imp->ipc->bucketsData[bucketIndex].tocData.mappingValid;
#endif

        ensureMappingValidInternal(lock, tocFile, &c->_imp->ipc->bucketsData[bucketIndex].tocData);
    }
    // Ensure the size of the ToC file is reasonable
    std::size_t curNumBytes = tocFile->size();

    if (curNumBytes == 0) {
        growToCFile(lock, minFreeSize);
    } else {
        reOpenToCData(this, false /*create*/);

        // Check that there's enough memory, if not grow the file
        ExternalSegmentType::size_type freeMem = tocFileManager->get_free_memory();
        if (freeMem < minFreeSize) {
            std::size_t minbytesToGrow = minFreeSize - freeMem;
            growToCFile(lock, minbytesToGrow);
        }
    }
    assert(tocFileManager->get_free_memory() >= minFreeSize);

} // remapToCMemoryFile


template <bool persistent>
void
CacheBucket<persistent>::growToCFile(Sharable_WriteLock& lock, std::size_t bytesToAdd)
{
    // Private - the tocData.segmentMutex is assumed to be taken for write lock

    boost::shared_ptr<Cache<persistent> > c = cache.lock();

    if (persistent) {
        c->_imp->ipc->bucketsData[bucketIndex].tocData.mappingValid = false;

        --c->_imp->ipc->bucketsData[bucketIndex].tocData.nProcessWithMappingValid;
        while (c->_imp->ipc->bucketsData[bucketIndex].tocData.nProcessWithMappingValid > 0) {
            c->_imp->ipc->bucketsData[bucketIndex].tocData.mappedProcessesNotEmpty.wait(lock);
        }
    }


    std::size_t oldSize = tocFile->size();

    // Round to the nearest next multiple of NATRON_CACHE_BUCKET_TOC_FILE_GROW_N_BYTES
    std::size_t bytesToAddRounded = std::max((std::size_t)1, (std::size_t)std::ceil(bytesToAdd / (double) NATRON_CACHE_BUCKET_TOC_FILE_GROW_N_BYTES)) * NATRON_CACHE_BUCKET_TOC_FILE_GROW_N_BYTES;
    std::size_t newSize = oldSize + bytesToAddRounded;

    resizeAndPreserve(tocFile, newSize);

#ifdef CACHE_TRACE_FILE_MAPPING
    qDebug() << "Growing ToC file to " << printAsRAM(newSize);
#endif


    reOpenToCData(this, oldSize == 0 /*create*/);

    if (persistent) {
        ++c->_imp->ipc->bucketsData[bucketIndex].tocData.nProcessWithMappingValid;

        // Flag that the mapping is valid again and notify all other threads waiting
        c->_imp->ipc->bucketsData[bucketIndex].tocData.mappingValid = true;

        c->_imp->ipc->bucketsData[bucketIndex].tocData.mappingInvalidCond.notify_all();
    }

} // growToCFile

template <bool persistent>
bool
CacheBucket<persistent>::tryCacheLookupImpl(U64 hash, typename EntriesMap::iterator* found, EntriesMap** storage)
{
    // The bucket mutex is assumed to be taken at least in read lock mode
    assert(!cache.lock()->_imp->ipc->bucketsData[bucketIndex].bucketMutex.try_lock());
    *storage = &ipc->entriesMap;
    *found = (*storage)->find(hash);
    return *found != (*storage)->end();
} // tryCacheLookupImpl

template <>
typename CacheBucket<false>::ShmEntryReadRetCodeEnum
CacheBucket<false>::deserializeEntry(EntryType* /*cacheEntry*/, const CacheEntryBasePtr& /*processLocalEntry*/, U64 /*hash*/, bool /*hasWriteRights*/) { return eShmEntryReadRetCodeOk; }

template <>
typename CacheBucket<true>::ShmEntryReadRetCodeEnum
CacheBucket<true>::deserializeEntry(EntryType* cacheEntry, const CacheEntryBasePtr& processLocalEntry, U64 hash, bool hasWriteRights)
{
    // Deserialize the entry. This may throw an exception if it cannot be deserialized properly
    // or out of memory
    // First check the hash: it was the last object written in the memory segment, check that it is valid
    U64 serializedHash = 0;

    //kNatronIPCPropertyHash
    bool gotHash = cacheEntry->properties.getIPCProperty(kNatronIPCPropertyHash, 0, &serializedHash);
    if (!gotHash || serializedHash != hash) {
        // The serialized hash is not the same, the entry was probably not written properly.
        return eShmEntryReadRetCodeDeserializationFailed;
    }

    CacheEntryBase::FromMemorySegmentRetCodeEnum stat;
    try {
        stat = processLocalEntry->fromMemorySegment(hasWriteRights, cacheEntry->properties);
    } catch (const bip::bad_alloc& /*e*/) {
        if (hasWriteRights) {
            // Whilst under the write lock, the process local entry is allowed to upload stuff to the cache which may require
            // allocation in the memory segment
            return eShmEntryReadRetCodeOutOfMemory;
        } else {
            // Whilst under the read lock just fail, the entry is not supposed to allocate anything
            return eShmEntryReadRetCodeDeserializationFailed;
        }
    } catch (const std::exception& /*e*/) {
        // Any other exception fail
        return eShmEntryReadRetCodeDeserializationFailed;
    }
    switch (stat) {
        case CacheEntryBase::eFromMemorySegmentRetCodeOk:
            break;
        case CacheEntryBase::eFromMemorySegmentRetCodeFailed:
            return eShmEntryReadRetCodeDeserializationFailed;
        case CacheEntryBase::eFromMemorySegmentRetCodeNeedWriteLock:
            // This status code can only be given if !hasWriteRights
            assert(!hasWriteRights);
            if (hasWriteRights) {
                return eShmEntryReadRetCodeDeserializationFailed;
            } else {
                return eShmEntryReadRetCodeNeedWriteLock;
            }
    }
    // Now compute the hash from the deserialized entry and check that it matches the given hash
    serializedHash = processLocalEntry->getHashKey(true /*forceComputation*/);

    // The entry changed, probably it was something of another type
    if (serializedHash != hash) {
        return eShmEntryReadRetCodeDeserializationFailed;
    }
    return eShmEntryReadRetCodeOk;

}

template <bool persistent>
typename CacheBucket<persistent>::ShmEntryReadRetCodeEnum
CacheBucket<persistent>::readFromSharedMemoryEntryImpl(EntryType* cacheEntry,
                                                       const CacheEntryBasePtr& processLocalEntry,
                                                       U64 hash,
                                                       bool hasWriteRights)
{
    boost::shared_ptr<Cache<persistent> > c = cache.lock();
    
    
    // Private - the tocData.segmentMutex is assumed to be taken at least in read lock mode
    assert(!c->_imp->ipc->bucketsData[bucketIndex].tocData.segmentMutex.try_lock());

    // The bucket mutex is assumed to be taken at least in read lock mode
    assert(!cache.lock()->_imp->ipc->bucketsData[bucketIndex].bucketMutex.try_lock());

    // The entry must have been looked up in tryCacheLookup()
    assert(cacheEntry);

    assert(cacheEntry->status == EntryType::eEntryStatusReady);


    if (persistent) {
        ShmEntryReadRetCodeEnum ret = deserializeEntry(cacheEntry, processLocalEntry, hash, hasWriteRights);
        if (ret != eShmEntryReadRetCodeOk) {
            return ret;
        }
    } // persistent


    // Update LRU record if this item is not already at the tail of the list
    //
    // Take the LRU list mutex
    {
        boost::scoped_ptr<ExclusiveLock> lruWriteLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        lruWriteLock.reset(new ExclusiveLock(c->_imp->ipc->bucketsData[bucketIndex].lruListMutex));
#else
        createTimedLock<ExclusiveLock>(c->_imp.get(), lruWriteLock, &c->_imp->ipc->bucketsData[bucketIndex].lruListMutex);
#endif

        assert(ipc->lruListBack && !ipc->lruListBack->next);
        if (getRawPointer(ipc->lruListBack) != &cacheEntry->lruNode) {

            LRUListNodePtr entryNode(&cacheEntry->lruNode);
            disconnectLinkedListNode(entryNode);

            // And push_back to the tail of the list...
            insertLinkedListNode(entryNode, ipc->lruListBack, LRUListNodePtr(0));
            ipc->lruListBack = entryNode;
        }
    } // lruWriteLock

    return eShmEntryReadRetCodeOk;

} // readFromSharedMemoryEntryImpl

/**
 * @brief Given an encoded tile index, the left most 32 bits represents the tile index in the file
 * The file index is determined by the right most 32 bits
 **/
inline void getTileIndex(U64 encoded, U32* tileIndex, U32* fileIndex)
{
    *fileIndex = encoded;
    *tileIndex = encoded >> 32;
    assert(*tileIndex >= 0 && *tileIndex < NATRON_NUM_TILES_PER_FILE);
}

template <bool persistent>
void
CacheBucket<persistent>::deallocateCacheEntryImpl(typename EntriesMap::iterator cacheEntryIt,
                                      EntriesMap* storage)
{

     boost::shared_ptr<Cache<persistent> > c = cache.lock();

    // The tocData.segmentMutex must be taken in read mode
    assert(!c->_imp->ipc->bucketsData[bucketIndex].tocData.segmentMutex.try_lock());

    // The bucket mutex is assumed to be taken in write mode
    assert(!c->_imp->ipc->bucketsData[bucketIndex].bucketMutex.try_lock());

    assert(cacheEntryIt != storage->end());

    ipc->size -= cacheEntryIt->second->size;

    // Clear allocated tiles for this entry
    if (!cacheEntryIt->second->tileIndices.empty()) {

        ipc->size -= cacheEntryIt->second->tileIndices.size() * NATRON_TILE_SIZE_BYTES;

        // Take the tilesStorageMutex in read mode to indicate that we are operating on it (flush)
        boost::scoped_ptr<Sharable_ReadLock> tileAlignedFileLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        tileAlignedFileLock.reset(new Sharable_ReadLock(c->_imp->ipc->tilesStorageMutex));
#else
        createTimedLock<Sharable_ReadLock>(c->_imp.get(), tileAlignedFileLock, &c->_imp->ipc->tilesStorageMutex);
#endif

        for (ExternalSegmentTypeULongLongList::const_iterator it = cacheEntryIt->second->tileIndices.begin(); it != cacheEntryIt->second->tileIndices.end(); ++it) {

            U32 fileIndex, tileIndex;
            getTileIndex(*it, &tileIndex, &fileIndex);

            if (persistent) {
                // Invalidate this portion of the memory mapped file so it doesn't get written on disk
                StoragePtrType storage;
                if (fileIndex < c->_imp->tilesStorage.size()) {
                    storage = c->_imp->tilesStorage[fileIndex];
                }
                if (storage) {
                    std::size_t dataOffset = tileIndex * NATRON_TILE_SIZE_BYTES;
                    flushMemory(storage, (int)MemoryFile::eFlushTypeInvalidate, storage->getData() + dataOffset, NATRON_TILE_SIZE_BYTES);
                }
                
            }

            // Retrieve the bucket index directly from the tile index: we know that each file contains exactly NATRON_NUM_TILES_PER_BUCKET_FILE * NATRON_CACHE_BUCKETS_COUNT
            int tileBucketIndex = tileIndex % NATRON_NUM_TILES_PER_BUCKET_FILE;
            assert(tileBucketIndex >= 0 && tileBucketIndex < NATRON_CACHE_BUCKETS_COUNT);
            // Take the bucket mutex except if this is the current bucket
            boost::scoped_ptr<Sharable_WriteLock> bucketWriteLock;
            if (tileBucketIndex != bucketIndex) {
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                bucketWriteLock.reset(new Sharable_WriteLock(c->_imp->ipc->bucketsData[tileBucketIndex].bucketMutex));
#else
                createTimedLock<Sharable_WriteLock>(c->_imp.get(), *bucketWriteLock, &c->_imp->ipc->bucketsData[tileBucketIndex].bucketMutex);
#endif
            }

            // Make this tile free again
#ifdef CACHE_TRACE_TILES_ALLOCATION
            qDebug() << "Bucket" << bucketIndex << ": tile freed" << tileIndex << " Nb free tiles left:" << c->_imp->buckets[tileBucketIndex].ipc->freeTiles.size();
#endif
            // free tiles are all shared in the FIRST bucket
            std::pair<U64_Set::iterator, bool>  insertOk = c->_imp->buckets[tileBucketIndex].ipc->freeTiles.insert(*it);
            assert(insertOk.second);
            (void)insertOk;
        }
        cacheEntryIt->second->tileIndices.clear();
    }


    // Remove this entry from the LRU list
    {
        // Take the lock of the LRU list.
        boost::scoped_ptr<ExclusiveLock> lruWriteLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        lruWriteLock.reset(new ExclusiveLock(c->_imp->ipc->bucketsData[bucketIndex].lruListMutex));
#else
        createTimedLock<ExclusiveLock>(c->_imp.get(), lruWriteLock, &c->_imp->ipc->bucketsData[bucketIndex].lruListMutex);
#endif
        // Ensure the back and front pointers do not point to this entry
        if (&cacheEntryIt->second->lruNode == getRawPointer(ipc->lruListBack)) {
            // We are the last node, we can't have a next entry
            assert(!cacheEntryIt->second->lruNode.next);
            ipc->lruListBack = cacheEntryIt->second->lruNode.prev;
        }
        if (&cacheEntryIt->second->lruNode == getRawPointer(ipc->lruListFront)) {
            ipc->lruListFront = cacheEntryIt->second->lruNode.prev;
        }

        // Remove this entry's node from the list
        disconnectLinkedListNode(&cacheEntryIt->second->lruNode);
    }
    try {
        tocFileManager->destroy_ptr<EntryType>(cacheEntryIt->second.get());
    } catch (...) {
        qDebug() << "[BUG]: Failure to free entry" << cacheEntryIt->first;
    }

    // deallocate the entry
#ifdef CACHE_TRACE_ENTRY_ACCESS
    qDebug() << QThread::currentThread() << cacheEntryIt->first << ": destroy entry";
#endif
    storage->erase(cacheEntryIt);
} // deallocateCacheEntryImpl

/*
 helper function to do thread sleeps, since usleep()/nanosleep()
 aren't reliable enough (in terms of behavior and availability)
 */
#ifdef __NATRON_UNIX__
static void thread_sleep(struct timespec *ti)
{
    pthread_mutex_t mtx;
    pthread_cond_t cnd;

    pthread_mutex_init(&mtx, 0);
    pthread_cond_init(&cnd, 0);

    pthread_mutex_lock(&mtx);
    (void) pthread_cond_timedwait(&cnd, &mtx, ti);
    pthread_mutex_unlock(&mtx);

    pthread_cond_destroy(&cnd);
    pthread_mutex_destroy(&mtx);
}
#endif

void
CacheEntryLockerBase::sleep_milliseconds(std::size_t amountMS)
{
#ifdef __NATRON_WIN32__
     ::Sleep(amountMS);
#elif defined(__NATRON_UNIX__)
    struct timeval tv;
    gettimeofday(&tv, 0);
    struct timespec ti;

    ti.tv_nsec = (tv.tv_usec + (amountMS % 1000) * 1000) * 1000;
    ti.tv_sec = tv.tv_sec + (amountMS / 1000) + (ti.tv_nsec / 1000000000);
    ti.tv_nsec %= 1000000000;
    thread_sleep(&ti);
#else
#error "unsupported OS"
#endif

}

template <> void CacheEntryLockerPrivate<true>::copyProcessLocalEntryFromEntry(const EntryType& /*entry*/) {}
template <> void CacheEntryLockerPrivate<true>::copyProcessLocalEntryToEntry(EntryType* /*entry*/) {}

template <>
void
CacheEntryLockerPrivate<false>::copyProcessLocalEntryFromEntry(const EntryType& entry)
{
    assert(entry.nonPersistentEntry);
    processLocalEntry = entry.nonPersistentEntry;
}

template <>
void
CacheEntryLockerPrivate<false>::copyProcessLocalEntryToEntry(EntryType* entry)
{
    entry->nonPersistentEntry = processLocalEntry;
}

template <bool persistent>
typename CacheEntryLockerPrivate<persistent>::LookUpRetCodeEnum
CacheEntryLockerPrivate<persistent>::lookupAndSetStatusInternal(bool hasWriteRights,
                                                    bool removeIfOOM,
                                                    std::size_t *timeSpentWaitingForPendingEntryMS,
                                                    std::size_t timeout)
{

    // By default the entry status is set to be computed
    status = CacheEntryLockerBase::eCacheEntryStatusMustCompute;


    // Look-up the entry
    typename EntriesMap::iterator found;
    EntriesMap* storage;
    if (!bucket->tryCacheLookupImpl(hash, &found, &storage)) {
        // No entry matching the hash could be found.
#ifdef CACHE_TRACE_ENTRY_ACCESS
        qDebug() << QThread::currentThread() << "(locker=" << this << ")"<< hash << "look-up: entry not found, type ID=" << processLocalEntry->getKey()->getUniqueID();
#endif
        return eLookUpRetCodeNotFound;
    }
#ifdef CACHE_TRACE_ENTRY_ACCESS
    qDebug() << QThread::currentThread() << "(locker=" << this << ")"<< hash << "look-up: found, type ID=" << processLocalEntry->getKey()->getUniqueID();
#endif

    if (found->second->status == MemorySegmentEntryHeaderBase::eEntryStatusNull) {
        // The entry was aborted by a thread and nobody is computing it yet.
        // If we have write rights, takeover the entry
        // otherwise, wait for the 2nd look-up under the Write lock to do it.
        if (!hasWriteRights) {
            return eLookUpRetCodeNotFound;
        }
#ifdef CACHE_TRACE_ENTRY_ACCESS
        qDebug() << QThread::currentThread() <<  "(locker=" << this << ")"<< hash << ": entry found but NULL, thread" << QThread::currentThread() << "is taking over the entry";
#endif
    }

    if (found->second->status == MemorySegmentEntryHeaderBase::eEntryStatusPending) {

        bool recursionDetected = !processLocalEntry->allowMultipleFetchForThread() && (found->second->computeThreadMagic == reinterpret_cast<U64>(QThread::currentThread()));
        if (recursionDetected) {
            qDebug() << "[BUG]: Detected recursion while computing" << hash << ". This means that the same thread is attempting to compute an entry recursively that it already started to compute. You should release the associated CacheEntryLocker first.";
        } else {
            // If a timeout was provided, takeover after the timeout
            if (timeout == 0 || *timeSpentWaitingForPendingEntryMS < timeout) {
                status = CacheEntryLockerBase::eCacheEntryStatusComputationPending;
#ifdef CACHE_TRACE_ENTRY_ACCESS
                qDebug() << QThread::currentThread() <<  "(locker=" << this << ")"<< hash << ": entry pending";
#endif

                return eLookUpRetCodeFound;
            }
        }
        // We need write rights to take over the entry
        if (!hasWriteRights) {
            return eLookUpRetCodeNotFound;
        }
#ifdef CACHE_TRACE_ENTRY_ACCESS
        qDebug() << QThread::currentThread() << "(locker=" << this << ")"<< hash << ": entry pending timeout, thread" << QThread::currentThread() << "is taking over the entry";
#endif
    }

    if (found->second->status == MemorySegmentEntryHeaderBase::eEntryStatusReady) {
        // Deserialize the entry and update the status
        typename CacheBucket<persistent>::ShmEntryReadRetCodeEnum readStatus = bucket->readFromSharedMemoryEntryImpl(found->second.get(), processLocalEntry, hash, hasWriteRights);

        // By default we must compute
        status = CacheEntryLockerBase::eCacheEntryStatusMustCompute;
        switch (readStatus) {
            case CacheBucket<persistent>::eShmEntryReadRetCodeOk:
                if (!persistent) {
                    copyProcessLocalEntryFromEntry(*found->second);
                }
                status = CacheEntryLockerBase::eCacheEntryStatusCached;
                break;
            case CacheBucket<persistent>::eShmEntryReadRetCodeDeserializationFailed:
                // If the entry failed to deallocate or is not of the type of the process local entry
                // we have to remove it from the cache.
                // However we cannot do so under the read lock, we must take the write lock.
                // So do it in the 2nd lookup attempt.
                if (hasWriteRights) {
                    bucket->deallocateCacheEntryImpl(found, storage);
                }
                return eLookUpRetCodeNotFound;
            case CacheBucket<persistent>::eShmEntryReadRetCodeNeedWriteLock:
                assert(!hasWriteRights);
                // Need to retry with a write lock
                return eLookUpRetCodeNotFound;
            case CacheBucket<persistent>::eShmEntryReadRetCodeOutOfMemory:
                if (removeIfOOM && hasWriteRights) {
                    bucket->deallocateCacheEntryImpl(found, storage);
                    return eLookUpRetCodeNotFound;
                }
                return eLookUpRetCodeOutOfMemory;

        }

    } else {
        // Either the entry was eEntryStatusNull and we have to compute it or the entry was still marked eEntryStatusPending
        // but we timed out and took over the entry computation.
        assert(hasWriteRights);
        found->second->status = MemorySegmentEntryHeaderBase::eEntryStatusPending;
        status = CacheEntryLockerBase::eCacheEntryStatusMustCompute;
    }

    // If the entry is still pending, that means the thread that originally should have computed this entry failed to do so.
    // If we were in waitForPendingEntry(), we now have the lock on the entry, thus change the status
    // to eCacheEntryStatusMustCompute to indicate that we must compute the entry now.
    // If we are looking up the first time, then we keep the status to pending, the caller will
    // just have to call waitForPendingEntry()
    switch (status) {
        case CacheEntryLockerBase::eCacheEntryStatusComputationPending:
        case CacheEntryLockerBase::eCacheEntryStatusMustCompute: {
#ifdef CACHE_TRACE_ENTRY_ACCESS
            qDebug() << QThread::currentThread() <<  "(locker=" << this << ")"<< hash << ": got entry but it has to be computed";
#endif
        }   break;
        case CacheEntryLockerBase::eCacheEntryStatusCached:
        {
            // We found in cache, nothing to do
#ifdef CACHE_TRACE_ENTRY_ACCESS
            qDebug() << QThread::currentThread() <<  "(locker=" << this << ")"<< hash << ": entry cached";
#endif
        }   break;
    } // switch(status)
    return eLookUpRetCodeFound;
} // lookupAndSetStatusInternal

template <bool persistent>
typename CacheEntryLockerPrivate<persistent>::LookupAndCreateRetCodeEnum
CacheEntryLockerPrivate<persistent>::lookupAndCreate(boost::scoped_ptr<Sharable_ReadLock> &tocReadLock,
                                                     boost::scoped_ptr<Sharable_WriteLock> &tocWriteLock,
                                                     std::size_t* timeSpentWaiting,
                                                     std::size_t timeout)
{
    boost::scoped_ptr<Sharable_WriteLock> writeLock;

#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
    writeLock.reset(new Sharable_WriteLock(cache->_imp->ipc->bucketsData[bucket->bucketIndex].bucketMutex));
#else
    createTimedLock<Sharable_WriteLock>(cache->_imp.get(), writeLock, &cache->_imp->ipc->bucketsData[bucket->bucketIndex].bucketMutex);
#endif


    // This function only fails if the entry must be computed anyway.
    {
        int nAttempts = 0;
        const int maxAttempts = 2;
        for (;;) {
            bool mustBreak = false;
            LookUpRetCodeEnum stat = lookupAndSetStatusInternal(true /*hasWriteRights*/, nAttempts == maxAttempts - 1/*removeIfOOM*/,timeSpentWaiting, timeout);
            switch (stat) {
                case eLookUpRetCodeFound:
                    return CacheEntryLockerPrivate::eLookupAndCreateRetCodeCreated;
                case eLookUpRetCodeNotFound:
                    mustBreak = true;
                    break;
                case eLookUpRetCodeOutOfMemory:
                    // If out of memory, grow the toc and try again
                    if (!tocWriteLock) {
                        tocReadLock.reset();
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                        tocWriteLock.reset(new Sharable_WriteLock(cache->_imp->ipc->bucketsData[bucket->bucketIndex].tocData.segmentMutex));
#else
                        createTimedLock<Sharable_WriteLock>(cache->_imp.get(), *tocWriteLock, &cache->_imp->ipc->bucketsData[bucket->bucketIndex].tocData.segmentMutex);
#endif
                    }
#ifdef DEBUG
                    qDebug() << "Out of memory after a call to fromMemorySegment, free mem= " << bucket->tocFileManager->get_free_memory();
#endif
                    if (!bucket->isToCFileMappingValid()) {
                        bucket->remapToCMemoryFile(*tocWriteLock, NATRON_CACHE_BUCKET_TOC_FILE_GROW_N_BYTES);
                    } else {
                        bucket->growToCFile(*tocWriteLock, NATRON_CACHE_BUCKET_TOC_FILE_GROW_N_BYTES);
                    }
                    break;
            }
            if (mustBreak) {
                break;
            }
            ++nAttempts;
            if (nAttempts >= maxAttempts) {
                break;
            }
        }

    }
    
    
    assert(status == CacheEntryLockerBase::eCacheEntryStatusMustCompute);

    // Edit: we don't use an upgradable lock anymore since we need exclusive rights in lookupAndSetStatusInternal()
    //boost::scoped_ptr<Upgradable_WriteLock> writeLock;
    // We need to upgrade the lock to a write lock. This will wait until all other threads have released their
    // read lock.
    //writeLock.reset(new scoped_upgraded_lock(boost::move(*upgradableLock)));

    // Now we are the only thread in this portion.

    // Ensure the bucket is in a valid state.
    BucketStateHandler_RAII<persistent> bucketStateHandler(bucket);


    // Create the MemorySegmentEntry if it does not exist
#ifdef CACHE_TRACE_ENTRY_ACCESS
    qDebug() << QThread::currentThread() <<  "(locker=" << this << ")"<< hash << ": construct entry type ID=" << processLocalEntry->getKey()->getUniqueID();
#endif

    EntryTypePtr cacheEntry;

    void_allocator allocator(bucket->tocFileManager->get_segment_manager());

    // the construction of the object may fail if the segment is out of memory. Upon failure, grow the ToC file and retry to allocate.
    try {

        cacheEntry = bucket->tocFileManager->template construct<EntryType>(bip::anonymous_instance)(allocator);
        EntriesMapValueType pair = std::make_pair(hash, cacheEntry);
        std::pair<typename EntriesMap::iterator, bool> ok = bucket->ipc->entriesMap.insert(boost::move(pair));
        assert(ok.second);
        (void)ok;
    } catch (const bip::bad_alloc& /*e*/) {
        return CacheEntryLockerPrivate::eLookupAndCreateRetCodeOutOfToCMemory;
    }

    copyProcessLocalEntryToEntry(cacheEntry.get());

    std::size_t entryToCSize = processLocalEntry->getMetadataSize();
    cacheEntry->size = entryToCSize;

    cacheEntry->pluginID.append(processLocalEntry->getKey()->getHolderPluginID().c_str());

    // Lock the statusMutex: this will lock-out other threads interested in this entry.
    // This mutex is unlocked in deallocateCacheEntryImpl() or in insertInCache()
    // We must get the lock since we are the first thread to create it and we own the write lock on the segmentMutex


    assert(cacheEntry->status == MemorySegmentEntryHeaderBase::eEntryStatusNull);

    // Set the status of the entry to pending because we (this thread) are going to compute it.
    // Other fields of the entry will be set once it is done computed in insertInCache()
    cacheEntry->status = MemorySegmentEntryHeaderBase::eEntryStatusPending;

    // Set the pointer to the current thread so we can detect immediate recursion and not wait forever
    // in waitForPendingEntry().
    // Note that this value has no meaning outside this process and is set back to 0 in insertInCache()
    cacheEntry->computeThreadMagic = reinterpret_cast<U64>(QThread::currentThread());

    return CacheEntryLockerPrivate::eLookupAndCreateRetCodeCreated;
} // lookupAndCreate

template <bool persistent>
void
CacheBucket<persistent>::checkToCMemorySegmentStatus(boost::scoped_ptr<Sharable_ReadLock>* tocReadLock, boost::scoped_ptr<Sharable_WriteLock>* tocWriteLock)
{
    boost::shared_ptr<Cache<persistent> > c = cache.lock();
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
    tocReadLock->reset(new Sharable_ReadLock(c->_imp->ipc->bucketsData[bucketIndex].tocData.segmentMutex));
#else
    createTimedLock<Sharable_ReadLock>(c->_imp.get(), *tocReadLock, &c->_imp->ipc->bucketsData[bucketIndex].tocData.segmentMutex);
#endif

    if (persistent) {
        // Every time we take the lock, we must ensure the memory mapping is ok because the
        // memory mapped file might have been resized to fit more entries.
        if (!isToCFileMappingValid()) {
            // Remove the read lock, and take a write lock.
            // This could allow other threads to run in-between, but we don't care since nothing happens.
            tocReadLock->reset();

#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            tocWriteLock->reset(new Sharable_WriteLock(c->_imp->ipc->bucketsData[bucketIndex].tocData.segmentMutex));
#else
            createTimedLock<Sharable_WriteLock>(c->_imp.get(), *tocWriteLock, &c->_imp->ipc->bucketsData[bucketIndex].tocData.segmentMutex);
#endif

            remapToCMemoryFile(**tocWriteLock, 0);
        }
    }
}

template <bool persistent>
void
CacheEntryLockerPrivate<persistent>::lookupAndSetStatus(std::size_t* timeSpentWaiting, std::size_t timeout)
{

    // Get the bucket corresponding to the hash. This will dispatch threads in (hopefully) different
    // buckets
    if (!bucket) {
        bucket = &cache->_imp->buckets[CacheBase::getBucketCacheBucketIndex(hash)];
    }

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmAccess(new SharedMemoryProcessLocalReadLocker(cache->_imp.get()));
#endif

    try {

        // Take the read lock on the toc file mapping
        boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
        boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
        bucket->checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);

        {

            // Take the bucket lock in read mode
            boost::scoped_ptr<Sharable_ReadLock> bucketReadLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            bucketReadLock.reset(new Sharable_ReadLock(cache->_imp->ipc->bucketsData[bucket->bucketIndex].bucketMutex));
#else
            createTimedLock<Sharable_ReadLock>(cache->_imp.get(), bucketReadLock, &cache->_imp->ipc->bucketsData[bucket->bucketIndex].bucketMutex);
#endif

            // This function succeeds either if
            // 1) The entry is cached and could be deserialized
            // 2) The entry is pending and thus the caller should call waitForPendingEntry
            // 3) The entry is not computed and thus the caller should compute the entry and call insertInCache
            //
            // This function returns false if the thread must take over the entry computation or the deserialization failed or it need a write lock to deserialize propely.
            // In any case, it should do so under the write lock below.
            LookUpRetCodeEnum stat = lookupAndSetStatusInternal(false /*hasWriteRights*/, false /*removeIfOOM*/, timeSpentWaiting, timeout);
            switch (stat) {
                    case eLookUpRetCodeFound:
                    return;
                    case eLookUpRetCodeNotFound:
                    case eLookUpRetCodeOutOfMemory:
                    break;
            }

        } // bucketReadLock

        // Concurrency resumes!

        assert(status == CacheEntryLockerBase::eCacheEntryStatusMustCompute ||
               status == CacheEntryLockerBase::eCacheEntryStatusComputationPending);

        // Either we failed to deserialize an entry or the caller timedout.
        // Take an upgradable lock and repeat the look-up.
        // Only a single thread/process can take the upgradable lock.

        int attempt_i = 0;
        while (attempt_i < 2) {
            LookupAndCreateRetCodeEnum stat = lookupAndCreate(tocReadLock, tocWriteLock, timeSpentWaiting, timeout);
            bool ok = false;
            switch (stat) {
                case eLookupAndCreateRetCodeCreated:
                    ok = true;
                    break;
                case eLookupAndCreateRetCodeOutOfToCMemory: {

                    if (persistent) {
                        // Ensure the memory mapping is ok. We grow the file so it contains at least the size needed by the entry
                        // plus some metadata required management algorithm store its own memory housekeeping data.
                        std::size_t entryToCSize = processLocalEntry->getMetadataSize();

                        if (!tocWriteLock) {
                            assert(tocReadLock);
                            tocReadLock.reset();
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                            tocWriteLock.reset(new Sharable_WriteLock(cache->_imp->ipc->bucketsData[bucket->bucketIndex].tocData.segmentMutex));
#else
                            createTimedLock<Sharable_WriteLock>(cache->_imp.get(), tocWriteLock, &cache->_imp->ipc->bucketsData[bucket->bucketIndex].tocData.segmentMutex);
#endif
                            if (!bucket->isToCFileMappingValid()) {
                                bucket->remapToCMemoryFile(*tocWriteLock, entryToCSize);
                            }
                        } else {
                            bucket->growToCFile(*tocWriteLock, entryToCSize);
                        }
                    }
                }   break;
            }
            if (ok) {
                break;
            }
            ++attempt_i;
        }
        // Concurrency resumes here!
    } catch (...) {
        // Any exception caught here means the cache is corrupted
        cache->_imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                                  shmAccess
#endif
                                                  );
    }


} // lookupAndSetStatus

template <bool persistent>
CacheEntryBasePtr
CacheEntryLocker<persistent>::getProcessLocalEntry() const
{
    return _imp->processLocalEntry;
}

template <bool persistent>
CacheEntryLockerBase::CacheEntryStatusEnum
CacheEntryLocker<persistent>::getStatus() const
{
    return _imp->status;
}

template <bool persistent>
bool
CacheEntryLocker<persistent>::isPersistent() const
{
    return persistent;
}

template <>
typename CacheEntryLockerPrivate<false>::InsertRetCodeEnum
CacheEntryLockerPrivate<false>::serializeCacheEntry(EntryType* /*entry*/) {  return CacheEntryLockerPrivate::eInsertRetCodeCreated; }

template <>
typename CacheEntryLockerPrivate<true>::InsertRetCodeEnum
CacheEntryLockerPrivate<true>::serializeCacheEntry(EntryType* entry)
{
    // Serialize the meta-datas in the memory segment
    // the construction of the object may fail if the segment is out of memory.

    try {
        assert(entry->properties.getSegmentManager() == bucket->tocFileManager->get_segment_manager());
        processLocalEntry->toMemorySegment(&entry->properties);

        // Add at the end the hash of the entry so that when deserializing we can check if everything was written correctly first
        entry->properties.setIPCProperty(kNatronIPCPropertyHash, hash);
    } catch (const bip::bad_alloc& /*e*/) {

        // Clear stuff that was already allocated by the entry
        entry->properties.clear();
        return CacheEntryLockerPrivate::eInsertRetCodeOutOfToCMemory;
    }
    return CacheEntryLockerPrivate::eInsertRetCodeCreated;
}

template <bool persistent>
typename CacheEntryLockerPrivate<persistent>::InsertRetCodeEnum
CacheEntryLockerPrivate<persistent>::insertInternal()
{


    // Take write lock on the bucket
    boost::scoped_ptr<Sharable_WriteLock> writeLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
    writeLock.reset(new Sharable_WriteLock(cache->_imp->ipc->bucketsData[bucket->bucketIndex].bucketMutex));
#else
    createTimedLock<Sharable_WriteLock>(cache->_imp.get(), writeLock, &cache->_imp->ipc->bucketsData[bucket->bucketIndex].bucketMutex);
#endif

    // Ensure the bucket is in a valid state.
    BucketStateHandler_RAII<persistent> bucketStateHandler(bucket);


    // Fetch the entry. It should be here unless the cache was wiped in between the lookupAndSetStatus and this function.
    typename EntriesMap::iterator cacheEntryIt;
    EntriesMap* storage;
    if (!bucket->tryCacheLookupImpl(hash, &cacheEntryIt, &storage)) {
        return CacheEntryLockerPrivate::eInsertRetCodeCreated;
    }

    // The status of the memory segment entry should be pending because we are the thread computing it.
    // All other threads are waiting.
    // It may be possible that the entry is marked eEntryStatusReady if there was a recursion, in which case the
    // computeThreadMagic should have been set to 0 in insertInCache
    assert(cacheEntryIt->second->status == MemorySegmentEntryHeaderBase::eEntryStatusPending || cacheEntryIt->second->computeThreadMagic == 0);
    if (cacheEntryIt->second->computeThreadMagic == 0) {
        status = CacheEntryLockerBase::eCacheEntryStatusCached;
        return CacheEntryLockerPrivate::eInsertRetCodeCreated;
    }
    // The cacheEntry fields should be uninitialized
    // This may throw an exception if out of memory or if the getMetadataSize function does not return
    // enough memory to encode all the data.


    if (persistent) {
        InsertRetCodeEnum retCode = serializeCacheEntry(cacheEntryIt->second.get());
        if (retCode != CacheEntryLockerPrivate::eInsertRetCodeCreated) {
            return retCode;
        }
    }
    
    
    // Record the memory taken by the entry in the bucket
    bucket->ipc->size += cacheEntryIt->second->size;

    // Insert the hash in the LRU linked list
    // Lock the LRU list mutex
    {
        boost::scoped_ptr<ExclusiveLock> lruWriteLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        lruWriteLock.reset(new ExclusiveLock(cache->_imp->ipc->bucketsData[bucket->bucketIndex].lruListMutex));
#else
        createTimedLock<ExclusiveLock>(cache->_imp.get(), lruWriteLock, &cache->_imp->ipc->bucketsData[bucket->bucketIndex].lruListMutex);
#endif

        cacheEntryIt->second->lruNode.prev = 0;
        cacheEntryIt->second->lruNode.next = 0;
        cacheEntryIt->second->lruNode.hash = hash;

        LRUListNodePtr thisNodePtr = LRUListNodePtr(&cacheEntryIt->second->lruNode);
        if (!bucket->ipc->lruListBack) {
            assert(!bucket->ipc->lruListFront);
            // The list is empty, initialize to this node
            bucket->ipc->lruListFront = thisNodePtr;
            bucket->ipc->lruListBack = thisNodePtr;
            assert(!bucket->ipc->lruListFront->prev && !bucket->ipc->lruListFront->next);
            assert(!bucket->ipc->lruListBack->prev && !bucket->ipc->lruListBack->next);
        } else {
            // Append to the tail of the list
            assert(bucket->ipc->lruListFront && bucket->ipc->lruListBack);

            insertLinkedListNode(thisNodePtr, bucket->ipc->lruListBack, LRUListNodePtr(0));
            // Update back node
            bucket->ipc->lruListBack = thisNodePtr;

        }
    } // lruWriteLock
    cacheEntryIt->second->computeThreadMagic = 0;
    cacheEntryIt->second->status = MemorySegmentEntryHeaderBase::eEntryStatusReady;

    status = CacheEntryLockerBase::eCacheEntryStatusCached;
    
#ifdef CACHE_TRACE_ENTRY_ACCESS
    qDebug() << QThread::currentThread() << "(locker=" << this << ")"<< hash << ": entry inserted in cache";
#endif

    return CacheEntryLockerPrivate::eInsertRetCodeCreated;

} // insertInternal

template <bool persistent>
void
CacheEntryLocker<persistent>::insertInCache()
{
    // The entry should only be computed and inserted in the cache if the status
    // of the object was eCacheEntryStatusMustCompute
    assert(_imp->status == eCacheEntryStatusMustCompute);

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmAccess(new SharedMemoryProcessLocalReadLocker(_imp->cache->_imp.get()));
#endif

    try {
        // Take the read lock on the toc file mapping
        boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
        boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
        _imp->bucket->checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);

        bool ok = false;
        int attempt_i = 0;
        while (attempt_i < 2) {

            typename CacheEntryLockerPrivate<persistent>::InsertRetCodeEnum stat = _imp->insertInternal();
            switch (stat) {
                case CacheEntryLockerPrivate<persistent>::eInsertRetCodeCreated:
                    ok = true;
                    break;
                case CacheEntryLockerPrivate<persistent>::eInsertRetCodeFailed:
                    break;
                case CacheEntryLockerPrivate<persistent>::eInsertRetCodeOutOfToCMemory:
                    break;
            }
            if (ok) {
                break;
            }

            // Ensure we have the ToC write mutex
            if (!tocWriteLock) {
                tocReadLock.reset();
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                tocWriteLock.reset(new Sharable_WriteLock(_imp->cache->_imp->ipc->bucketsData[_imp->bucket->bucketIndex].tocData.segmentMutex));
#else
                createTimedLock<Sharable_WriteLock>(_imp->cache->->_imp.get(), tocWriteLock, &_imp->cache->_imp->ipc->bucketsData[_imp->bucket->bucketIndex].tocData.segmentMutex);
#endif
            }
            // Grow the file
            _imp->bucket->growToCFile(*tocWriteLock, 0);

            ++attempt_i;
        }
        if (!ok) {
            return;
        }

        // We just inserted something, ensure the cache size remains reasonable.
        // We cannot block here until the memory stays contained in the user requested memory portion:
        // if we would do so, then it could deadlock: Natron could require more memory than what
        // the user requested. The workaround here is to evict least recently used entries from the cache
        // in a separate thread.
        appPTR->checkCachesMemory();
    } catch (...) {
        // Any exception caught here means the cache is corrupted
        _imp->cache->_imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                                   shmAccess
#endif
                                                  );
    }
    

} // insertInCache

template <bool persistent>
CacheEntryLockerBase::CacheEntryStatusEnum
CacheEntryLocker<persistent>::waitForPendingEntry(std::size_t timeout)
{

    // The thread can only wait if the status was set to eCacheEntryStatusComputationPending
    assert(_imp->status == eCacheEntryStatusComputationPending);
    assert(_imp->processLocalEntry);

    // If this thread is a threadpool thread, it may wait for a while that results gets available.
    // Release the thread to the thread pool so that it may use this thread for other runnables
    // and reserve it back when done waiting.
    bool hasReleasedThread = false;
    if (isRunningInThreadPoolThread()) {
        QThreadPool::globalInstance()->releaseThread();
        hasReleasedThread = true;
    }

    //
    // To correctly prevent other thread/processes to not try to compute the same cache entry some form of locking is
    // required:
    //
    // Cache::get() --> Take a write lock on the entry if it does not exist
    // CacheEntryLocker::insertInCache() --> Release the write lock taken in get()
    //
    // Since the cache is persistent, the entries in the cache contain only interprocess compliant data structures.
    // That means the entry lock should be an interprocess mutex. However if we were to place an interprocess mutex in a
    // MemorySegmentEntryHeader this would introduce quite a few complexities:
    // We would need to keep the read lock on the memory file (tocData.segmentMutex) alive while we wait because if the memory file
    // gets remapped the cache entry mutex would become invalid.
    // Locking 2 locks with such pattern is almost doomed to produce a deadlock at some point if another thread wants to grow the
    // memory files (hence take the memory segment mutex in write mode)
    //
    //
    // Instead we chose a "polling" method: we lookup the entry every X ms: this has the advantage not to retain any cache mutex
    // so the amount of time we wait is really just imparing this thead rather than the whole cache bucket.

    std::size_t timeSpentWaitingForPendingEntryMS = 0;
    std::size_t timeToWaitMS = 20;

    do {
        // Look up the cache and sleep if not found
        _imp->lookupAndSetStatus(&timeSpentWaitingForPendingEntryMS, timeout);

        if (_imp->status == eCacheEntryStatusComputationPending) {

            timeSpentWaitingForPendingEntryMS += timeToWaitMS;
            if (timeout == 0 || timeSpentWaitingForPendingEntryMS < timeout) {
                CacheEntryLockerBase::sleep_milliseconds(timeToWaitMS);

                // Increase the time to wait at the next iteration
                timeToWaitMS *= 1.2;

            }
        }

    } while(_imp->status == eCacheEntryStatusComputationPending);

    // Concurrency resumes!

    if (hasReleasedThread) {
        QThreadPool::globalInstance()->reserveThread();
    }
    return _imp->status;
} // waitForPendingEntry

template <bool persistent>
CacheEntryLocker<persistent>::~CacheEntryLocker()
{
#ifdef CACHE_TRACE_ENTRY_ACCESS
    qDebug() << QThread::currentThread() <<  "(locker=" << this << ")"<< _imp->hash << ": destroying locker object";
#endif
    // If cached, we don't have to release any data
    if (_imp->status == eCacheEntryStatusCached) {
        return;
    }

    // The cache entry is still pending: the caller thread did not call waitForPendingEntry() nor
    // insertInCache().
    // Release the entry from the cache if we should be computing it
    if (_imp->status == eCacheEntryStatusMustCompute) {

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
        SHMReadLockerPtr shmAccess(new SharedMemoryProcessLocalReadLocker(_imp->cache->_imp.get()));
#endif
        try {
            // Take the read lock on the toc file mapping
            boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
            boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
            _imp->bucket->checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);

            // Take write lock on the bucket
            boost::scoped_ptr<Sharable_WriteLock> writeLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            writeLock.reset(new Sharable_WriteLock(_imp->cache->_imp->ipc->bucketsData[_imp->bucket->bucketIndex].bucketMutex));
#else
            createTimedLock<Sharable_WriteLock>(_imp->cache->_imp.get(), writeLock, &_imp->cache->_imp->ipc->bucketsData[_imp->bucket->bucketIndex].bucketMutex);
#endif

            // Ensure the bucket is in a valid state.
            BucketStateHandler_RAII<persistent> bucketStateHandler(_imp->bucket);


            typename CacheEntryLockerPrivate<persistent>::EntriesMap::iterator cacheEntryIt;
            typename CacheEntryLockerPrivate<persistent>::EntriesMap* storage;
            if (!_imp->bucket->tryCacheLookupImpl(_imp->hash, &cacheEntryIt, &storage)) {
                // The cache may have been wiped in between
                return;
            }

            _imp->bucket->deallocateCacheEntryImpl(cacheEntryIt, storage);

        } catch (...) {
            // Any exception caught here means the cache is corrupted
            _imp->cache->_imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                                            shmAccess
#endif
                                                            );
        }
    }
} // ~CacheEntryLocker


template <bool persistent>
Cache<persistent>::Cache(bool enableTileStorage)
: _imp(new CachePrivate<persistent>(this, enableTileStorage))
{

}
template <bool persistent>
Cache<persistent>::~Cache()
{

}

template <bool persistent>
std::string
CachePrivate<persistent>::getSharedMemoryName() const
{

    std::stringstream ss;
    ss << NATRON_APPLICATION_NAME << NATRON_CACHE_DIRECTORY_NAME  << "SHM";
    return ss.str();

}

template <bool persistent>
std::size_t
CachePrivate<persistent>::getSharedMemorySize() const
{
    // Allocate 500KB rounded to page size for the global data.
    // This gives the global memory segment a little bit of room for its own housekeeping of memory.
    std::size_t pageSize = bip::mapped_region::get_page_size();
    std::size_t desiredSize = 500 * 1024;
    desiredSize = std::ceil(desiredSize / (double)pageSize) * pageSize;
    return desiredSize;
}

template <bool persistent>
void
Cache<persistent>::initialize(const boost::shared_ptr<Cache<persistent> >& thisShared)
{
    if (persistent) {
        // Open or create the file lock

        _imp->initializeCacheDirPath();
        _imp->ensureCacheDirectoryExists();

        std::string cacheDir;
        {
            std::stringstream ss;
            ss << _imp->directoryContainingCachePath << "/" << NATRON_CACHE_DIRECTORY_NAME << "/";
            cacheDir = ss.str();
        }
        std::string fileLockFile = cacheDir + "Lock";

        // Ensure the file lock file exists in read/write mode
        {
            FStreamsSupport::ofstream ofile;
            FStreamsSupport::open(&ofile, fileLockFile);
            if (!ofile || fileLockFile.empty()) {
                assert(false);
                std::string message = "Failed to open file: " + fileLockFile;
                throw std::runtime_error(message);
            }

            try {
                _imp->globalFileLock.reset(new bip::file_lock(fileLockFile.c_str()));
            } catch (...) {
                assert(false);
                throw std::runtime_error("Failed to initialize shared memory file lock, exiting.");
            }
        }
    } // persistent

    // Take the file lock in write mode:
    //      - If it succeeds, that means no other process is active: We remove the globalMemorySegment shared memory segment
    //        and create a new one, to ensure no lock was left in a bad state. Then we release the file lock
    //      - If it fails, another process is still actively using the globalMemorySegment shared memory: it must still be valid
    bool gotFileLock = true;
    if (persistent) {
        gotFileLock = _imp->globalFileLock->try_lock();


#ifndef NATRON_CACHE_INTERPROCESS_ROBUST

        // If we did not get the file lock
        if (!gotFileLock) {
            std::cerr << "Another " << NATRON_APPLICATION_NAME << " process is active, this process will fallback on a local cache instead of a persistent cache." << std::endl;
            _imp->globalFileLock.reset();
            throw BusyCacheException();
        }
#else
        // Create 2 semaphores used to ensure the integrity of the shared memory segment holding interprocess mutexes.
        std::string semValidStr, semInvalidStr;
        {
            std::string semBaseName;
            {
                std::stringstream ss;
                ss << NATRON_APPLICATION_NAME << NATRON_CACHE_DIRECTORY_NAME;
                semBaseName = ss.str();
            }
            semValidStr = std::string(semBaseName + "nSHMValidSem");
            semInvalidStr = std::string(semBaseName + "nSHMInvalidSem");
        }
        try {
            if (gotFileLock) {
                // Remove the semaphore if we are the only process alive to ensure its state.
                bip::named_semaphore::remove(semValidStr.c_str());
            }
            _imp->nSHMValidSem.reset(new bip::named_semaphore(bip::open_or_create,
                                                                   semValidStr.c_str(),
                                                                   0));
            if (gotFileLock) {
                // Remove the semaphore if we are the only process alive to ensure its state.
                bip::named_semaphore::remove(semInvalidStr.c_str());
            }
            _imp->nSHMInvalidSem.reset(new bip::named_semaphore(bip::open_or_create,
                                                                     semInvalidStr.c_str(),
                                                                     0));
        } catch (...) {
            assert(false);
            throw std::runtime_error("Failed to initialize named semaphores, exiting.");
        }
#endif // NATRON_CACHE_INTERPROCESS_ROBUST

    } // persistent



    // Create the main memory segment containing the CacheIPCData
    {
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        _imp->ipc.reset(new CacheIPCData);
#else
        std::size_t desiredSize = _imp->getSharedMemorySize();
        std::string sharedMemoryName = _imp->getSharedMemoryName();
        try {
            if (gotFileLock) {
                bip::shared_memory_object::remove(sharedMemoryName.c_str());
            }
            _imp->globalMemorySegment.reset(new bip::managed_shared_memory(bip::open_or_create, sharedMemoryName.c_str(), desiredSize));
            _imp->ipc = _imp->globalMemorySegment->find_or_construct<CacheIPCData>("CacheData")();
        } catch (...) {
            assert(false);
            bip::shared_memory_object::remove(sharedMemoryName.c_str());
            throw std::runtime_error("Failed to initialize managed shared memory, exiting.");
        }
#endif

    }

    if (persistent && gotFileLock) {
        _imp->globalFileLock->unlock();
        // Indicate that we use the shared memory by taking the file lock in read mode.
        if (_imp->globalFileLock) {
            _imp->globalFileLock->lock_sharable();
        }

    }


    // Open each bucket individual memory segment.
    // They are not created in shared memory but in a memory mapped file instead
    // to be persistent when the OS shutdown.
    // Each segment controls the table of content of the bucket.
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmReader(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif


    for (int i = 0; i < NATRON_CACHE_BUCKETS_COUNT; ++i) {

        // Hold a weak pointer to the cache on the bucket
        _imp->buckets[i].cache = thisShared;
        _imp->buckets[i].bucketIndex = i;
        

        _imp->buckets[i].tocFile.reset(new typename CacheBucket<persistent>::StorageType);

        if (persistent) {
            // Get the bucket directory path. It ends with a separator.
            QString bucketDirPath = _imp->getBucketAbsoluteDirPath(i);

            std::string tocFilePath = bucketDirPath.toStdString() + "Index";
            openStorage(_imp->buckets[i].tocFile, tocFilePath, (int)MemoryFile::eFileOpenModeOpenOrCreate);
            
        }
        
        
    } // for each bucket

    // Remap each bucket, this may potentially fail
    for (int i = 0; i < NATRON_CACHE_BUCKETS_COUNT; ++i) {
        try {

            boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
            {
                // Take the ToC mapping mutex
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                tocWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[i].tocData.segmentMutex));
#else
                createTimedLock<Sharable_WriteLock>(_imp.get(), tocWriteLock, &_imp->ipc->bucketsData[i].tocData.segmentMutex);
#endif

                _imp->buckets[i].remapToCMemoryFile(*tocWriteLock, 0);
            }
        } catch (...) {
            // Any exception caught here means the cache is corrupted
            _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                               shmReader
#endif
                                               );

        }
    } // for each bucket

    if (persistent) {


        try {

            boost::scoped_ptr<Sharable_WriteLock> writeLock;
            // Take the tilesStorageMutex in read mode to indicate that we are operating on it (flush)
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            writeLock.reset(new Sharable_WriteLock(_imp->ipc->tilesStorageMutex));
#else
            // Take read lock on the tile data
            createTimedLock<Sharable_WriteLock>(_imp.get(), writeLock, _imp->ipc->tilesStorageMutex);
#endif
            _imp->reOpenTileStorage();
            if (_imp->tilesStorage.empty()) {
                // Ensure we initialize the cache with at least one tile storage file
                _imp->createTileStorage();
            }
        } catch (const CorruptedCacheException&) {
            clear();
        }
    } // persistent
    
    

} // initialize

template <bool persistent>
CacheBasePtr
Cache<persistent>::create(bool enableTileStorage)
{
    boost::shared_ptr<Cache<persistent> > ret (new Cache<persistent>(enableTileStorage));
    ret->initialize(ret);
    return ret;
} // create

struct CacheTilesLockImpl
{
    // Protects the shared memory segment so that mutexes stay valid
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmAccess;
#endif

    // Mutex that protects access to the tiles memory mapped file
    boost::scoped_ptr<Sharable_ReadLock> tileReadLock;
    boost::scoped_ptr<Sharable_WriteLock> tileWriteLock;

    CacheTilesLockImpl()
    {

    }

    ~CacheTilesLockImpl()
    {
        // ensure the lock is released before the shm access
        tileReadLock.reset();
        tileWriteLock.reset();
    }


};

template <bool persistent>
void
CachePrivate<persistent>::createTileStorage()
{
    if (!useTileStorage) {
        return;
    }
    // The lock must be taken in write mode
    assert(!ipc->tilesStorageMutex.try_lock());

    StoragePtrType data(new StorageType);
    if (persistent) {
        std::stringstream ss;
        ss << directoryContainingCachePath << "/" <<  NATRON_CACHE_DIRECTORY_NAME << "/TilesStorage" << tilesStorage.size() + 1;
        openStorage(data, ss.str(), (int)MemoryFile::eFileOpenModeOpenOrCreate);
    }
    resizeStorage(data, NATRON_TILE_STORAGE_FILE_SIZE);

    U64 fileIndex = tilesStorage.size();
    tilesStorage.push_back(data);

    // The number of tiles should be a multiple of the buckets count
    assert(NATRON_NUM_TILES_PER_FILE % NATRON_CACHE_BUCKETS_COUNT == 0);

#ifdef CACHE_TRACE_TILES_ALLOCATION
    std::cout << "=============================================\nFree tiles state:\n\n";
#endif

    for (int bucket_i = 0; bucket_i < NATRON_CACHE_BUCKETS_COUNT; ++bucket_i) {

        boost::scoped_ptr<Sharable_WriteLock> bucketWriteLock;
        boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
        boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;

        // Take the ToC write lock
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        tocReadLock.reset(new Sharable_ReadLock(ipc->bucketsData[bucket_i].tocData.segmentMutex));
#else
        createTimedLock<Sharable_ReadLock>(_imp.get(), *tocReadLock, &ipc->bucketsData[bucket_i].tocData.segmentMutex);
#endif


        // Take the bucket mutex
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        bucketWriteLock.reset(new Sharable_WriteLock(ipc->bucketsData[bucket_i].bucketMutex));
#else
        createTimedLock<Sharable_WriteLock>(_imp.get(), *bucketWriteLock, &ipc->bucketsData[bucket_i].bucketMutex);
#endif
        
        
        
#ifdef CACHE_TRACE_TILES_ALLOCATION
        std::cout << "[" << bucket_i << "] = " << buckets[bucket_i].ipc->freeTiles.size();
        if (bucket_i < NATRON_CACHE_BUCKETS_COUNT - 1) {
            std::cout << " , ";
        }
#endif

        // Insert the new available tiles in the freeTiles set.
        // First insert in a temporary set and then assign to the free tiles set to avoid out of memory exceptions
        std::set<U64> tmpSet;
        tmpSet.insert(buckets[bucket_i].ipc->freeTiles.begin(), buckets[bucket_i].ipc->freeTiles.end());
        U64 nTiles = bucket_i * NATRON_NUM_TILES_PER_BUCKET_FILE;
        for (U64 i = nTiles; i < nTiles + NATRON_NUM_TILES_PER_BUCKET_FILE; ++i) {
            U64 encodedIndex = 0;
            encodedIndex = ((i << 32) | fileIndex);
            tmpSet.insert(encodedIndex);
        }
        {

            int nAttempts = 0;
            while (nAttempts < 2) {
                try {
                    buckets[bucket_i].ipc->freeTiles.clear();
                    buckets[bucket_i].ipc->freeTiles.insert(tmpSet.begin(), tmpSet.end());
                    break;
                } catch (const bip::bad_alloc&) {

                    // We may not have enough memory to store all indices, so grow the ToC mapping
                    std::size_t tocMemNeeded = tmpSet.size() * sizeof(U64) * 2;

                    if (!tocWriteLock) {

                        // Release the bucket lock: it's only guarantee to be valid is while under the toc lock!
                        bucketWriteLock.reset();
                        tocReadLock.reset();
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                        tocWriteLock.reset(new Sharable_WriteLock(ipc->bucketsData[bucket_i].tocData.segmentMutex));
#else
                        createTimedLock<Sharable_WriteLock>(c->_imp.get(), tocWriteLock, &ipc->bucketsData[bucket_i].tocData.segmentMutex);
#endif
                    }

                    buckets[bucket_i].growToCFile(*tocWriteLock, tocMemNeeded);

                    // Take back the bucket mutex
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                    bucketWriteLock.reset(new Sharable_WriteLock(ipc->bucketsData[bucket_i].bucketMutex));
#else
                    createTimedLock<Sharable_WriteLock>(_imp.get(), *bucketWriteLock, &ipc->bucketsData[bucket_i].bucketMutex);
#endif
                }
                ++nAttempts;
            }
        }

    } // for each bucket
#ifdef CACHE_TRACE_TILES_ALLOCATION
    std::cout << "\n=============================================" << std::endl;
#endif


} // createTileStorage

template <>
void
CachePrivate<false>::reOpenTileStorage() {}

template <>
void
CachePrivate<true>::reOpenTileStorage()
{
    // The lock must be taken in write mode
    assert(!ipc->tilesStorageMutex.try_lock());
    QString dirPath;
    {
        std::stringstream ss;
        ss << directoryContainingCachePath << "/" <<  NATRON_CACHE_DIRECTORY_NAME;
        dirPath = QString::fromUtf8(ss.str().c_str());
    }
    QDir d(dirPath);
    QStringList nameFilters;
    nameFilters.push_back(QString::fromUtf8("TilesStorage*"));
    QStringList files = d.entryList(nameFilters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name /*sort by name*/);
    files.sort();
    for (QStringList::iterator it = files.begin(); it != files.end(); ++it) {
        MemoryFilePtr data(new MemoryFile);
        std::string filePath = dirPath.toStdString() + "/" + it->toStdString();
        (data)->open(filePath, MemoryFile::eFileOpenModeOpenOrCreate);
        if ((data)->size() != NATRON_TILE_STORAGE_FILE_SIZE) {
            (data)->resize(NATRON_TILE_STORAGE_FILE_SIZE, false);
        }
        tilesStorage.push_back(data);
    }

}

static int getBucketIndexForTile(U64 entryHash, U64 tileIndex)
{
    return CacheBase::getBucketCacheBucketIndex(entryHash + tileIndex);
    //(Cache::getBucketCacheBucketIndex(entryHash) + (*tilesToAlloc)[i]) % NATRON_CACHE_BUCKETS_COUNT;
}

template <bool persistent>
void
CachePrivate<persistent>::freeAllocatedTiles(U64 entryHash, const std::vector<U64>& tilesToAlloc, const std::vector<std::pair<U64, void*> >& allocatedTiles)
{
    // If somehow the cache entry is no longer in the cache, we must make free again all tile indices
    for (std::size_t i = 0; i < tilesToAlloc.size(); ++i) {

        // Recover the bucket index for this tile
        int bucketIndex = getBucketIndexForTile(entryHash, tilesToAlloc[i]);
        CacheBucket<persistent>& tileBucket = buckets[bucketIndex];

        boost::scoped_ptr<Sharable_WriteLock> bucketWriteLock;
        boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
        boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;

        // Take the read lock on the toc file mapping for the bucket
        tileBucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);

        // Lock the bucket in write mode to edit the freeTiles list
        if (!bucketWriteLock) {
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            bucketWriteLock.reset(new Sharable_WriteLock(ipc->bucketsData[bucketIndex].bucketMutex));
#else
            createTimedLock<Sharable_WriteLock>(_imp.get(), bucketWriteLock, &_imp->ipc->bucketsData[bucketIndex].bucketMutex);
#endif
        }

        // Re-insert the tile index in the freeTiles list. Since we are adding data, this may throw an exception
        // because the ToC might run out of memory. In this case, we grow it and try again.
        tileBucket.ipc->freeTiles.insert(allocatedTiles[i].first);

    } // for each allocated tile

} // freeAllocatedTiles


template <bool persistent>
bool
Cache<persistent>::retrieveAndLockTiles(const CacheEntryBasePtr& entry,
                                        const std::vector<U64>* tileIndices,
                                        const std::vector<U64>* tilesToAlloc,
                                        std::vector<void*>* existingTilesData,
                                        std::vector<std::pair<U64, void*> >* allocatedTilesData,
                                        void** cacheData)
{
    assert(_imp->useTileStorage);
    assert(cacheData);
    *cacheData = 0;

    if ((!tileIndices || tileIndices->empty()) && (!tilesToAlloc || tilesToAlloc->empty())) {
        // Nothing to do
        return true;
    }
    // Get the bucket corresponding to the hash
    // Each tile gets a different hash since all tiles of an image share the same base hash
    // The tile hash is determined by the hash passed in tilesToAlloc.
    // This hash is produced with tile x + tile y + mipmaplevel
    U64 entryHash = entry->getHashKey();

    // Since this function returns pointers to the underlying memory mapped file, we need to hold the tilesStorageMutex
    // in read mode whilst the user is using it, we let him free the cacheData using the unLockTiles() function once he is done
    // with the pointers.
    CacheTilesLockImpl* tilesLock = new CacheTilesLockImpl;
    *cacheData = tilesLock;

    // Catch corrupted cache or abandonned mutex exceptions
    try {

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
        // Public function, the SHM must be locked.
        tilesLock->shmAccess.reset(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif

        // Take the tilesStorageMutex in read mode to indicate that we are operating on it
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        tilesLock->tileReadLock.reset(new Sharable_ReadLock(_imp->ipc->tilesStorageMutex));
#else
        // Take read lock on the tile data
        createTimedLock<Sharable_ReadLock>(_imp.get(), tilesLock->tileReadLock, &_imp->ipc->tilesStorageMutex);
#endif


        if (tilesToAlloc && tilesToAlloc->size() > 0) {
            allocatedTilesData->resize(tilesToAlloc->size());

            for (std::size_t i = 0; i < tilesToAlloc->size(); ++i) {

                // The bucket index for the tile depends on the bucket of the cache entry + a number based off the tile index so that
                // we ensure that we distribute uniformly all tiles across buckets.
                int bucketIndex = getBucketIndexForTile(entryHash, (*tilesToAlloc)[i]);

                CacheBucket<persistent>& tileBucket = _imp->buckets[bucketIndex];

                boost::scoped_ptr<Sharable_WriteLock> bucketWriteLock;
                boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
                boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;


                // Take the read lock on the toc file mapping of the bucket
                if (!tocReadLock && !tocWriteLock) {
                    tileBucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);
                }

                // Lock the bucket in write mode to edit the freeTiles list
                if (!bucketWriteLock) {
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                    bucketWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[bucketIndex].bucketMutex));
#else
                    createTimedLock<Sharable_WriteLock>(_imp.get(), bucketWriteLock, &_imp->ipc->bucketsData[bucketIndex].bucketMutex);
#endif
                }


                if (tileBucket.ipc->freeTiles.empty()) {
                    // No free tile in the bucket: make a new file
                    // To create a file, we need a write lock on the tiles storage
                    if (!tilesLock->tileWriteLock) {
                        tilesLock->tileReadLock.reset();
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                        tilesLock->tileWriteLock.reset(new Sharable_WriteLock(_imp->ipc->tilesStorageMutex));
#else
                        createTimedLock<Sharable_WriteLock>(_imp.get(), tilesLock->tileWriteLock, &_imp->ipc->tilesStorageMutex);
#endif
                    }

                    // Release the locks before calling createTileStorage
                    tocReadLock.reset();
                    tocWriteLock.reset();
                    bucketWriteLock.reset();

                    _imp->createTileStorage();

                    // Take back the locks
                    if (!tocReadLock && !tocWriteLock) {
                        tileBucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);
                    }

                    if (!bucketWriteLock) {
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                        bucketWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[bucketIndex].bucketMutex));
#else
                        createTimedLock<Sharable_WriteLock>(_imp.get(), bucketWriteLock, &_imp->ipc->bucketsData[bucketIndex].bucketMutex);
#endif
                    }

                }

                // Extract the first free tile
                assert(tileBucket.ipc->freeTiles.size() >= 1);
                U64 freeTileEncodedIndex;
                {
                    U64_Set::iterator freeTileIt = tileBucket.ipc->freeTiles.begin();
                    freeTileEncodedIndex = *freeTileIt;
                    tileBucket.ipc->freeTiles.erase(freeTileIt);
#ifdef CACHE_TRACE_TILES_ALLOCATION
                    qDebug() << "Bucket" << bucketIndex << ": removing tile" << freeTileEncodedIndex << " Nb free tiles left:" << tileBucket.ipc->freeTiles.size();
#endif
                }


                // Get the pointer to the data corresponding to the free tile index
                U32 fileIndex, tileIndex;
                getTileIndex(freeTileEncodedIndex, &tileIndex, &fileIndex);
                typename CachePrivate<persistent>::StoragePtrType* storage = 0;
                if (fileIndex < _imp->tilesStorage.size()) {
                    storage = &_imp->tilesStorage[fileIndex];
                }
                if (!storage) {
                    assert(false);
                    return false;
                }

                char* data = (*storage)->getData();

                // Set the tile index on the entry so we can free it afterwards.
                char* ptr = data + tileIndex * NATRON_TILE_SIZE_BYTES;
                assert((ptr >= data) && (ptr < (data + NATRON_NUM_TILES_PER_FILE * NATRON_TILE_SIZE_BYTES)));
                (*allocatedTilesData)[i] = std::make_pair(freeTileEncodedIndex, ptr);

            } // for each tile to allocate
        } // tilesToAlloc

        // Now for each tile to allocate, add the tile cache indices to the corresponding cache entry so that when deallocating
        // the entry, we can also properly free the tiles.
        if (tilesToAlloc && tilesToAlloc->size() > 0) {

            typename CacheBucket<persistent>::EntryType* cacheEntry = 0;
            int cacheEntryBucketIndex = Cache::getBucketCacheBucketIndex(entryHash);

            boost::scoped_ptr<Sharable_WriteLock> bucketWriteLock;
            boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
            boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;

            {
                // The entry must exist in the cache to be able to allocate tiles!
                typename CacheBucket<persistent>::EntriesMap* storage;
                typename CacheBucket<persistent>::EntriesMap::iterator found;
                CacheBucket<persistent>& bucket = _imp->buckets[cacheEntryBucketIndex];


                // Take the read lock on the toc file mapping of the bucket
                bucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);

                // Lock the bucket in write mode, we are going to write to the tiles list of the entry
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                bucketWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[cacheEntryBucketIndex].bucketMutex));
#else
                createTimedLock<Sharable_WriteLock>(_imp.get(), bucketWriteLock, &_imp->ipc->bucketsData[cacheEntryBucketIndex].bucketMutex);
#endif

                // Look-up the cache entry
                bool gotEntry = bucket.tryCacheLookupImpl(entryHash, &found, &storage);
                if (!gotEntry) {

                    // The entry does no longer exist in the cache... A possible explanation is that the cache was wiped just prior to
                    // this function call.
                    // Release the write lock on the bucket and cycle through each tile to free them.
                    bucketWriteLock.reset();

                    // If somehow the cache entry is no longer in the cache, we must make free again all tile indices
                    _imp->freeAllocatedTiles(entryHash, *tilesToAlloc, *allocatedTilesData);

                    return false;
                }
                cacheEntry = found->second.get();

                // Increment the size of the entry in the cache
                bucket.ipc->size += tilesToAlloc->size() * NATRON_TILE_SIZE_BYTES;
            }

            // Actually add the allocated tile indices in the cache entry so that we can free them when the cache entry gets destroyed.
            // Since we are adding data, this may throw an exception
            // because the ToC might run out of memory. In this case, we grow it and try again.

            // First work on a local set on the heap and then copy it to the ToC
            std::vector<U64> tmpSet(cacheEntry->tileIndices.size() + tilesToAlloc->size());
            {
                int i = 0;
                for (ExternalSegmentTypeULongLongList::const_iterator it = cacheEntry->tileIndices.begin(); it != cacheEntry->tileIndices.end(); ++it, ++i) {
                    tmpSet[i] = *it;
                }
                for (std::size_t c = 0; c < tilesToAlloc->size(); ++c, ++i) {
                    tmpSet[i] = (*allocatedTilesData)[c].first;
                }
            }


            int nAttempts = 0;
            while (nAttempts < 2) {

                try {
                    cacheEntry->tileIndices.clear();
                    cacheEntry->tileIndices.insert(cacheEntry->tileIndices.end(), tmpSet.begin(), tmpSet.end());
                    break;
                } catch (const bip::bad_alloc&) {

                    // We may not have enough memory to store all indices, so grow the ToC mapping
                    std::size_t tocMemNeeded = tmpSet.size() * sizeof(U64) * 2;

                    // Release the bucket mutex because it will become invalid while we grow the ToC file
                    bucketWriteLock.reset();

                    // Ensure we have the ToC write mutex
                    if (!tocWriteLock) {
                        tocReadLock.reset();
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                        tocWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[cacheEntryBucketIndex].tocData.segmentMutex));
#else
                        createTimedLock<Sharable_WriteLock>(c->_imp.get(), tocWriteLock, &_imp->ipc->bucketsData[cacheEntryBucketIndex].tocData.segmentMutex);
#endif
                    }

                    // Grow the file
                    _imp->buckets[cacheEntryBucketIndex].growToCFile(*tocWriteLock, tocMemNeeded);

                    // Take back the mutex
                    // Lock the bucket in write mode, we are going to write to the tiles list of the entry
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                    bucketWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[cacheEntryBucketIndex].bucketMutex));
#else
                    createTimedLock<Sharable_WriteLock>(_imp.get(), bucketWriteLock, &_imp->ipc->bucketsData[cacheEntryBucketIndex].bucketMutex);
#endif

                    // Look-up the cache entry again: it became invalid when we called growToCFile
                    typename CacheBucket<persistent>::EntriesMap* storage;
                    typename CacheBucket<persistent>::EntriesMap::iterator found;
                    bool gotEntry = _imp->buckets[cacheEntryBucketIndex].tryCacheLookupImpl(entryHash, &found, &storage);
                    if (!gotEntry) {

                        // The entry does no longer exist in the cache... A possible explanation is that the cache was wiped just prior to
                        // this function call.
                        // Release the write lock on the bucket and cycle through each tile to free them.
                        bucketWriteLock.reset();

                        // If somehow the cache entry is no longer in the cache, we must make free again all tile indices
                        _imp->freeAllocatedTiles(entryHash, *tilesToAlloc, *allocatedTilesData);

                        return false;
                    }
                    cacheEntry = found->second.get();

                }
                ++nAttempts;
            }


        } // numTilesToAlloc > 0



        if (tileIndices && !tileIndices->empty()) {
            existingTilesData->resize(tileIndices->size());
            for (std::size_t i = 0; i < tileIndices->size(); ++i) {
                
                U32 fileIndex, tileIndex;
                getTileIndex((*tileIndices)[i], &tileIndex, &fileIndex);
                typename CachePrivate<persistent>::StoragePtrType* storage = 0;
                if (fileIndex < _imp->tilesStorage.size()) {
                    storage = &_imp->tilesStorage[fileIndex];
                }
                if (!storage) {
                    assert(false);
                    return false;
                }


                char* data = (*storage)->getData();
                char* tileDataPtr = data + tileIndex * NATRON_TILE_SIZE_BYTES;
                assert((tileDataPtr >= data) && (tileDataPtr < (data + NATRON_NUM_TILES_PER_FILE * NATRON_TILE_SIZE_BYTES)));
                (*existingTilesData)[i] = tileDataPtr;
            } // for each tile indices
        }
    } catch (...) {

        tilesLock->tileReadLock.reset();
        tilesLock->tileWriteLock.reset();

        // Any exception caught here means the cache is corrupted
        _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                            tilesLock->shmAccess
#endif
                                           );
        delete tilesLock;
        return false;
    }

    return true;

} // retrieveAndLockTiles

#ifdef DEBUG
template <bool persistent>
bool
Cache<persistent>::checkTileIndex(U64 encodedIndex) const
{
    U32 fileIndex, tileIndex;
    getTileIndex(encodedIndex, &tileIndex, &fileIndex);

    // We must be inbetween retrieveAndLockTiles and unLockTiles
    assert(!_imp->ipc->tilesStorageMutex.try_lock());

    if (fileIndex >= _imp->tilesStorage.size()) {
        assert(false);
        return false;
    }
    char* data = _imp->tilesStorage[fileIndex]->getData();
    char* tileDataPtr = data + tileIndex * NATRON_TILE_SIZE_BYTES;
    if (tileDataPtr < data || tileDataPtr >= (data + NATRON_NUM_TILES_PER_FILE * NATRON_TILE_SIZE_BYTES)) {
        assert(false);
        return false;
    }
    return true;
}
#endif // #ifdef DEBUG

template <bool persistent>
void
Cache<persistent>::unLockTiles(void* cacheData)
{
    delete (CacheTilesLockImpl*)cacheData;
} // unLockTiles


template <bool persistent>
void
Cache<persistent>::releaseTiles(const CacheEntryBasePtr& entry, const std::vector<U64>& localIndices, const std::vector<U64>& cacheIndices)
{
    assert(_imp->useTileStorage);
    if (localIndices.empty() || localIndices.size() != cacheIndices.size()) {
        return;
    }

    // Get the bucket corresponding to the hash
    U64 entryHash = entry->getHashKey();

    // Protects the shared memory segment so that mutexes stay valid
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmAccess;
#endif

    // Mutex that protects access to the tiles memory mapped file
    boost::scoped_ptr<Sharable_ReadLock> tileReadLock;
    boost::scoped_ptr<Sharable_WriteLock> tileWriteLock;


    try {

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
        // Public function, the SHM must be locked.
        tilesLock->shmAccess.reset(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif

        // Take the tilesStorageMutex in read mode to indicate that we are operating on it
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        tileReadLock.reset(new Sharable_ReadLock(_imp->ipc->tilesStorageMutex));
#else
        // Take read lock on the tile data
        createTimedLock<Sharable_ReadLock>(_imp.get(), tileReadLock, &_imp->ipc->tilesStorageMutex);
#endif


        {
            int cacheEntryBucketIndex = Cache::getBucketCacheBucketIndex(entryHash);

            // The entry must exist in the cache to be able to allocate tiles!
            typename CacheBucket<persistent>::EntriesMap* storage;
            typename CacheBucket<persistent>::EntriesMap::iterator found;
            CacheBucket<persistent>& bucket = _imp->buckets[cacheEntryBucketIndex];

            boost::scoped_ptr<Sharable_WriteLock> entryBucketWriteLock;
            boost::scoped_ptr<Sharable_ReadLock> entryTocReadLock;
            boost::scoped_ptr<Sharable_WriteLock> entryTocWriteLock;

            bucket.checkToCMemorySegmentStatus(&entryTocReadLock, &entryTocWriteLock);

            // Lock the bucket in write mode, we are going to write to the tiles list of the entry
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            entryBucketWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[cacheEntryBucketIndex].bucketMutex));
#else
            createTimedLock<Sharable_WriteLock>(_imp.get(), bucketWriteLock, &_imp->ipc->bucketsData[cacheEntryBucketIndex].bucketMutex);
#endif

            bool gotEntry = bucket.tryCacheLookupImpl(entryHash, &found, &storage);
            if (gotEntry) {
                // Remove the tile indices from the list of the entry
                typename CacheBucket<persistent>::EntryType* cacheEntry = found->second.get();
                for (std::size_t i = 0; i < cacheIndices.size(); ++i) {
                    ExternalSegmentTypeULongLongList::iterator foundTile = std::find(cacheEntry->tileIndices.begin(), cacheEntry->tileIndices.end(), cacheIndices[i]);
                    if (foundTile != cacheEntry->tileIndices.end()) {
                        cacheEntry->tileIndices.erase(foundTile);
                    }
                }
            }

            // Relase the bucket write lock
            entryBucketWriteLock.reset();

            // If somehow the cache entry is no longer in the cache, we must make free again all tile indices
            for (std::size_t i = 0; i < cacheIndices.size(); ++i) {

                int bucketIndex = getBucketIndexForTile(entryHash, localIndices[i]);
                CacheBucket<persistent>& tileBucket = _imp->buckets[bucketIndex];

                boost::scoped_ptr<Sharable_WriteLock> bucketWriteLock;
                boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
                boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;

                // Take the read lock on the toc file mapping
                tileBucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);


                // Lock the bucket in write mode to edit the freeTiles list
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                bucketWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[bucketIndex].bucketMutex));
#else
                createTimedLock<Sharable_WriteLock>(_imp.get(), bucketWriteLock, &_imp->ipc->bucketsData[bucketIndex].bucketMutex);
#endif

                tileBucket.ipc->freeTiles.insert(cacheIndices[i]);
            }
        }


    } catch (...) {

        tileReadLock.reset();
        tileWriteLock.reset();

        // Any exception caught here means the cache is corrupted
        _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                           tilesLock->shmAccess
#endif
                                           );
    }


} // releaseTiles

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
template <bool persistent>
void
CachePrivate<persistent>::ensureSharedMemoryIntegrity()
{
    // Any operation taking the segmentMutex in the shared memory, must do so with a timeout so we can avoid deadlocks:
    // If a process crashes whilst the segmentMutex is taken, the file lock is ensured to be released but the
    // segmentMutex will remain taken, deadlocking any other process.

    // Multiple threads in this process can time-out, however we just need to remap the shared memory once.
    // Ensure that no other thread is reading
    boost::unique_lock<boost::shared_mutex> processLocalLocker(nThreadsTimedOutFailedMutex);

    // Mark that this thread is in a timeout operation.
    ++nThreadsTimedOutFailed;

    if (nThreadsTimedOutFailed == 1) {
        // If we are the first thread in a timeout, handle it.

        // Unmap the shared memory segment. This is safe to do since we have the nThreadsTimedOutFailedMutex write lock
        globalMemorySegment.reset();

        // The mapping for this process is no longer invalid
        nSHMInvalidSem->post();

        // We release the read lock taken on the globalFileLock
        globalFileLock->unlock();

        {
            // We take the file lock in write mode.
            // The lock is guaranteed to be taken at some point since any active process will eventually timeout on the segmentMutex and release
            // their read lock on the globalFileLock in the unlock call above.
            // We are sure that when the lock is taken, every process has its shared memory segment unmapped.
            bip::scoped_lock<bip::file_lock> writeLocker(*globalFileLock);

            std::string sharedMemoryName = getSharedMemoryName();
            std::size_t sharedMemorySize = getSharedMemorySize();

            if (!nSHMValidSem->try_wait()) {
                // We are the first process to take the write lock.
                // We know at this point that any other process has released its read lock on the globalFileLock
                // and that the globalMemorySegment is no longer mapped anywhere.
                // We thus remove the globalMemorySegment and re-create it and remap it.
                bool ok = bip::shared_memory_object::remove(sharedMemoryName.c_str());

                // The call should succeed since no one else should be using it
                assert(ok);
                (void)ok;

            } else {
                // We are not the first process to take the write lock, hence the globalMemorySegment
                // has been re-created already, so just map it.
                // Increment what was removed in try_wait()
                nSHMValidSem->post();
            }

            try {
                globalMemorySegment.reset(new bip::managed_shared_memory(bip::open_or_create, sharedMemoryName.c_str(), sharedMemorySize));
                ipc = globalMemorySegment->find_or_construct<CacheIPCData>("CacheData")();
            } catch (...) {
                assert(false);
                bip::shared_memory_object::remove(sharedMemoryName.c_str());
                throw std::runtime_error("Failed to initialize managed shared memory, exiting.");
            }


            // Indicate that we mapped the shared memory segment
            nSHMValidSem->post();

            // Decrement the post() that we made earlier
            nSHMInvalidSem->wait();
            
            // Unlock the file lock
        } // writeLocker

        // When the write lock is released we cannot take the globalFileLock in read mode yet, we could block other processes that
        // are still waiting for the write lock.
        // We must wait that every other process has a valid mapping.

        //  nSHMInvalid.try_wait() will return false when all processes have been remapped.
        //  If it returns true, that means another process is still in-between steps 4 and 8, thus we post
        //  what we decremented in try_wait and re-try again.
        while(nSHMInvalidSem->try_wait()) {
            nSHMInvalidSem->post();
        }

    } // nThreadsTimedOutFailed == 1


    // Wait for all timed out threads to go through the the timed out process.
    --nThreadsTimedOutFailed;
    while (nThreadsTimedOutFailed > 0) {
        nThreadsTimedOutFailedCond.wait(processLocalLocker);
    }

} // ensureSharedMemoryIntegrity
#endif // #ifdef NATRON_CACHE_INTERPROCESS_ROBUST

int
CacheBase::getBucketCacheBucketIndex(U64 hash)
{
    return getBucketStorageIndex<7>(hash);
}

bool
CacheBase::fileExists(const std::string& filename)
{
#ifdef _WIN32
    WIN32_FIND_DATAW FindFileData;
    std::wstring wpath = StrUtils::utf8_to_utf16 (filename);
    HANDLE handle = FindFirstFileW(wpath.c_str(), &FindFileData);
    if (handle != INVALID_HANDLE_VALUE) {
        FindClose(handle);

        return true;
    }

    return false;
#else
    // on Unix platforms passing in UTF-8 works
    std::ifstream fs( filename.c_str() );

    return fs.is_open() && fs.good();
#endif
}


template <bool persistent>
void
Cache<persistent>::setMaximumCacheSize(std::size_t size)
{
    std::size_t curSize = getMaximumCacheSize();
    {
        boost::unique_lock<boost::mutex> k(_imp->maximumSizeMutex);
        _imp->maximumSize = size;
    }

    // Clear exceeding entries if we are shrinking the cache.
    if (size < curSize) {
        evictLRUEntries(0);
    }
}

template <bool persistent>
std::size_t
Cache<persistent>::getMaximumCacheSize() const
{
    {
        boost::unique_lock<boost::mutex> k(_imp->maximumSizeMutex);
        return _imp->maximumSize;
    }
}

template <bool persistent>
std::size_t
Cache<persistent>::getCurrentSize() const
{
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmReader(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif

    std::size_t ret = 0;
    for (int i = 0; i < NATRON_CACHE_BUCKETS_COUNT; ++i) {

        try {
            // Take the read lock on the toc file mapping
            boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
            boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
            _imp->buckets[i].checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);


#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            boost::scoped_ptr<Sharable_ReadLock> locker(new Sharable_ReadLock(_imp->ipc->bucketsData[i].bucketMutex));
#else
            boost::scoped_ptr<Sharable_ReadLock> locker(new Sharable_ReadLock(_imp->ipc->bucketsData[i].bucketMutex, _imp->timerFrequency));
            if (!locker->timed_lock(500)) {
                return 0;
            }
#endif
            ret +=  _imp->buckets[i].ipc->size;
            
        } catch (...) {
            // Any exception caught here means the cache is corrupted
            _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                               shmReader
#endif
                                               );
            return 0;
        }
    }


    return ret;


} // getCurrentSize

static std::string getBucketDirName(int bucketIndex)
{
    std::string dirName;
    {
        std::string formatStr;
        {

            std::stringstream ss;
            ss << "%0";
            ss << NATRON_CACHE_BUCKETS_N_DIGITS;
            ss << "x";
            formatStr = ss.str();
        }

        std::ostringstream oss;
        oss <<  boost::format(formatStr) % bucketIndex;
        dirName = oss.str();
    }
    return dirName;
}

static void createIfNotExistBucketDirs(const QDir& d)
{
    // Create a directory for each bucket with the index as name
    for (int i = 0; i < NATRON_CACHE_BUCKETS_COUNT; ++i) {
        QString qDirName = QString::fromUtf8( getBucketDirName(i).c_str() );
        if (!d.exists(qDirName)) {
            d.mkdir(qDirName);
        }
    }

}

template <bool persistent>
void
CachePrivate<persistent>::initializeCacheDirPath()
{
    std::string cachePath = appPTR->getCurrentSettings()->getDiskCachePath();
    // Check that the user provided path exists otherwise fallback on default.
    bool userDirExists;
    if (cachePath.empty()) {
        userDirExists = false;
    } else {
        QString userDirectoryCache = QString::fromUtf8(cachePath.c_str());
        QDir d(userDirectoryCache);
        userDirExists = d.exists();
    }
    if (userDirExists) {
        directoryContainingCachePath = cachePath;
    } else {
        directoryContainingCachePath = StandardPaths::writableLocation(StandardPaths::eStandardLocationCache).toStdString();
    }
} // initializeCacheDirPath

template <bool persistent>
void
CachePrivate<persistent>::ensureCacheDirectoryExists()
{
    QString userDirectoryCache = QString::fromUtf8(directoryContainingCachePath.c_str());

    {
        QDir d = QDir::root();
        d.mkpath(userDirectoryCache);
    }

    QDir d(userDirectoryCache);
    if (d.exists()) {
        QString cacheDirName = QString::fromUtf8(NATRON_CACHE_DIRECTORY_NAME);
        if (!d.exists(cacheDirName)) {
            d.mkdir(cacheDirName);
        }
        d.cd(cacheDirName);
        createIfNotExistBucketDirs(d);

    }
} // ensureCacheDirectoryExists


template <bool persistent>
std::string
Cache<persistent>::getCacheDirectoryPath() const
{
    QString cacheFolderName;
    cacheFolderName = QString::fromUtf8(_imp->directoryContainingCachePath.c_str());
    StrUtils::ensureLastPathSeparator(cacheFolderName);
    cacheFolderName.append( QString::fromUtf8(NATRON_CACHE_DIRECTORY_NAME) );
    return cacheFolderName.toStdString();
} // getCacheDirectoryPath


template <bool persistent>
bool
Cache<persistent>::isPersistent() const
{
    return persistent;
}

void
CacheBase::getTileSizePx(ImageBitDepthEnum bitdepth, int *tx, int *ty)
{
    switch (bitdepth) {
        case eImageBitDepthByte:
            *tx = NATRON_TILE_SIZE_X_8_BIT;
            *ty = NATRON_TILE_SIZE_Y_8_BIT;
            break;
        case eImageBitDepthShort:
        case eImageBitDepthHalf:
            *tx = NATRON_TILE_SIZE_X_16_BIT;
            *ty = NATRON_TILE_SIZE_Y_16_BIT;
            break;
        case eImageBitDepthFloat:
            *tx = NATRON_TILE_SIZE_X_32_BIT;
            *ty = NATRON_TILE_SIZE_Y_32_BIT;
            break;
        case eImageBitDepthNone:
            *tx = *ty = 0;
            break;
    }
}

template <bool persistent>
QString
CachePrivate<persistent>::getBucketAbsoluteDirPath(int bucketIndex) const
{
    QString bucketDirPath;
    bucketDirPath = QString::fromUtf8(directoryContainingCachePath.c_str());
    StrUtils::ensureLastPathSeparator(bucketDirPath);
    bucketDirPath += QString::fromUtf8(NATRON_CACHE_DIRECTORY_NAME);
    StrUtils::ensureLastPathSeparator(bucketDirPath);
    bucketDirPath += QString::fromUtf8(getBucketDirName(bucketIndex).c_str());
    StrUtils::ensureLastPathSeparator(bucketDirPath);
    return bucketDirPath;
}

template <bool persistent>
CacheEntryLockerBasePtr
Cache<persistent>::get(const CacheEntryBasePtr& entry) const
{
    return CacheEntryLocker<persistent>::create(_imp->buckets[0].cache.lock(), entry);
} // get

template <bool persistent>
bool
Cache<persistent>::hasCacheEntryForHash(U64 hash) const
{

    int bucketIndex = Cache::getBucketCacheBucketIndex(hash);
    CacheBucket<persistent>& bucket = _imp->buckets[bucketIndex];

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmReader(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif

    try {

        // Take the read lock on the toc file mapping
        boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
        boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
        bucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);



        boost::scoped_ptr<Sharable_ReadLock> readLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        readLock.reset(new Sharable_ReadLock(_imp->ipc->bucketsData[bucketIndex].bucketMutex));
#else
        createTimedLock<Sharable_ReadLock>(_imp.get(), readLock, &_imp->ipc->bucketsData[bucketIndex].bucketMutex);
#endif


        typename CacheBucket<persistent>::EntriesMap::iterator cacheEntryIt;
        typename CacheBucket<persistent>::EntriesMap* storage;
        return bucket.tryCacheLookupImpl(hash, &cacheEntryIt, &storage);
    } catch (...) {
        // Any exception caught here means the cache is corrupted
        _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                            shmReader
#endif
                                                        );
        return false;
    }
} // hasCacheEntryForHash

template <bool persistent>
void
Cache<persistent>::removeEntry(const CacheEntryBasePtr& entry)
{
    if (!entry) {
        return;
    }


    U64 hash = entry->getHashKey();
    int bucketIndex = Cache::getBucketCacheBucketIndex(hash);

    CacheBucket<persistent>& bucket = _imp->buckets[bucketIndex];

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmReader(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif

    // Take the bucket lock in write mode
    try {

        // Take the read lock on the toc file mapping
        boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
        boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
        bucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);

        boost::scoped_ptr<Sharable_WriteLock> writeLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        writeLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[bucketIndex].bucketMutex));
#else
        createTimedLock<Sharable_WriteLock>(_imp.get(), writeLock, &_imp->ipc->bucketsData[bucketIndex].bucketMutex);
#endif


        // Ensure the bucket is in a valid state.
        BucketStateHandler_RAII<persistent> bucketStateHandler(&bucket);

        // Deallocate the memory taken by the cache entry in the ToC
        {
            typename CacheBucket<persistent>::EntriesMap::iterator cacheEntryIt;
            typename CacheBucket<persistent>::EntriesMap* storage;
            if (bucket.tryCacheLookupImpl(hash, &cacheEntryIt, &storage)) {
                bucket.deallocateCacheEntryImpl(cacheEntryIt, storage);
            }
        }
    } catch (...) {
        // Any exception caught here means the cache is corrupted
        _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                            shmReader
#endif
                                           );
    }


} // removeEntry

template <bool persistent>
void
CachePrivate<persistent>::recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                           SHMReadLockerPtr& shmAccess
#endif
)
{


#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    // Release the read lock on the SHM
    shmAccess.reset();

    // Create and remap the SHM: do it so safely so we don't crash any other process
    ensureSharedMemoryIntegrity();

    // Flag that we are reading it
    shmAccess.reset(new SharedMemoryProcessLocalReadLocker(this));
#endif


    // Clear the cache: it could be corrupted
    _publicInterface->clear();

} // recoverFromInconsistentState

template <bool persistent>
void
CachePrivate<persistent>::clearCacheBucket(int bucket_i)
{

    CacheBucket<persistent>& bucket = buckets[bucket_i];

    // Take the write lock on the toc file mapping
    boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
    {

#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        tocWriteLock.reset(new Sharable_WriteLock(ipc->bucketsData[bucket_i].tocData.segmentMutex));
#else
        createTimedLock<Sharable_WriteLock>(this, tocWriteLock, &ipc->bucketsData[bucket_i].tocData.segmentMutex);
#endif
        // Close and re-create the memory mapped files
        std::string tocFilePath = getStoragePath(bucket.tocFile);
        clearStorage(bucket.tocFile);
        openStorage(bucket.tocFile, tocFilePath, (int)MemoryFile::eFileOpenModeOpenTruncateOrCreate);
        bucket.remapToCMemoryFile(*tocWriteLock, 0);

    }

} // clearCacheBucket


template <bool persistent>
void
Cache<persistent>::clear()
{

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    _imp->ensureSharedMemoryIntegrity();
    SHMReadLockerPtr shmReader(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif
    try {


        boost::scoped_ptr<Sharable_WriteLock> tileWriteLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
        tileWriteLock.reset(new Sharable_WriteLock(_imp->ipc->tilesStorageMutex));
#else
        createTimedLock<Sharable_WriteLock>(this, tileWriteLock, &_imp->ipc->tilesStorageMutex);
#endif
        for (std::size_t i = 0; i < _imp->tilesStorage.size(); ++i) {
            clearStorage(_imp->tilesStorage[i]);
        }
        _imp->tilesStorage.clear();


        for (int bucket_i = 0; bucket_i < NATRON_CACHE_BUCKETS_COUNT; ++bucket_i) {
            _imp->clearCacheBucket(bucket_i);
        } // for each bucket

        // Ensure we initialize the cache with at least one tile storage file
        _imp->createTileStorage();

    } catch (...) {

    }
    
    
    
} // clear()

template <bool persistent>
void
Cache<persistent>::evictLRUEntries(std::size_t nBytesToFree)
{
    std::size_t maxSize = getMaximumCacheSize();

    // If max size == 0 then there's no limit.
    if (maxSize == 0) {
        return;
    }

    if (nBytesToFree >= maxSize) {
        maxSize = 0;
    } else {
        maxSize = maxSize - nBytesToFree;
    }

    std::size_t curSize = getCurrentSize();

    bool mustEvictEntries = curSize > maxSize;

    while (mustEvictEntries) {
        
        bool foundBucketThatCanEvict = false;

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
        SHMReadLockerPtr shmReader(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif

        // Check each bucket
        for (int bucket_i = 0; bucket_i < NATRON_CACHE_BUCKETS_COUNT; ++bucket_i) {
            CacheBucket<persistent> & bucket = _imp->buckets[bucket_i];

            try {
                // Take the read lock on the toc file mapping
                boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
                boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
                bucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);


                // Take write lock on the bucket
                boost::scoped_ptr<Sharable_WriteLock> bucketLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                bucketLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[bucket_i].bucketMutex));
#else
                createTimedLock<Sharable_WriteLock>(_imp.get(), bucketLock, &_imp->ipc->bucketsData[bucket_i].bucketMutex);
#endif

                BucketStateHandler_RAII<persistent> bucketStateHandler(&bucket);


                U64 hash = 0;
                {
                    // Lock the LRU list
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                    boost::scoped_ptr<ExclusiveLock> writeLock (new ExclusiveLock(_imp->ipc->bucketsData[bucket_i].lruListMutex));
#else
                    boost::scoped_ptr<ExclusiveLock> lruWriteLock;
                    createTimedLock<ExclusiveLock>(_imp.get(), lruWriteLock, &_imp->ipc->bucketsData[bucket_i].lruListMutex);
#endif
                    // The least recently used entry is the one at the front of the linked list
                    if (bucket.ipc->lruListFront) {
                        hash = bucket.ipc->lruListFront->hash;
                    }
                }
                if (hash == 0) {
                    continue;
                }

                // Deallocate the memory taken by the cache entry in the ToC
                typename CacheBucket<persistent>::EntriesMap::iterator cacheEntryIt;
                typename CacheBucket<persistent>::EntriesMap* storage;
                if (!bucket.tryCacheLookupImpl(hash, &cacheEntryIt, &storage)) {
                    continue;
                }


                // We evicted one, decrease the size
                curSize -= cacheEntryIt->second->size;
                curSize -= cacheEntryIt->second->tileIndices.size() * NATRON_TILE_SIZE_BYTES;
                
                bucket.deallocateCacheEntryImpl(cacheEntryIt, storage);



            } catch (...) {
                // Any exception caught here means the cache is corrupted
                _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                                    shmReader
#endif
                                                   );
                return;
            }
            
            foundBucketThatCanEvict = true;
            
        } // for each bucket

        // No bucket can be evicted anymore, exit.
        if (!foundBucketThatCanEvict) {
            break;
        }

        // Update mustEvictEntries for next iteration
        mustEvictEntries = curSize > maxSize;

    } // while(mustEvictEntries)

} // evictLRUEntries

template <bool persistent>
void
Cache<persistent>::getMemoryStats(std::map<std::string, CacheReportInfo>* infos) const
{
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    boost::scoped_ptr<SharedMemoryProcessLocalReadLocker> shmReader(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif

    for (int bucket_i = 0; bucket_i < NATRON_CACHE_BUCKETS_COUNT; ++bucket_i) {
        CacheBucket<persistent>& bucket = _imp->buckets[bucket_i];

        try {
            // Take the read lock on the toc file mapping
            boost::scoped_ptr<Sharable_ReadLock> tocReadLock;
            boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
            bucket.checkToCMemorySegmentStatus(&tocReadLock, &tocWriteLock);

            // Take read lock on the bucket
            boost::scoped_ptr<Sharable_ReadLock> bucketLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            bucketLock.reset(new Sharable_ReadLock(_imp->ipc->bucketsData[bucket_i].bucketMutex));
#else
            createTimedLock<Sharable_ReadLock>(_imp.get(), bucketLock, &_imp->ipc->bucketsData[bucket_i].bucketMutex);
#endif

            // Cycle through the whole LRU list
            bip::offset_ptr<LRUListNode> it = bucket.ipc->lruListFront;
            while (it) {

                typename CacheBucket<persistent>::EntriesMap::iterator cacheEntryIt;
                typename CacheBucket<persistent>::EntriesMap* storage;
                if (!bucket.tryCacheLookupImpl(it->hash, &cacheEntryIt, &storage)) {
                    assert(false);
                    continue;
                }

                if (!cacheEntryIt->second->pluginID.empty()) {

                    std::string pluginID(cacheEntryIt->second->pluginID.c_str());
                    CacheReportInfo& entryData = (*infos)[pluginID];
                    ++entryData.nEntries;
                    entryData.nBytes += cacheEntryIt->second->size;
                }
                it = it->next;
            }
        } catch(...) {
            // Any exception caught here means the cache is corrupted
            _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                                shmReader
#endif
                                               );
            return;

        }

        
    } // for each bucket
} // getMemoryStats

template <bool persistent>
void
Cache<persistent>::flushCacheOnDisk(bool async)
{
    (void)async;
#if 0
    if (!persistent) {
        return;
    }

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    SHMReadLockerPtr shmReader(new SharedMemoryProcessLocalReadLocker(_imp.get()));
#endif
    
    for (int bucket_i = 0; bucket_i < NATRON_CACHE_BUCKETS_COUNT; ++bucket_i) {
        CacheBucket<persistent>& bucket = _imp->buckets[bucket_i];

        try {
            boost::scoped_ptr<Sharable_WriteLock> tocWriteLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
            tocWriteLock.reset(new Sharable_WriteLock(_imp->ipc->bucketsData[bucket_i].tocData.segmentMutex));
#else
            createTimedLock<Sharable_WriteLock>(_imp.get(), tocWriteLock, &_imp->ipc->bucketsData[bucket_i].tocData.segmentMutex);
#endif
            // First take a read lock and check if the mapping is valid. Otherwise take a write lock
            if (!bucket.isToCFileMappingValid()) {
                // This function will flush for us.
                bucket.remapToCMemoryFile(*tocWriteLock, 0);
            } else {
                bucket.tocFile->flush(async ? MemoryFile::eFlushTypeAsync : MemoryFile::eFlushTypeSync, NULL, 0);
            }

            {

                boost::scoped_ptr<Sharable_ReadLock> tilesStorageReadLock;
#ifndef NATRON_CACHE_INTERPROCESS_ROBUST
                tilesStorageReadLock.reset(new Sharable_ReadLock(_imp->ipc->tilesStorageMutex));
#else
                createTimedLock<Sharable_WriteLock>(this, tilesStorageReadLock, &_imp->ipc->tilesStorageMutex);
#endif
                for (U64_Set::const_iterator it = _imp->buckets[bucket_i].ipc->freeTiles.begin(); it != _imp->buckets[bucket_i].ipc->freeTiles.end(); ++it) {
                    U32 tileIndex, fileIndex;
                    getTileIndex(*it, &tileIndex, &fileIndex);

                    if (fileIndex >= _imp->tilesStorage.size()) {
                        continue;
                    }
                    CachePrivate::TileAlignedData* storage = &_imp->tilesStorage[fileIndex];
                    std::size_t dataOffset = tileIndex * NATRON_TILE_SIZE_BYTES;
                    storage->tileAlignedFile->flush(MemoryFile::eFlushTypeInvalidate, storage->tileAlignedFile->data() + dataOffset, NATRON_TILE_SIZE_BYTES);

                }
            }

        } catch (...) {
            // Any exception caught here means the cache is corrupted
            _imp->recoverFromInconsistentState(
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
                                                shmReader
#endif
                                               );

        }

    } // for each bucket
#endif
} // flushCacheOnDisk

template class Cache<true>;
template class Cache<false>;

NATRON_NAMESPACE_EXIT;

