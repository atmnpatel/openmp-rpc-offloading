//===--------------------- rtl.cpp - Remote RTL Plugin --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RTL for Host.
//
//===----------------------------------------------------------------------===//

#include <cstddef>
#include <memory>
#include <string>

#include "Client.h"
#include "omptarget.h"
#include "omptargetplugin.h"

#define TARGET_NAME RPC
#define DEBUG_PREFIX "Target " GETNAME(TARGET_NAME) " RTL"

#include "../../common/elf_common.c"

RemoteClientManager *Manager;

__attribute__((constructor(101))) void initRPC() {
  DP("Init RPC library!\n");

  std::vector<std::string> ServerAddresses = {"0.0.0.0:50051"};
  if (const char *Env = std::getenv("GRPC_ADDRESS")) {
    ServerAddresses.clear();
    std::string AddressString = Env;
    std::string Delimiter = ",";

    size_t Pos = 0;
    std::string Token;
    while ((Pos = AddressString.find(Delimiter)) != std::string::npos) {
        Token = AddressString.substr(0, Pos);
        ServerAddresses.push_back(Token);
        AddressString.erase(0, Pos + Delimiter.length());
    }
    ServerAddresses.push_back(AddressString);
  }

  int Timeout = 5;
  if (const char *Env1 = std::getenv("GRPC_LATENCY")) {
    if (const char *Env2 = std::getenv("GRPC_LATENCY_MULTIPLIER"))
      Timeout += std::stoi(Env1) * std::stoi(Env2);
    else
      Timeout += std::stoi(Env1);
  }

  int Shots = 1;
  if (const char *Env = std::getenv("GRPC_TRY"))
    Shots = std::stoi(Env);

  uint64_t MaxSize = 1 << 30;
  if (const char *Env = std::getenv("GRPC_ALLOCATOR_MAX"))
    MaxSize = std::stoi(Env);

  int BlockSize = 1 << 20;
  if (const char *Env = std::getenv("GRPC_BLOCK_SIZE"))
    BlockSize = std::stoi(Env);

  Manager = new RemoteClientManager(ServerAddresses, Timeout, Shots, MaxSize, BlockSize);
}

__attribute__((destructor(101))) void deinitRPC() {
  Manager->shutdown();
  DP("Deinit RPC library!\n");
  delete Manager;
}

// Exposed library API function
#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////
// Registrations with the Remote.
///////////////////////////////////////////////////////////////////////////////

void __tgt_rtl_register_lib(__tgt_bin_desc *Desc) { Manager->registerLib(Desc); }

void __tgt_rtl_unregister_lib(__tgt_bin_desc *Desc) {
  Manager->unregisterLib(Desc);
}

void __tgt_rtl_register_requires(int64_t Flags) {
  Manager->registerRequires(Flags);
}

///////////////////////////////////////////////////////////////////////////////
// Target Runtime Library Functions.
///////////////////////////////////////////////////////////////////////////////

int32_t __tgt_rtl_is_valid_binary(__tgt_device_image *Image) {
  return Manager->isValidBinary(Image);
}

int32_t __tgt_rtl_number_of_devices() { return Manager->getNumberOfDevices(); }

///////////////////////////////////////////////////////////////////////////////

int32_t __tgt_rtl_init_device(int32_t DeviceId) {
  return Manager->initDevice(DeviceId);
}

int64_t __tgt_rtl_init_requires(int64_t RequiresFlags) {
  return Manager->initRequires(RequiresFlags);
}

///////////////////////////////////////////////////////////////////////////////

__tgt_target_table *__tgt_rtl_load_binary(int32_t DeviceId,
                                          __tgt_device_image *Image) {
  return Manager->loadBinary(DeviceId, (__tgt_device_image *)Image);
}

/*
int32_t __tgt_rtl_synchronize(int32_t DeviceId,
                              __tgt_async_info *AsyncInfoPtr) {
  return Manager->synchronize(DeviceId, AsyncInfoPtr);
}
*/

int32_t __tgt_rtl_is_data_exchangable(int32_t SrcDevId, int32_t DstDevId) {
  return Manager->isDataExchangeable(SrcDevId, DstDevId);
}

///////////////////////////////////////////////////////////////////////////////

