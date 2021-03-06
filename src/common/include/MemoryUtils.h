// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.


#ifndef DATADIST_MEMORY_UTILS_H_
#define DATADIST_MEMORY_UTILS_H_

#include <boost/container/pmr/memory_resource.hpp>
#include <boost/filesystem.hpp>

#include <fairmq/FairMQDevice.h>
#include <fairmq/FairMQChannel.h>

#include "DataDistLogger.h"

#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <chrono>

#include <sys/mman.h>
#include <cstdlib>
#include <unistd.h>

class DataHeader;
class FairMQUnmanagedRegion;

namespace o2
{
namespace DataDistribution
{

static constexpr const char *ENV_SHM_PATH = "DATADIST_SHM_PATH";

class FMQUnsynchronizedPoolMemoryResource
{

public:
  FMQUnsynchronizedPoolMemoryResource() = delete;

  FMQUnsynchronizedPoolMemoryResource(std::string pSegmentName, FairMQChannel &pChan,
                                      const std::size_t pSize, const std::size_t pObjSize,
                                      std::uint64_t pRegionFlags = 0)
  : mSegmentName(pSegmentName),
    mChan(pChan),
    mObjectSize(pObjSize)//,
    // mAlignedSize((pObjSize + sizeof(max_align_t) - 1) / sizeof(max_align_t) * sizeof(max_align_t))
  {
    int lMapFlags = 0;
    std::string lSegmentRoot = "";

    // don't reserve swap space and try to lock the region
#if defined(MAP_NORESERVE) && defined(MAP_LOCKED)
    lMapFlags = MAP_NORESERVE | MAP_LOCKED;
#endif

    // try to use different file mapping (hugetlbfs)
    const auto lHugetlbfsPath = std::getenv(ENV_SHM_PATH);
    if (lHugetlbfsPath) {
      do {
        // make sure directory exists
        namespace bfs = boost::filesystem;
        bfs::path lDirPath(lHugetlbfsPath);
        if (!bfs::is_directory(lDirPath)) {
          DDLOGF(fair::Severity::ERROR, "Hugetlbfs mountpoint does not exist. Not using huge pages. {}={}",
            ENV_SHM_PATH, lHugetlbfsPath);
          break;
        }

        // check if the hugetlbs is writeable
        if (0 != access(lHugetlbfsPath, W_OK)) {
          DDLOGF(fair::Severity::ERROR, "Hugetlbfs mountpoint is not writeable. "
            "Make sure the permissions are properly set. {}={}", ENV_SHM_PATH, lHugetlbfsPath);
          break;
        }

        lSegmentRoot = lHugetlbfsPath;
        lSegmentRoot += bfs::path::preferred_separator;
      } while (false);
    }


    DDLOGF(fair::Severity::INFO, "Creating new UnmanagedRegion name={} path={} size={} channel={}",
      mSegmentName, lSegmentRoot, pSize, pChan.GetName());

    mRegion = pChan.Transport()->CreateUnmanagedRegion(
      pSize,
      pRegionFlags,
      [this](const std::vector<FairMQRegionBlock>& pBlkVect) {
        // callback to be called when message buffers no longer needed by transport
        std::scoped_lock lock(mReclaimLock);

        if (!mRunning) {
          return;
        }

        for (const auto &lBlk : pBlkVect) {
          reclaimSHMMessage(lBlk.ptr, lBlk.size);
        }
      },
      lSegmentRoot.c_str(),
      lMapFlags
    );

    if (!mRegion) {
      DDLOGF(fair::Severity::FATAL, "Creation of new memory region failed. name={} size={} path={} channel={}",
      mSegmentName, pSize, lSegmentRoot, pChan.GetName());
      throw std::bad_alloc();
    }

    // prepare header pointers
    unsigned char* lObj = static_cast<unsigned char*>(mRegion->GetData());
    memset(lObj, 0xAA, mRegion->GetSize());

    const std::size_t lObjectCnt = mRegion->GetSize() / mObjectSize;

    for (std::size_t i = 0; i < lObjectCnt; i++) {
      mAvailableObjects.push_back(lObj + i * mObjectSize);
    }
  }

