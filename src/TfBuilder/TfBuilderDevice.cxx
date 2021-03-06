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

#include "TfBuilderDevice.h"

#include <SubTimeFrameDataModel.h>
#include <SubTimeFrameVisitors.h>

#include <ConfigConsul.h>
#include <SubTimeFrameDPL.h>

#include <chrono>
#include <thread>

namespace o2
{
namespace DataDistribution
{

using namespace std::chrono_literals;

TfBuilderDevice::TfBuilderDevice()
  : DataDistDevice(),
    IFifoPipeline(eTfPipelineSize),
    mFileSink(*this, *this, eTfFileSinkIn, eTfFileSinkOut),
    mFileSource(*this, eTfFileSourceOut),
    mTfSizeSamples(),
    mTfFreqSamples()
{
}

TfBuilderDevice::~TfBuilderDevice()
{
  DDLOG(fair::Severity::TRACE) << "TfBuilderDevice::~TfBuilderDevice()";
}

void TfBuilderDevice::InitTask()
{
  DataDistLogger::SetThreadName("tfb-main");

  {
    mDplChannelName = GetConfig()->GetValue<std::string>(OptionKeyDplChannelName);
    mStandalone = GetConfig()->GetValue<bool>(OptionKeyStandalone);
    mTfBufferSize = GetConfig()->GetValue<std::uint64_t>(OptionKeyTfMemorySize);
    mTfBufferSize <<= 20; /* input parameter is in MiB */

    mDiscoveryConfig = std::make_shared<ConsulTfBuilder>(ProcessType::TfBuilder,
      Config::getEndpointOption(*GetConfig()));

    auto &lStatus =  mDiscoveryConfig->status();
    lStatus.mutable_info()->set_type(TfBuilder);
    lStatus.mutable_info()->set_process_id(Config::getIdOption(*GetConfig()));
    lStatus.mutable_info()->set_ip_address(Config::getNetworkIfAddressOption(*GetConfig()));

    mPartitionId = Config::getPartitionOption(*GetConfig());
    lStatus.mutable_partition()->set_partition_id(mPartitionId);

    // File sink
    if (!mFileSink.loadVerifyConfig(*(this->GetConfig()))) {
      throw "File Sink options";
      return;
    }

    mRpc = std::make_shared<TfBuilderRpcImpl>(mDiscoveryConfig);
    mFlpInputHandler = std::make_unique<TfBuilderInput>(*this, mRpc, eTfBuilderOut);
  }

  {
    auto &lStatus = mDiscoveryConfig->status();

    // finish discovery information: gRPC server
    int lRpcRealPort = 0;
    mRpc->initDiscovery(lStatus.info().ip_address(), lRpcRealPort);
    lStatus.set_rpc_endpoint(lStatus.info().ip_address() + ":" + std::to_string(lRpcRealPort));

    if (! mDiscoveryConfig->write(true)) {
      DDLOGF(fair::Severity::ERROR, "Can not start TfBuilder. id={:d}", lStatus.info().process_id());
      DDLOG(fair::Severity::ERROR) << "Process with the same id already running? If not, clear the key manually.";
      throw "Discovery database error";
      return;
    }

    // Using DPL?
    if (mDplChannelName != "") {
      mDplEnabled = true;
      mStandalone = false;
      DDLOGF(fair::Severity::INFO, "Using DPL channel. channel_name={:s}", mDplChannelName);
    } else {
      mDplEnabled = false;
      mStandalone = true;
      DDLOGF(fair::Severity::INFO, "Not sending to DPL.");
    }

    // channel for FileSource: stf or dpl, or generic one in case of standalone
    if (mStandalone) {
      // create default FMQ shm channel
      auto lTransportFactory = FairMQTransportFactory::CreateTransportFactory("shmem", "", GetConfig());
      if (!lTransportFactory) {
        DDLOG(fair::Severity::ERROR) << "Creating transport factory failed!";
        throw "Transport factory";
        return;
      }
      mStandaloneChannel = std::make_unique<FairMQChannel>(
        "standalone-chan[0]" ,  // name
        "pair",                 // type
        "bind",                 // method
        "ipc:///tmp/standalone-chan-tfb", // address
        lTransportFactory
      );

      mStandaloneChannel->Init();
      mStandaloneChannel->Validate();
    }

    // start the info thread
    mInfoThread = std::thread(&TfBuilderDevice::InfoThread, this);
  }
}

void TfBuilderDevice::PreRun()
{
  start();
}

bool TfBuilderDevice::start()
{
  // start all gRPC clients
  while (!mRpc->start(mTfBufferSize)) {
    // try to reach the scheduler unless we should exit
    if (IsRunningState() && NewStatePending()) {
      mShouldExit = true;
      return false;
    }

    std::this_thread::sleep_for(1s);
  }

  // we reached the scheduler instance, initialize everything else
  mRunning = true;

  if (!mStandalone && dplEnabled()) {
    auto& lOutputChan = GetChannel(getDplChannelName(), 0);
    mTfBuilder = std::make_unique<TimeFrameBuilder>(lOutputChan, mTfBufferSize, dplEnabled());
    mTfDplAdapter = std::make_unique<StfToDplAdapter>(lOutputChan);
  }

  // start TF forwarding thread
  mTfFwdThread = std::thread(&TfBuilderDevice::TfForwardThread, this);
  // start file sink
  mFileSink.start();

  // Start input handlers
  if (!mFlpInputHandler->start(mDiscoveryConfig)) {
    mShouldExit = true;
    DDLOG(fair::Severity::ERROR) << "Could not initialize input connections. Exiting.";
    throw "Input connection error";
    return false;
  }

  // start file source
  if (mStandalone) {
    mFileSource.start(*mStandaloneChannel, false);
  } else {
    mFileSource.start(GetChannel(mDplChannelName), mDplEnabled);
  }

  return true;
}

void TfBuilderDevice::stop()
{
  mRpc->stopAcceptingTfs();

  if (mTfDplAdapter) {
    mTfDplAdapter->stop();
  }

  if (mTfBuilder) {
    mTfBuilder->stop();
  }

  mRunning = false;

  // Stop the pipeline
  stopPipeline();

  // stop output handlers
  mFlpInputHandler->stop(mDiscoveryConfig);
  // signal and wait for the output thread
  mFileSink.stop();
  // join on fwd thread
  if (mTfFwdThread.joinable()) {
    mTfFwdThread.join();
  }

  //wait for the info thread
  if (mInfoThread.joinable()) {
    mInfoThread.join();
  }

  // stop the RPCs
  mRpc->stop();

  mDiscoveryConfig.reset();

  DDLOG(fair::Severity::trace) << "Reset() done... ";
}

void TfBuilderDevice::ResetTask()
{
  stop();
}

bool TfBuilderDevice::ConditionalRun()
{
  if (mShouldExit) {
    mRunning = false;
    throw std::string("intentional exit");
    return false;
  }

  // nothing to do here sleep for awhile
  std::this_thread::sleep_for(1s);
  return true;
}

void TfBuilderDevice::TfForwardThread()
{
  auto lFreqStartTime = std::chrono::high_resolution_clock::now();
  while (mRunning) {
    std::unique_ptr<SubTimeFrame> lTf = dequeue(eTfFwdIn);
    if (!lTf) {
      DDLOG(fair::Severity::WARNING) << "TfForwardThread(): Exiting... ";
      break;
    }

    const auto lTfId = lTf->header().mId;

    // MON: record frequency and size of TFs
    {
      mTfFreqSamples.Fill(
        1.0 / std::chrono::duration<double>(
                std::chrono::high_resolution_clock::now() - lFreqStartTime)
                .count());

      lFreqStartTime = std::chrono::high_resolution_clock::now();

      // size samples
      mTfSizeSamples.Fill(lTf->getDataSize());
    }

    if (!mStandalone) {
      try {
        static std::uint64_t sTfOutCnt = 0;
        if (++sTfOutCnt % 256 == 0) {
          DDLOGF(fair::Severity::TRACE, "Forwarding new TF to DPL. tf_id={} total={}",
            lTf->header().mId, sTfOutCnt);
        }

        if (dplEnabled()) {
          // adapt headers to include DPL processing header on the stack
          assert(mTfBuilder);
          mTfBuilder->adaptHeaders(lTf.get());

          // DPL Channel
          static thread_local unsigned long lThrottle = 0;
          if (++lThrottle % 100 == 0) {
            DDLOGF(fair::Severity::TRACE, "Sending STF to DPL. stf_id={:d} stf_size={:d} unique_equipments={:d}",
              lTf->header().mId, lTf->getDataSize(), lTf->getEquipmentIdentifiers().size());
          }

          // Send to DPL bridge
          assert (mTfDplAdapter);
          mTfDplAdapter->sendToDpl(std::move(lTf));
        }
      } catch (std::exception& e) {
        if (IsRunningState()) {
          DDLOGF(fair::Severity::ERROR, "StfOutputThread: exception on send. exception_what={:s}", e.what());
        } else {
          DDLOGF(fair::Severity::INFO, "StfOutputThread: shutting down. exception_what={:s}", e.what());
        }
        break;
      }
    }

    // decrement the size used by the TF
    // TODO: move this close to the output channel send
    mRpc->recordTfForwarded(lTfId);

  }

  DDLOG(fair::Severity::INFO) << "Exiting TF forwarding thread... ";
}

void TfBuilderDevice::InfoThread()
{
  // wait for the device to go into RUNNING state
  WaitForRunningState();

  while (IsRunningState()) {

    DDLOG(fair::Severity::INFO) << "Mean size of TimeFrames : " << mTfSizeSamples.Mean();
    DDLOG(fair::Severity::INFO) << "Mean TimeFrame frequency: " << mTfFreqSamples.Mean();
    DDLOG(fair::Severity::INFO) << "Number of queued TFs    : " << getPipelineSize(); // current value

    std::this_thread::sleep_for(2s);
  }

  DDLOGF(fair::Severity::trace, "Exiting info thread...");
}

}
} /* namespace o2::DataDistribution */
