//===------------------ Client.h - Client Implementation ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// gRPC Client for the remote plugin.
//
//===----------------------------------------------------------------------===//

#include "Debug.h"
#include "Utils.h"
#include "omptarget.h"
#include <google/protobuf/arena.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/channel_arguments.h>
#include <memory>
#include <numeric>

using grpc::Channel;
using grpc::ServerContext;

using remoteoffloading::RemoteOffload;

using namespace google;

class RemoteOffloadClient {
  const int Timeout;
  const int Shots;

  __tgt_target_table *Table;
  int DebugLevel;
  uint64_t MaxSize;
  int64_t BlockSize;

public:
  RemoteOffloadClient(std::shared_ptr<Channel> Channel, int Timeout, int Shots,
                      uint64_t MaxSize, int64_t BlockSize)
      : Timeout(Timeout), Shots(Shots), MaxSize(MaxSize), BlockSize(BlockSize),
        Stub(RemoteOffload::NewStub(Channel)) {
    DebugLevel = getDebugLevel();
    Arena = std::make_unique<protobuf::Arena>();
  }

  std::map<void *, void *> HostToRemoteTargetTableMap;
  std::map<void *, void *> HostToRemoteDataMap;
  std::unique_ptr<protobuf::Arena> Arena;

  template <typename Pre, typename Post>
  void call(Pre Preprocess, Post Postprocess, bool Retry = true,
            bool Deadline = true);

  template <typename Pre, typename Post, typename Fail>
  auto call(Pre Preprocess, Post Postprocess, Fail Failure, bool Retry = true,
            bool Deadline = true);

  void shutdown(void);
  /////////////////////////////////////////////////////////////////////////////
  // Remote Registrations.
  /////////////////////////////////////////////////////////////////////////////
  void registerLib(__tgt_bin_desc *Desc);
  void unregisterLib(__tgt_bin_desc *Desc);
  void registerRequires(int64_t Flags);

  /////////////////////////////////////////////////////////////////////////////
  // Target Runtime Library Functions.
  /////////////////////////////////////////////////////////////////////////////

  int32_t isValidBinary(__tgt_device_image *Image);
  int32_t getNumberOfDevices();

  /////////////////////////////////////////////////////////////////////////////

  int32_t initDevice(int32_t DeviceId);
  int32_t initRequires(int64_t RequiresFlags);

  /////////////////////////////////////////////////////////////////////////////

  __tgt_target_table *loadBinary(int32_t DeviceId, __tgt_device_image *Image);
  int64_t synchronize(int32_t DeviceId, __tgt_async_info *AsyncInfoPtr);
  int32_t isDataExchangeable(int32_t SrcDevId, int32_t DstDevId);

  /////////////////////////////////////////////////////////////////////////////

  void *dataAlloc(int32_t DeviceId, int64_t Size, void *HstPtr);
  int32_t dataSubmit(int32_t DeviceId, void *TgtPtr, void *HstPtr,
                     int64_t Size);
  int32_t dataRetrieve(int32_t DeviceId, void *HstPtr, void *TgtPtr,
                       int64_t Size);
  int32_t dataDelete(int32_t DeviceId, void *TgtPtr);
  int32_t dataSubmitAsync(int32_t DeviceId, void *TgtPtr, void *HstPtr,
                          int64_t Size, __tgt_async_info *AsyncInfoPtr);
  int32_t dataRetrieveAsync(int32_t DeviceId, void *HstPtr, void *TgtPtr,
                            int64_t Size, __tgt_async_info *AsyncInfoPtr);
  int32_t dataExchange(int32_t SrcDevId, void *SrcPtr, int32_t DstDevId,
                       void *DstPtr, int64_t Size);
  int32_t dataExchangeAsync(int32_t SrcDevId, void *SrcPtr, int32_t DstDevId,
                            void *DstPtr, int64_t Size,
                            __tgt_async_info *AsyncInfoPtr);

  ///////////////////////////////////////////////////////////////////////////////

  int32_t runTargetRegion(int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs,
                          ptrdiff_t *TgtOffsets, int32_t ArgNum);
  int32_t runTargetRegionAsync(int32_t DeviceId, void *TgtEntryPtr,
                               void **TgtArgs, ptrdiff_t *TgtOffsets,
                               int32_t ArgNum, __tgt_async_info *AsyncInfoPtr);

  int32_t runTargetTeamRegion(int32_t DeviceId, void *TgtEntryPtr,
                              void **TgtArgs, ptrdiff_t *TgtOffsets,
                              int32_t ArgNum, int32_t TeamNum,
                              int32_t ThreadLimit, uint64_t LoopTripCount);