void *__tgt_rtl_data_alloc(int32_t DeviceId, int64_t Size, void *HstPtr) {
  return Manager->dataAlloc(DeviceId, Size, HstPtr);
}

int32_t __tgt_rtl_data_submit(int32_t DeviceId, void *TgtPtr, void *HstPtr,
                              int64_t Size) {
  return Manager->dataSubmit(DeviceId, TgtPtr, HstPtr, Size);
}

int32_t __tgt_rtl_data_submit_async(int32_t DeviceId, void *TgtPtr,
                                    void *HstPtr, int64_t Size,
                                    __tgt_async_info *AsyncInfoPtr) {
  return Manager->dataSubmitAsync(DeviceId, TgtPtr, HstPtr, Size, AsyncInfoPtr);
}

int32_t __tgt_rtl_data_retrieve(int32_t DeviceId, void *HstPtr, void *TgtPtr,
                                int64_t Size) {
  return Manager->dataRetrieve(DeviceId, HstPtr, TgtPtr, Size);
}

int32_t __tgt_rtl_data_retrieve_async(int32_t DeviceId, void *HstPtr,
                                      void *TgtPtr, int64_t Size,
                                      __tgt_async_info *AsyncInfoPtr) {
  return Manager->dataRetrieveAsync(DeviceId, HstPtr, TgtPtr, Size,
                                   AsyncInfoPtr);
}

int32_t __tgt_rtl_data_delete(int32_t DeviceId, void *tgt_ptr) {
  return Manager->dataDelete(DeviceId, tgt_ptr);
}

int32_t __tgt_rtl_data_exchange(int32_t SrcDevId, void *SrcPtr,
                                int32_t DstDevId, void *DstPtr, int64_t Size) {
  return Manager->dataExchange(SrcDevId, SrcPtr, DstDevId, DstPtr, Size);
}

int32_t __tgt_rtl_data_exchange_async(int32_t SrcDevId, void *SrcPtr,
                                      int32_t DstDevId, void *DstPtr,
                                      int64_t Size,
                                      __tgt_async_info *AsyncInfoPtr) {
  return Manager->dataExchangeAsync(SrcDevId, SrcPtr, DstDevId, DstPtr, Size,
                                   AsyncInfoPtr);
}

///////////////////////////////////////////////////////////////////////////////

int32_t __tgt_rtl_run_target_region(int32_t DeviceId, void *TgtEntryPtr,
                                    void **TgtArgs, ptrdiff_t *TgtOffsets,
                                    int32_t ArgNum) {
  return Manager->runTargetRegion(DeviceId, TgtEntryPtr, TgtArgs, TgtOffsets,
                                 ArgNum);
}

int32_t __tgt_rtl_run_target_region_async(int32_t DeviceId, void *TgtEntryPtr,
                                          void **TgtArgs, ptrdiff_t *TgtOffsets,
                                          int32_t ArgNum,
                                          __tgt_async_info *AsyncInfoPtr) {
  return Manager->runTargetRegionAsync(DeviceId, TgtEntryPtr, TgtArgs,
                                      TgtOffsets, ArgNum, AsyncInfoPtr);
}

int32_t __tgt_rtl_run_target_team_region(int32_t DeviceId, void *TgtEntryPtr,
                                         void **TgtArgs, ptrdiff_t *TgtOffsets,
                                         int32_t ArgNum, int32_t TeamNum,
                                         int32_t ThreadLimit,
                                         uint64_t LoopTripCount) {
  return Manager->runTargetTeamRegion(DeviceId, TgtEntryPtr, TgtArgs, TgtOffsets,
                                     ArgNum, TeamNum, ThreadLimit,
                                     LoopTripCount);
}

int32_t __tgt_rtl_run_target_team_region_async(
    int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs, ptrdiff_t *TgtOffsets,
    int32_t ArgNum, int32_t TeamNum, int32_t ThreadLimit,
    uint64_t LoopTripCount, __tgt_async_info *AsyncInfoPtr) {
  return Manager->runTargetTeamRegionAsync(
      DeviceId, TgtEntryPtr, TgtArgs, TgtOffsets, ArgNum, TeamNum, ThreadLimit,
      LoopTripCount, AsyncInfoPtr);
}

// Exposed library API function
#ifdef __cplusplus
}
#endif