  std::unique_ptr<FairMQMessage> NewFairMQMessage(const std::size_t pSize=0) {

    const auto lMem = do_allocate(pSize, 0); // boost -> do_allocate(0) .. always return fixed object size (mObjectSize)

    if (!lMem && !mRunning) {
      return nullptr;
    }

    if (lMem != nullptr) {
      return mChan.NewMessage(mRegion, lMem, mObjectSize);
    } else {
      // Log warning to increase the pool size
      static thread_local unsigned throttle = 0;
      if (++throttle > (1U << 18)) {
       DDLOG(fair::Severity::WARNING) << "Header pool exhausted. Allocating from the global SHM pool.";
        throttle = 0;
      }

      return mChan.NewMessage(mObjectSize);
    }
  }

  std::unique_ptr<FairMQMessage> NewFairMQMessageFromPtr(void *pPtr) {
    assert(pPtr >= static_cast<byte*>(mRegion->GetData()) && pPtr < static_cast<byte*>(mRegion->GetData()) + mRegion->GetSize());

    return mChan.NewMessage(mRegion, pPtr, mObjectSize);
  }

  std::size_t objectSize() const { return mObjectSize; }

  inline void stop() {
    std::scoped_lock lock(mReclaimLock);
    mRunning = false;
  }

protected:
  void* do_allocate(std::size_t , std::size_t)
  {
    if (!mRunning) {
      return nullptr;
    }

    unsigned long lAllocAttempt = 0;

    auto lRet = try_alloc();
    // we cannot fail! report problem if failing to allocate block often
    while (!lRet && mRunning) {
      // try to reclaim if possible
      try_reclaim();
      // try again
      lRet = try_alloc();

      if (!lRet) {
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(1ms);

        if (++lAllocAttempt % 512 == 0) {
          DDLOGF(fair::Severity::WARNING,
            "FMQUnsynchronizedPoolMemoryResource: failing to allocate free block of {} B, total region size: {} B",
            mObjectSize, mRegion->GetSize());
          DDLOGF(fair::Severity::WARNING, "Memory segment '{}' too small or large back-pressure.", mSegmentName);
        }
      }
    }

    assert (lRet);
    std::memset(lRet, 0, mObjectSize);

    return lRet;
  }

  void do_deallocate(void *, std::size_t, std::size_t)
  {
    // Objects are only freed through SHM message reclaim callback.
    // This is intentionally noop
    // NOTE: handled in reclaimSHMMessage()
  }

private:

  void* try_alloc() {

    if (!mAvailableObjects.empty()) {
      auto lObjectPtr = mAvailableObjects.back();
      mAvailableObjects.pop_back();

      return lObjectPtr;
    }

    return nullptr;
  }

  bool try_reclaim() {

    std::scoped_lock lock(mReclaimLock);

    assert(mAvailableObjects.empty());

    mAvailableObjects = std::move(mReclaimedObjects);
    mReclaimedObjects.clear();

    return !mAvailableObjects.empty();
  }

  void reclaimSHMMessage(void* pData, size_t pSize)
  {
    (void) pSize;
    assert (pSize == mObjectSize);
    mReclaimedObjects.push_back(pData);
  }

  /// fields
  std::string mSegmentName;
  std::atomic_bool mRunning = true;

  FairMQChannel& mChan;

  std::unique_ptr<FairMQUnmanagedRegion> mRegion;
  std::size_t mObjectSize;

  std::vector<void*> mAvailableObjects;
  std::mutex mReclaimLock;

  // two step reclaim to avoid lock contention in the allocation path
  std::vector<void*> mReclaimedObjects;
};


template<size_t ALIGN = 64>
class RegionAllocatorResource
{

public:
  RegionAllocatorResource() = delete;