  int32_t runTargetTeamRegionAsync(int32_t DeviceId, void *TgtEntryPtr,
                                   void **TgtArgs, ptrdiff_t *TgtOffsets,
                                   int32_t ArgNum, int32_t TeamNum,
                                   int32_t ThreadLimit, uint64_t LoopTripCount,
                                   __tgt_async_info *AsyncInfoPtr);

private:
  std::unique_ptr<RemoteOffload::Stub> Stub;
};

class RemoteClientManager {
private:
  std::vector<std::string> Addresses;
  std::vector<RemoteOffloadClient> Clients;
  std::vector<int> Devices;

  std::pair<int32_t, int32_t> mapDeviceId(int32_t DeviceId);
  int DebugLevel;

public:
  RemoteClientManager(std::vector<std::string> Addresses, int Timeout,
                      int Shots, uint64_t MaxSize, int64_t BlockSize)
      : Addresses(Addresses) {
    grpc::ChannelArguments ChArgs;
    ChArgs.SetMaxReceiveMessageSize(-1);
    DebugLevel = getDebugLevel();
    for (auto Address : Addresses) {
      Clients.emplace_back(
          grpc::CreateCustomChannel(Address, grpc::InsecureChannelCredentials(),
                                    ChArgs),
          Timeout, Shots, MaxSize, BlockSize);
    }
  }

  void shutdown(void);

  /////////////////////////////////////////////////////////////////////////////
  // Remote Registrations.
  /////////////////////////////////////////////////////////////////////////////

  void registerLib(__tgt_bin_desc *Desc);
  void unregisterLib(__tgt_bin_desc *Desc);
  void registerRequires(int64_t Flags);

  /////////////////////////////////////////////////////////////////////////////
  // Target Runtime Library Functions.
  /////////////////////////////////////////////////////////////////////////////

  int32_t isValidBinary(__tgt_device_image *Image);
  int32_t getNumberOfDevices();

  /////////////////////////////////////////////////////////////////////////////

  int32_t initDevice(int32_t DeviceId);
  int32_t initRequires(int64_t RequiresFlags);

  /////////////////////////////////////////////////////////////////////////////

  __tgt_target_table *loadBinary(int32_t DeviceId, __tgt_device_image *Image);
  int64_t synchronize(int32_t DeviceId, __tgt_async_info *AsyncInfoPtr);
  int32_t isDataExchangeable(int32_t SrcDevId, int32_t DstDevId);

  /////////////////////////////////////////////////////////////////////////////

  void *dataAlloc(int32_t DeviceId, int64_t Size, void *HstPtr);
  int32_t dataSubmit(int32_t DeviceId, void *TgtPtr, void *HstPtr,
                     int64_t Size);
  int32_t dataRetrieve(int32_t DeviceId, void *HstPtr, void *TgtPtr,
                       int64_t Size);
  int32_t dataDelete(int32_t DeviceId, void *TgtPtr);
  int32_t dataSubmitAsync(int32_t DeviceId, void *TgtPtr, void *HstPtr,
                          int64_t Size, __tgt_async_info *AsyncInfoPtr);
  int32_t dataRetrieveAsync(int32_t DeviceId, void *HstPtr, void *TgtPtr,
                            int64_t Size, __tgt_async_info *AsyncInfoPtr);
  int32_t dataExchange(int32_t SrcDevId, void *SrcPtr, int32_t DstDevId,
                       void *DstPtr, int64_t Size);
  int32_t dataExchangeAsync(int32_t SrcDevId, void *SrcPtr, int32_t DstDevId,
                            void *DstPtr, int64_t Size,
                            __tgt_async_info *AsyncInfoPtr);

  ///////////////////////////////////////////////////////////////////////////////

  int32_t runTargetRegion(int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs,
                          ptrdiff_t *TgtOffsets, int32_t ArgNum);
  int32_t runTargetRegionAsync(int32_t DeviceId, void *TgtEntryPtr,
                               void **TgtArgs, ptrdiff_t *TgtOffsets,
                               int32_t ArgNum, __tgt_async_info *AsyncInfoPtr);

  int32_t runTargetTeamRegion(int32_t DeviceId, void *TgtEntryPtr,
                              void **TgtArgs, ptrdiff_t *TgtOffsets,
                              int32_t ArgNum, int32_t TeamNum,
                              int32_t ThreadLimit, uint64_t LoopTripCount);

  int32_t runTargetTeamRegionAsync(int32_t DeviceId, void *TgtEntryPtr,
                                   void **TgtArgs, ptrdiff_t *TgtOffsets,
                                   int32_t ArgNum, int32_t TeamNum,
                                   int32_t ThreadLimit, uint64_t LoopTripCount,
                                   __tgt_async_info *AsyncInfoPtr);
};
