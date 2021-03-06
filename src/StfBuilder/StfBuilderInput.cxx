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

#include "StfBuilderInput.h"
#include "StfBuilderDevice.h"

#include <SubTimeFrameBuilder.h>
#include <Utilities.h>

#include <DataDistLogger.h>

#include <FairMQDevice.h>

#include <vector>
#include <queue>
#include <chrono>
#include <sstream>

namespace o2
{
namespace DataDistribution
{

void StfInputInterface::start(const std::size_t pNumBuilders)
{
  mNumBuilders = pNumBuilders;
  mRunning = true;

  mBuilderInputQueues.clear();
  mBuilderInputQueues.resize(mNumBuilders);

  // Reference to the output or DPL channel
  // const auto &lOutChanName = mDevice.getOutputChannelName();
  auto& lOutputChan = mDevice.getOutputChannel();

  // NOTE: create the mStfBuilders first to avid resizing the vector; then threads
  for (std::size_t i = 0; i < mNumBuilders; i++) {
    mStfBuilders.emplace_back(lOutputChan, mDevice.dplEnabled());
  }

  for (std::size_t i = 0; i < mNumBuilders; i++) {
    mBuilderThreads.emplace_back(std::thread(&StfInputInterface::StfBuilderThread, this, i));
  }

  mInputThread = std::thread(&StfInputInterface::DataHandlerThread, this, 0);
}

void StfInputInterface::stop()
{
  mRunning = false;

  for (auto &lBuilder : mStfBuilders) {
    lBuilder.stop();
  }

  if (mInputThread.joinable()) {
    mInputThread.join();
  }

  for (auto &lQueue : mBuilderInputQueues) {
    lQueue.stop();
  }

  for (auto &lBldThread : mBuilderThreads) {
    if (lBldThread.joinable()) {
      lBldThread.join();
    }
  }

  // mStfBuilders.clear(); // TODO: deal with shm region cleanup
  mBuilderThreads.clear();
  mBuilderInputQueues.clear();

  DDLOGF(fair::Severity::trace, "INPUT INTERFACE: Stopped.");
}

/// Receiving thread
void StfInputInterface::DataHandlerThread(const unsigned pInputChannelIdx)
{
  std::vector<FairMQMessagePtr> lReadoutMsgs;
  lReadoutMsgs.reserve(1U << 20);
  // current TF Id
  std::uint64_t lCurrentStfId = 0;

  // Reference to the input channel
  auto& lInputChan = mDevice.GetChannel(mDevice.getInputChannelName(), pInputChannelIdx);

  try {
    while (mRunning) {

      // Equipment ID for the HBFrames (from the header)
      ReadoutSubTimeframeHeader lReadoutHdr;
      lReadoutMsgs.clear();

      // receive readout messages
      const auto lRet = lInputChan.Receive(lReadoutMsgs);
      if (lRet < 0 && mRunning) {
        // DDLOG(fair::Severity::WARNING) << "StfHeader receive failed (err = " + std::to_string(lRet) + ")";
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(500ms);
        continue;
      } else if (lRet < 0) {
        break; // should exit?
      }

      if (lReadoutMsgs.empty() ) {
        // nothing received?
        continue;
      }

      // Copy to avoid surprises. The receiving header is not O2 compatible and can be discarded
      assert(lReadoutMsgs[0]->GetSize() == sizeof(ReadoutSubTimeframeHeader));
      std::memcpy(&lReadoutHdr, lReadoutMsgs[0]->GetData(), sizeof(ReadoutSubTimeframeHeader));

      {
        static thread_local std::uint64_t sNumContIncProblems = 0;
        static thread_local std::uint64_t sNumContDecProblems = 0;

        if (lReadoutHdr.mTimeFrameId < lCurrentStfId) {
          std::stringstream lErrMsg;
          lErrMsg << "READOUT INTERFACE: "
              "TF ID decreased! (" << lCurrentStfId << ") -> (" << lReadoutHdr.mTimeFrameId << ") "
              "readout.exe sent messages with non-monotonic TF id! SubTimeFrames will be incomplete! "
              "Total occurrences: " << sNumContIncProblems;

          if (sNumContIncProblems++ % 10 == 0) {
            DDLOGF(fair::Severity::ERROR, lErrMsg.str());
          } else {
            DDLOGF(fair::Severity::DEBUG, lErrMsg.str());
          }

          // TODO: accout for lost data
          lReadoutMsgs.clear();
          continue;
        }

        if (lReadoutHdr.mTimeFrameId > (lCurrentStfId + 1)) {
          std::stringstream lErrMsg;
          lErrMsg << "READOUT INTERFACE: "
            "TF ID non-contiguous increase! (" << lCurrentStfId << ") -> (" << lReadoutHdr.mTimeFrameId << ") "
            "readout.exe sent messages with non-monotonic TF id! SubTimeFrames will be incomplete! "
            "Total occurrences: " << sNumContDecProblems;

          if (sNumContDecProblems++ % 10 == 0) {
            DDLOGF(fair::Severity::ERROR, lErrMsg.str());
          } else {
            DDLOGF(fair::Severity::DEBUG, lErrMsg.str());
          }
        }
      }

      // make sure we never jump down
      lCurrentStfId = std::max(lCurrentStfId, std::uint64_t(lReadoutHdr.mTimeFrameId));

      mBuilderInputQueues[lReadoutHdr.mTimeFrameId % mNumBuilders].push(std::move(lReadoutMsgs));
    }
  } catch (std::runtime_error& e) {
    DDLOGF(fair::Severity::ERROR, "Input channel receive failed. Stopping input thread...");
    return;
  }

  DDLOGF(fair::Severity::trace, "Exiting the input thread...");
}

/// StfBuilding thread
void StfInputInterface::StfBuilderThread(const std::size_t pIdx)
{
  using namespace std::chrono_literals;
  // current TF Id
  std::int64_t lCurrentStfId = 0;
  std::vector<FairMQMessagePtr> lReadoutMsgs;
  lReadoutMsgs.reserve(1U << 20);

  // Reference to the input channel
  assert (mBuilderInputQueues.size() == mNumBuilders);
  assert (pIdx < mBuilderInputQueues.size());
  auto &lInputQueue = mBuilderInputQueues[pIdx];

  // Stf builder
  SubTimeFrameReadoutBuilder &lStfBuilder = mStfBuilders[pIdx];

  const std::chrono::microseconds cMinWaitTime = 2s;
  const std::chrono::microseconds cDesiredWaitTime = 2s * mNumBuilders / 3;
  const auto cStfDataWaitFor = std::max(cMinWaitTime, cDesiredWaitTime);

  using hres_clock = std::chrono::high_resolution_clock;
  auto lStfStartTime = hres_clock::now();

    while (mRunning) {

      // Equipment ID for the HBFrames (from the header)
      lReadoutMsgs.clear();

      // receive readout messages
      const auto lRet = lInputQueue.pop_wait_for(lReadoutMsgs, cStfDataWaitFor);
      if (!lRet && mRunning) {

        // timeout! should finish the Stf if have outstanding data
        std::unique_ptr<SubTimeFrame> lStf = lStfBuilder.getStf();

        if (lStf) {
         DDLOGF(fair::Severity::WARNING, "READOUT INTERFACE: finishing STF on a timeout. stf_id={} size={}",
          lStf->header().mId, lStf->getDataSize());

          mDevice.queue(eStfBuilderOut, std::move(lStf));

          { // MON: data of a new STF received, get the freq and new start time
            const auto lStfDur = std::chrono::duration<float>(hres_clock::now() - lStfStartTime);
            mStfFreqSamples.Fill(1.0f / lStfDur.count() * mNumBuilders);
            lStfStartTime = hres_clock::now();
          }
        }

        lReadoutMsgs.clear();
        continue;

      } else if (!mRunning) {
        break;
      }

      if (lReadoutMsgs.empty()) {
        DDLOGF(fair::Severity::ERROR, "READOUT INTERFACE: empty readout multipart.");
        continue;
      }

      if (lReadoutMsgs.size() < 2) {
        DDLOGF(fair::Severity::ERROR, "READOUT INTERFACE: no data sent, only header.");
        continue;
      }

      // Copy to avoid surprises. The receiving header is not O2 compatible and can be discarded
      ReadoutSubTimeframeHeader lReadoutHdr;
      assert(lReadoutMsgs[0]->GetSize() == sizeof(ReadoutSubTimeframeHeader));
      std::memcpy(&lReadoutHdr, lReadoutMsgs[0]->GetData(), sizeof(ReadoutSubTimeframeHeader));

      // log only
      if (lReadoutHdr.mTimeFrameId % (100 + pIdx) == 0) {
        static thread_local std::uint64_t sStfSeen = 0;
        if (lReadoutHdr.mTimeFrameId != sStfSeen) {
          sStfSeen = lReadoutHdr.mTimeFrameId;
          DDLOGF(fair::Severity::DEBUG, "READOUT INTERFACE: Received an ReadoutMsg. stf_id={}", lReadoutHdr.mTimeFrameId);
        }
      }

      // check multipart size
      {
        if (lReadoutHdr.mNumberHbf != lReadoutMsgs.size() - 1) {
          static thread_local std::uint64_t sNumMessages = 0;
          if (sNumMessages++ % 8192 == 0) {
            DDLOGF(fair::Severity::ERROR, "READOUT INTERFACE: wrong number of HBFrames in the header."
              "header_cnt={} msg_length={} total_occurrences={}",
              lReadoutHdr.mNumberHbf, (lReadoutMsgs.size() - 1), sNumMessages);
          }

          lReadoutHdr.mNumberHbf = lReadoutMsgs.size() - 1;
        }

        if (lReadoutMsgs.size() > 1) {
          try {
            const auto R = RDHReader(lReadoutMsgs[1]);
            const auto lLinkId = R.getLinkID();

            if (lLinkId != lReadoutHdr.mLinkId) {
              DDLOGF(fair::Severity::ERROR, "READOUT INTERFACE: update link ID does not match RDH in the data block."
                " hdr_link_id={} rdh_link_id={}", lReadoutHdr.mLinkId, lLinkId);
            }
          } catch (RDHReaderException &e) {
            DDLOGF(fair::Severity::ERROR, e.what());
            // TODO: the whole ReadoutMsg is discarded. Account and report the data size.
            continue;
          }
        }
      }

      if (lReadoutMsgs.size() <= 1) {
        DDLOGF(fair::Severity::ERROR, "READOUT INTERFACE: no data sent, invalid blocks removed.");
        continue;
      }

      // DDLOG(fair::Severity::DEBUG) << "RECEIVED:: "
      //           << "TF id: " << lReadoutHdr.mTimeFrameId << ", "
      //           << "#HBF: " << lReadoutHdr.mNumberHbf << ", "
      //           << "EQ: " << lReadoutHdr.linkId;

      // check for the new TF marker
      if (lReadoutHdr.mTimeFrameId != lCurrentStfId) {

        if (lReadoutMsgs.size() > 1) {
          ReadoutDataUtils::sFirstSeenHBOrbitCnt = 0;
        }

        if (lCurrentStfId >= 0) {
          // Finished: queue the current STF and start a new one
          std::unique_ptr<SubTimeFrame> lStf = lStfBuilder.getStf();

          if (lStf) {
            // DDLOG(fair::Severity::DEBUG) << "Received TF[" << lStf->header().mId<< "]::size= " << lStf->getDataSize();
            mDevice.queue(eStfBuilderOut, std::move(lStf));

            { // MON: data of a new STF received, get the freq and new start time
              const auto lStfDur = std::chrono::duration<float>(hres_clock::now() - lStfStartTime);
              mStfFreqSamples.Fill(1.0f / lStfDur.count() * mNumBuilders);
              lStfStartTime = hres_clock::now();
            }
          }
        }

        // start a new STF
        lCurrentStfId = lReadoutHdr.mTimeFrameId;
      }

      // check subspecifications of all messages
      header::DataHeader::SubSpecificationType lSubSpecification = ~header::DataHeader::SubSpecificationType(0);
      header::DataOrigin lDataOrigin;
      try {
        const auto R1 = RDHReader(lReadoutMsgs[1]);
        lDataOrigin = ReadoutDataUtils::getDataOrigin(R1);
        lSubSpecification = ReadoutDataUtils::getSubSpecification(R1);
      } catch (RDHReaderException &e) {
        DDLOGF(fair::Severity::ERROR, e.what());
        // TODO: the whole ReadoutMsg is discarded. Account and report the data size.
        continue;
      }

      assert (lReadoutMsgs.size() > 1);
      auto lStartHbf = lReadoutMsgs.begin() + 1; // skip the meta message
      auto lEndHbf = lStartHbf + 1;

      std::size_t lAdded = 0;
      bool lErrorWhileAdding = false;

      while (1) {
        if (lEndHbf == lReadoutMsgs.end()) {
          //insert
          lStfBuilder.addHbFrames(lDataOrigin, lSubSpecification, lReadoutHdr, lStartHbf, lEndHbf - lStartHbf);
          lAdded += (lEndHbf - lStartHbf);
          break;
        }

        header::DataHeader::SubSpecificationType lNewSubSpec = ~header::DataHeader::SubSpecificationType(0);
        try {
          const auto Rend = RDHReader(*lEndHbf);
          lNewSubSpec = ReadoutDataUtils::getSubSpecification(Rend);
        } catch (RDHReaderException &e) {
            DDLOGF(fair::Severity::ERROR, e.what());
            // TODO: portion of the ReadoutMsg is discarded. Account and report the data size.
            lErrorWhileAdding = true;
            break;
          }

        if (lNewSubSpec != lSubSpecification) {
          DDLOGF(fair::Severity::ERROR, "READOUT INTERFACE: update with mismatched subspecification."
            " block[0]: {:#06x}, block[{}]: {:#06x}",
            lSubSpecification, (lEndHbf - (lReadoutMsgs.begin() + 1)), lNewSubSpec);
          // insert
          lStfBuilder.addHbFrames(lDataOrigin, lSubSpecification, lReadoutHdr, lStartHbf, lEndHbf - lStartHbf);
          lAdded += (lEndHbf - lStartHbf);
          lStartHbf = lEndHbf;

          lSubSpecification = lNewSubSpec;
        }
        lEndHbf = lEndHbf + 1;
      }

      if (!lErrorWhileAdding && (lAdded != lReadoutMsgs.size() - 1) ) {
        DDLOGF(fair::Severity::ERROR, "BUG: Not all received HBFrames added to the STF...");
      }
    }

  DDLOGF(fair::Severity::trace, "Exiting StfBuilder thread...");
}

}
}