  RegionAllocatorResource(std::string pSegmentName, FairMQChannel &pChan,
                          std::size_t pSize, std::uint64_t pRegionFlags = 0)
  : mSegmentName(pSegmentName),
    mChan(pChan)
  {
    static_assert(ALIGN && !(ALIGN & (ALIGN - 1)), "Alignment must be power of 2");

    pSize = align_size_up(pSize);

    int lMapFlags = 0;
    std::string lSegmentRoot = "";

    // don't reserve swap space and try to lock the region
#if defined(MAP_NORESERVE) && defined(MAP_LOCKED)
    lMapFlags = MAP_NORESERVE | MAP_LOCKED;
#endif

    // try to use different file mapping (hugetlbfs)
    const auto lHugetlbfsPath = std::getenv(ENV_SHM_PATH);
    if (lHugetlbfsPath) {
      do {
        // make sure directory exists
        namespace bfs = boost::filesystem;
        bfs::path lDirPath(lHugetlbfsPath);
        if (!bfs::is_directory(lDirPath)) {
          DDLOGF(fair::Severity::ERROR, "Hugetlbfs mountpoint does not exist. Not using huge pages. {}={}",
            ENV_SHM_PATH, lHugetlbfsPath);
          break;
        }

        // check if the hugetlbs is writeable
        if (0 != access(lHugetlbfsPath, W_OK)) {
          DDLOGF(fair::Severity::ERROR, "Hugetlbfs mountpoint is not writeable. "
            "Make sure the permissions are properly set. {}={}", ENV_SHM_PATH, lHugetlbfsPath);
          break;
        }

        lSegmentRoot = lHugetlbfsPath;
        lSegmentRoot += bfs::path::preferred_separator;
      } while (false);
    }

    DDLOGF(fair::Severity::INFO, "Creating new UnmanagedRegion name={} path={} size={} channel={}",
      mSegmentName, lSegmentRoot, pSize, pChan.GetName());

    mRegion = pChan.Transport()->CreateUnmanagedRegion(
      pSize,
      pRegionFlags,
      [this](const std::vector<FairMQRegionBlock>& pBlkVect) {
        // callback to be called when message buffers no longer needed by transports
        std::scoped_lock lock(mReclaimLock);
        if (!mRunning) {
          return;
        }

        for (const auto &lBlk : pBlkVect) {
          reclaimSHMMessage(lBlk.ptr, lBlk.size);
        }
      },
      lSegmentRoot.c_str(),
      lMapFlags
    );

    if (!mRegion) {
      DDLOGF(fair::Severity::FATAL, "Creation of new memory region failed. name={} size={} path={} channel={}",
      mSegmentName, pSize, lSegmentRoot, pChan.GetName());
      throw std::bad_alloc();
    }

    mStart = static_cast<char*>(mRegion->GetData());
    mLength = mRegion->GetSize();

    memset(mStart, 0xAA, mLength);

    // start the allocations
    mRunning = true;
  }

  inline
  std::unique_ptr<FairMQMessage> NewFairMQMessage(std::size_t pSize) {
    auto* lMem = do_allocate(pSize, ALIGN);
    if (lMem) {
      return mChan.NewMessage(mRegion, lMem, pSize);
    } else {
      return nullptr;
    }
  }

  inline
  std::unique_ptr<FairMQMessage> NewFairMQMessageFromPtr(void *pPtr, const std::size_t pSize) {
    assert(pPtr >= static_cast<char*>(mRegion->GetData()));
    assert(static_cast<char*>(pPtr)+pSize <= static_cast<char*>(mRegion->GetData()) + mRegion->GetSize());

    if (pSize == 0) {
       DDLOGF(fair::Severity::WARNING, "NewFairMQMessageFromPtr: Zero message allocation name={}: {}",
        mSegmentName, __LINE__);
    }

    assert(pSize > 0);

    return mChan.NewMessage(mRegion, pPtr, pSize);
  }

  inline void stop() {
    std::scoped_lock lock(mReclaimLock);
    mRunning = false;
  }

protected:
  // NOTE: we align sizes of returned messages, but keep the exact size for allocation
  //       otherwise the shm messages would be larger than requested
  static constexpr inline
  std::size_t align_size_up(const std::size_t pSize) {
    return (pSize + ALIGN - 1) / ALIGN * ALIGN;
  }

  void* do_allocate(std::size_t pSize, std::size_t /* pAlign */)
  {
    if (!mRunning) {
      return nullptr;
    }

    unsigned long lAllocAttempt = 0;

    if (pSize == 0) {
       DDLOGF(fair::Severity::WARNING, "Zero message allocation name={}: {}",
        mSegmentName, __LINE__);
    }

    // align up
    pSize = align_size_up(pSize);

    auto lRet = try_alloc(pSize);
    // we cannot fail! report problem if failing to allocate block often
    while (!lRet && mRunning) {
      // try to reclaim if possible
      if (try_reclaim(pSize)) {
        // try again
        lRet = try_alloc(pSize);
      }

      if (!lRet) {
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(1ms);

        if (++lAllocAttempt % 512 == 0) {
          DDLOGF(fair::Severity::WARNING,
            "RegionAllocatorResource: failing to allocate free block of {} B, total region size: {} B",
            pSize, mRegion->GetSize());
          DDLOGF(fair::Severity::WARNING, "Memory segment '{}' too small or large back-pressure.", mSegmentName);
        }
      }
    }

    return lRet;
  }

  void do_deallocate(void *, std::size_t pSize, std::size_t)
  {
    // we are called only if allocation was made through the polymorphic_allocator
    // but with only a pointer, size == 0!
    if (pSize != 0) {
       DDLOGF(fair::Severity::WARNING, "do_deallocate: non-Zero message dealloc name={} size={}",
        mSegmentName, pSize);
    } else {
      return; // dealloc goes through shmem queue
    }
  }

private:
  void* try_alloc(const std::size_t pSize) {
    if (mLength >= pSize) {
      const auto lObjectPtr = mStart;

      mStart += pSize;
      mLength -= pSize;

      if (mLength == 0) {
        mStart = nullptr;
      }

      return lObjectPtr;
    }

    return nullptr;
  }


  bool try_reclaim(const std::size_t pSize) {
    // First declare any leftover memory as free
    std::scoped_lock lock(mReclaimLock);

    if (mLength > 0) {
      assert(mStart != nullptr);
      // NOTE: caller must hold mReclaimLock lock
      reclaimSHMMessage(mStart, mLength);
      // invalidate the working extent
      mLength = 0;
      mStart = nullptr;
    }

    if (mFrees.empty()) {
      return false;
    }

    // find the largest free extent and return it if the size is adequate
    auto lMaxIter = std::max_element(std::begin(mFrees), std::end(mFrees),
      [](const auto& l, const auto& r) { return l.second < r.second; });

    // check if the size is adequate
    if (pSize > lMaxIter->second) {
      return false;
    }

    // return the extent
    mStart = const_cast<char*>(lMaxIter->first);
    mLength = lMaxIter->second;
    mFrees.erase(lMaxIter);

    return true;
  }

  void reclaimSHMMessage(void* pData, size_t pSize)
  {
    // align up
    pSize = align_size_up(pSize);

    const char *lData = reinterpret_cast<const char*>(pData);

    // push object to the free map. Try to merge nodes
    const auto lIter = mFrees.lower_bound(lData);
    bool lInserted = false;

#if !defined(NDEBUG)
    if (lIter != mFrees.end()) {
      if (lIter->first <= lData) {
        DDLOGF(fair::Severity::ERROR, "iter={:p}, data={:p}, Free map:", lIter->first, lData);

        for (const auto &lit : mFrees) {
          DDLOGF(fair::Severity::ERROR, " iter={:p}, size={}", lit.first, lit.second);
        }
      }
      assert(lIter->first > lData); // we cannot have this exact value in the free list
    }
#endif

    // check if we can merge with the previous
    if (!mFrees.empty() && lIter != mFrees.begin()) {
      auto lPrev = std::prev(lIter);

      if ((lPrev->first + lPrev->second) == lData) {
        lPrev->second += pSize;
        lInserted = true;

        // check if we also can merge with the next (lIter)
        if (lIter != mFrees.end()) {
          if ((lPrev->first + lPrev->second) == lIter->first) {
            lPrev->second += lIter->second;
            mFrees.erase(lIter);
          }
        }
      }
    }

    if (!lInserted) {
      // insert the new range
      auto lIt = mFrees.emplace_hint(lIter, lData, pSize);

      if (lIt->second != pSize) {
        DDLOGF(fair::Severity::ERROR, "BUG: RegionAllocatorResource: REPEATED INSERT!!! "
          " {:p} : {}, original size: {}", lIt->first, pSize, lIt->second);
      }

      // check if we can merge with the next
      auto lNextIt = std::next(lIt);
      if (lNextIt != mFrees.cend()) {
        if ((lData + pSize) == lNextIt->first) {
          lIt->second += lNextIt->second;
          mFrees.erase(lNextIt);
        }
      }
    }
  }

  /// fields
  std::string mSegmentName;
  std::atomic_bool mRunning = true;

  FairMQChannel& mChan;
  std::unique_ptr<FairMQUnmanagedRegion> mRegion;

  char *mStart = nullptr;
  std::size_t mLength = 0;

  // two step reclaim to avoid lock contention in the allocation path
  std::mutex mReclaimLock;
  std::map<const char*, std::size_t> mFrees; // keep all returned blocks
};

}
} /* o2::DataDistribution */

#endif /* DATADIST_MEMORY_UTILS_H_ */
