//===----------------- Server.cpp - Server Implementation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Offloading gRPC server for remote host.
//
//===----------------------------------------------------------------------===//

#include "Server.h"
#include "omptarget.h"
#include "openmp.grpc.pb.h"
#include "openmp.pb.h"

#include <cmath>
#include <future>

using grpc::WriteOptions;

extern std::promise<void> ShutdownPromise;

Status
RemoteOffloadImpl::Shutdown(ServerContext *Context,
                               const Null *Request,
                               Null *Reply) {
  SERVER_DBG("Shutting down the Server\n");
  ShutdownPromise.set_value();

  return Status::OK;
}

///////////////////////////////////////////////////////////////////////////////
// Remote Registrations.
///////////////////////////////////////////////////////////////////////////////

Status
RemoteOffloadImpl::RegisterLib(ServerContext *Context,
                               const TargetBinaryDescription *Description,
                               Null *Reply) {
  SERVER_DBG("Registering Library\n");

  if (Desc) {
    freeTargetBinaryDescription(Desc); // FIXME: Double Free Corruption?
    Desc = nullptr;
    SERVER_DBG("Cleaned up prior target binary Description - last run was "
               "unsuccessful.\n");
  }

  Desc = (__tgt_bin_desc *)malloc(sizeof(__tgt_bin_desc));

  // HostToRemoteDeviceImage, HostToRemoteOffloadEntry map host addresses to
  // device images and global offload entries on the remote so I/O is minimal.
  unloadTargetBinaryDescription(Description, Desc, HostToRemoteDeviceImage,
                                HostToRemoteOffloadEntry);
  PM->RTLs.RegisterLib(Desc);

  SERVER_DBG("Registered Library\n");
  return Status::OK;
}

Status RemoteOffloadImpl::UnregisterLib(ServerContext *Context,
                                        const Null *Request, Null *Reply) {
  SERVER_DBG("Unregistering Library\n");

  PM->RTLs.UnregisterLib(Desc);
  freeTargetBinaryDescription(Desc);
  Desc = nullptr;

  SERVER_DBG("Unregistered Library\n");
  return Status::OK;
}

Status RemoteOffloadImpl::RegisterRequires(ServerContext *Context,
                                           const LongScalar *Flags,
                                           Null *Reply) {
  SERVER_DBG("Register Requires\n");

  PM->RTLs.RegisterRequires(Flags->number());

  SERVER_DBG("Registered Requires\n");
  return Status::OK;
}

///////////////////////////////////////////////////////////////////////////////

Status RemoteOffloadImpl::IsValidBinary(ServerContext *Context,
                                        const TargetDeviceImagePtr *DeviceImage,
                                        Scalar *IsValid) {
  SERVER_DBG("Checking if binary (%p) is valid\n",
             (void *)DeviceImage->image_ptr());

  __tgt_device_image *Image =
      HostToRemoteDeviceImage[(void *)DeviceImage->image_ptr()];
  assert(Image && "Image doesn't exist in remote");

  // This function is called when the remote libomptarget plugin does not know
  // which RTLs are used, so we iterate through them all. We assume that there
  // should be a unique mapping from valid binary images to run time libraries.
  for (auto &RTL : PM->RTLs.AllRTLs)
    if (auto Ret = RTL.is_valid_binary(Image)) {
      IsValid->set_number(Ret);
      break;
    }

  SERVER_DBG("Checked if binary (%p) is valid\n",
             (void *)DeviceImage->image_ptr());
  return Status::OK;
}

Status RemoteOffloadImpl::GetNumberOfDevices(ServerContext *Context,
                                             const Null *Null,
                                             Scalar *NumberOfDevices) {
  SERVER_DBG("Getting Number of Devices\n");

  // GetNumberOfDevices will be called by the host before the library is
  // registered with the remote, so the remote plugins need to be loaded
  // explicitly.
  std::call_once(PM->RTLs.initFlag, &RTLsTy::LoadRTLs, &PM->RTLs);

  int32_t Devices = 0;
  PM->RTLsMtx.lock();
  for (auto &RTL : PM->RTLs.AllRTLs)
    Devices += RTL.NumberOfDevices;
  PM->RTLsMtx.unlock();

  NumberOfDevices->set_number(Devices);

  SERVER_DBG("Got Number of Devices\n");
  return Status::OK;
}

///////////////////////////////////////////////////////////////////////////////

Status RemoteOffloadImpl::InitDevice(ServerContext *Context,
                                     const Scalar *DeviceNum, Scalar *Reply) {
  SERVER_DBG("Initializing Device %d\n", DeviceNum->number());

  auto Ret = PM->Devices[DeviceNum->number()].RTL->init_device(
      mapHostRTLDeviceId(DeviceNum->number()));
  Reply->set_number(Ret);

  SERVER_DBG("Initialized Device %d\n", DeviceNum->number());
  return Status::OK;
}

Status RemoteOffloadImpl::InitRequires(ServerContext *Context,
                                       const LongScalar *RequiresFlag,
                                       Scalar *Reply) {
  SERVER_DBG("Initializing Requires for Devices\n");

  for (auto &Device : PM->Devices)
    if (Device.RTL->init_requires)
      Device.RTL->init_requires(RequiresFlag->number());
  Reply->set_number(RequiresFlag->number());

  SERVER_DBG("Initialized Requires for Devices\n");
  return Status::OK;
}

///////////////////////////////////////////////////////////////////////////////

Status RemoteOffloadImpl::LoadBinary(ServerContext *Context,
                                     const Binary *Binary, TargetTable *Reply) {
  SERVER_DBG("Loading binary (%p) to Device %d\n", (void *)Binary->image_ptr(),
             Binary->device_id());

  __tgt_device_image *Image =
      HostToRemoteDeviceImage[(void *)Binary->image_ptr()];
  assert(Image && "Image not found in remote.");

  Table = PM->Devices[Binary->device_id()].RTL->load_binary(
      mapHostRTLDeviceId(Binary->device_id()), Image);
  if (Table)
    loadTargetTable(Table, *Reply, Image);

  SERVER_DBG("Loaded binary (%p) to Device %d\n", (void *)Binary->image_ptr(),
             Binary->device_id());
  return Status::OK;
}

Status RemoteOffloadImpl::Synchronize(ServerContext *Context,
                                      const SynchronizeDevice *Info,
                                      Scalar *Reply) {
  SERVER_DBG("Synchronizing Device %d (probably won't work)\n",
             Info->device_id());

  void *AsyncInfoPtr = (void *)Info->queue_ptr();
  auto Ret = 0;
  if (PM->Devices[Info->device_id()].RTL->synchronize)
    Ret = PM->Devices[Info->device_id()].synchronize(
        (__tgt_async_info *)AsyncInfoPtr);

  Reply->set_number(Ret);

  SERVER_DBG("Synchronized Device %d\n", Info->device_id());
  return Status::OK;
}

Status RemoteOffloadImpl::IsDataExchangeable(ServerContext *Context,
                                             const DevicePair *Request,
                                             Scalar *Reply) {
  SERVER_DBG("Checking if data exchangable between device %d and device %d\n",
             Request->src_dev_id(), Request->dst_dev_id());

  int32_t Ret = -1;
  if (PM->Devices[mapHostRTLDeviceId(Request->src_dev_id())]
          .RTL->is_data_exchangable)
    Ret = PM->Devices[mapHostRTLDeviceId(Request->src_dev_id())]
              .RTL->is_data_exchangable(Request->src_dev_id(),
                                        Request->dst_dev_id());

  Reply->set_number(Ret);

  SERVER_DBG("Checked if data exchangable between device %d and device %d\n",
             Request->src_dev_id(), Request->dst_dev_id());
  return Status::OK;
}

///////////////////////////////////////////////////////////////////////////////

Status RemoteOffloadImpl::DataAlloc(ServerContext *Context,
                                    const AllocData *Request, Pointer *Reply) {
  SERVER_DBG("Allocating %ld bytes on Device %d\n", Request->size(),
             Request->device_id());

  uint64_t TgtPtr = (uint64_t)PM->Devices[Request->device_id()].RTL->data_alloc(
      mapHostRTLDeviceId(Request->device_id()), Request->size(),
      (void *)Request->hst_ptr());
  Reply->set_number(TgtPtr);

  SERVER_DBG("Allocated at " DPxMOD "\n", DPxPTR((void *)TgtPtr));

  return Status::OK;
}

Status RemoteOffloadImpl::DataSubmit(ServerContext *Context,
                                     ServerReader<SubmitData> *Reader,
                                     Scalar *Reply) {
  SubmitData Request;
  char *HostCopy = nullptr;
  while (Reader->Read(&Request)) {
    if (Request.start() == 0 && Request.size() == Request.data().size()) {
      SERVER_DBG("Submitting %lu bytes to (%p) on Device %d\n",
                 Request.data().size(), (void *)Request.tgt_ptr(),
                 Request.device_id());

      SERVER_DBG("  Host Pointer Info: %p, %p\n", (void *)Request.hst_ptr(),
                 static_cast<const void *>(Request.data().data()));

      Reader->SendInitialMetadata();

      Reply->set_number(PM->Devices[Request.device_id()].RTL->data_submit(
          mapHostRTLDeviceId(Request.device_id()), (void *)Request.tgt_ptr(),
          (void *)Request.data().data(), Request.data().size()));

      SERVER_DBG("Submitted %lu bytes to (%p) on Device %d\n",
                 Request.data().size(), (void *)Request.tgt_ptr(),
                 Request.device_id());

      return Status::OK;
    } else {
      if (!HostCopy) {
        HostCopy = (char *)malloc(sizeof(char) * Request.size());
        Reader->SendInitialMetadata();
      }

      SERVER_DBG("Submitting %lu-%lu/%lu bytes to (%p) on Device %d\n",
                 Request.start(), Request.start() + Request.data().size(),
                 Request.size(), (void *)Request.tgt_ptr(),
                 Request.device_id());

      memcpy((void *)((char *)HostCopy + Request.start()),
             Request.data().data(), Request.data().size());
    }
  }
  SERVER_DBG("  Host Pointer Info: %p, %p\n", (void *)Request.hst_ptr(),
             static_cast<const void *>(Request.data().data()));

  Reply->set_number(PM->Devices[Request.device_id()].RTL->data_submit(
      mapHostRTLDeviceId(Request.device_id()), (void *)Request.tgt_ptr(),
      HostCopy, Request.size()));

  free(HostCopy);

  SERVER_DBG("Submitted %lu bytes to (%p) on Device %d\n",
             Request.data().size(), (void *)Request.tgt_ptr(),
             Request.device_id());

  return Status::OK;
}

Status RemoteOffloadImpl::DataSubmitAsync(ServerContext *Context,
                                          ServerReader<SubmitDataAsync> *Reader,
                                          Scalar *Reply) {
  SubmitDataAsync Request;
  char *HostCopy = nullptr;
  while (Reader->Read(&Request)) {
    if (Request.start() == 0 && Request.size() == Request.data().size()) {
      SERVER_DBG("Submitting %lu bytes async to (%p) on Device %d\n",
                 Request.data().size(), (void *)Request.tgt_ptr(),
                 Request.device_id());

      SERVER_DBG("  Host Pointer Info: %p, %p\n", (void *)Request.hst_ptr(),
                 static_cast<const void *>(Request.data().data()));

      Reader->SendInitialMetadata();

      Reply->set_number(PM->Devices[Request.device_id()].RTL->data_submit(
          mapHostRTLDeviceId(Request.device_id()), (void *)Request.tgt_ptr(),
          (void *)Request.data().data(), Request.data().size()));

      SERVER_DBG("Submitted %lu bytes async to (%p) on Device %d\n",
                 Request.data().size(), (void *)Request.tgt_ptr(),
                 Request.device_id());

      return Status::OK;
    } else {
      if (!HostCopy) {
        HostCopy = (char *)malloc(sizeof(char) * Request.size());
        Reader->SendInitialMetadata();
      }

      SERVER_DBG("Submitting %lu-%lu/%lu bytes async to (%p) on Device %d\n",
                 Request.start(), Request.start() + Request.data().size(),
                 Request.size(), (void *)Request.tgt_ptr(),
                 Request.device_id());

      memcpy((void *)((char *)HostCopy + Request.start()),
             Request.data().data(), Request.data().size());
    }
  }
  SERVER_DBG("  Host Pointer Info: %p, %p\n", (void *)Request.hst_ptr(),
             static_cast<const void *>(Request.data().data()));


  Reply->set_number(PM->Devices[Request.device_id()].RTL->data_submit(
      mapHostRTLDeviceId(Request.device_id()), (void *)Request.tgt_ptr(),
      HostCopy, Request.size()));

  free(HostCopy);

  SERVER_DBG("Submitted %lu bytes to (%p) on Device %d\n",
             Request.data().size(), (void *)Request.tgt_ptr(),
             Request.device_id());

  return Status::OK;
}

Status RemoteOffloadImpl::DataRetrieve(ServerContext *Context,
                                       const RetrieveData *Request,
                                       ServerWriter<Data> *Writer) {
  auto *HstPtr = malloc(sizeof(char) * Request->size());
  auto Ret = PM->Devices[Request->device_id()].RTL->data_retrieve(
      mapHostRTLDeviceId(Request->device_id()), HstPtr,
      (void *)Request->tgt_ptr(), Request->size());

  if (Arena->SpaceUsed() >= MaxSize)
    Arena->Reset();

  if (Request->size() > BlockSize) {
    int64_t Start = 0, End = BlockSize;
    for (auto I = 0; I < ceil((float)Request->size() / BlockSize); I++) {
      auto *Reply = protobuf::Arena::CreateMessage<Data>(Arena.get());

      Reply->set_start(Start);
      Reply->set_size(Request->size());
      Reply->set_data((char *)HstPtr + Start, End - Start);
      Reply->set_ret(Ret);

      SERVER_DBG("Retrieving %lu-%lu/%lu bytes from (%p) on Device %d\n", Start,
                 End, Request->size(), (void *)Request->tgt_ptr(),
                 mapHostRTLDeviceId(Request->device_id()));

      if (!Writer->Write(*Reply)) {
        CLIENT_DBG("Broken stream when submitting data\n");
      }

      SERVER_DBG("Retrieved %lu-%lu/%lu bytes from (%p) on Device %d\n", Start,
                 End, Request->size(), (void *)Request->tgt_ptr(),
                 mapHostRTLDeviceId(Request->device_id()));

      Start += BlockSize;
      End += BlockSize;
      if (End >= Request->size())
        End = Request->size();
    }
  } else {
    auto *Reply = protobuf::Arena::CreateMessage<Data>(Arena.get());

    SERVER_DBG("Retrieve %lu bytes from (%p) on Device %d\n", Request->size(),
               (void *)Request->tgt_ptr(),
               mapHostRTLDeviceId(Request->device_id()));

    Reply->set_start(0);
    Reply->set_data((char *)HstPtr, Request->size());
    Reply->set_ret(Ret);
    Reply->set_size(Request->size());

    SERVER_DBG("Retrieved %lu bytes from (%p) on Device %d\n", Request->size(),
               (void *)Request->tgt_ptr(),
               mapHostRTLDeviceId(Request->device_id()));

    Writer->WriteLast(*Reply, WriteOptions());
  }

  free(HstPtr);

  return Status::OK;
}

Status RemoteOffloadImpl::DataRetrieveAsync(ServerContext *Context,
                                            const RetrieveDataAsync *Request,
                                            ServerWriter<Data> *Writer) {
  auto *HstPtr = malloc(sizeof(char) * Request->size());
  auto Ret = PM->Devices[Request->device_id()].RTL->data_retrieve(
      mapHostRTLDeviceId(Request->device_id()), HstPtr,
      (void *)Request->tgt_ptr(), Request->size());

  if (Arena->SpaceUsed() >= MaxSize)
    Arena->Reset();

  if (Request->size() > BlockSize) {
    int64_t Start = 0, End = BlockSize;
    for (auto I = 0; I < ceil((float) Request->size() / BlockSize); I++) {
      auto *Reply = protobuf::Arena::CreateMessage<Data>(Arena.get());

      Reply->set_start(Start);
      Reply->set_size(Request->size());
      Reply->set_data((char *)HstPtr + Start, End - Start);
      Reply->set_ret(Ret);

      SERVER_DBG("Retrieving %lu-%lu/%lu bytes from (%p) on Device %d\n", Start,
                 End, Request->size(), (void *)Request->tgt_ptr(),
                 mapHostRTLDeviceId(Request->device_id()));

      if (!Writer->Write(*Reply)) {
        CLIENT_DBG("Broken stream when submitting data\n");
      }

      SERVER_DBG("Retrieved %lu-%lu/%lu bytes from (%p) on Device %d\n", Start,
                 End, Request->size(), (void *)Request->tgt_ptr(),
                 mapHostRTLDeviceId(Request->device_id()));

      Start += BlockSize;
      End += BlockSize;
      if (End >= Request->size())
        End = Request->size();
    }
  } else {
    auto *Reply = protobuf::Arena::CreateMessage<Data>(Arena.get());

    SERVER_DBG("Retrieve %lu bytes from (%p) on Device %d\n", Request->size(),
               (void *)Request->tgt_ptr(),
               mapHostRTLDeviceId(Request->device_id()));

    Reply->set_start(0);
    Reply->set_size(Request->size());
    Reply->set_data((char *)HstPtr, Request->size());
    Reply->set_ret(Ret);

    SERVER_DBG("Retrieved %lu bytes from (%p) on Device %d\n", Request->size(),
               (void *)Request->tgt_ptr(),
               mapHostRTLDeviceId(Request->device_id()));

    Writer->WriteLast(*Reply, WriteOptions());
  }

  free(HstPtr);

  return Status::OK;
}

Status RemoteOffloadImpl::DataExchange(ServerContext *Context,
                                       const ExchangeData *Request,
                                       Scalar *Reply) {
  SERVER_DBG(
      "Exchanging Data from Device %d (%p) to Device %d (%p) of size %lu\n",
      mapHostRTLDeviceId(Request->src_dev_id()), (void *)Request->src_ptr(),
      mapHostRTLDeviceId(Request->dst_dev_id()), (void *)Request->dst_ptr(),
      Request->size());

  if (PM->Devices[Request->src_dev_id()].RTL->data_exchange) {
    auto Ret = PM->Devices[Request->src_dev_id()].RTL->data_exchange(
        mapHostRTLDeviceId(Request->src_dev_id()), (void *)Request->src_ptr(),
        mapHostRTLDeviceId(Request->dst_dev_id()), (void *)Request->dst_ptr(),
        Request->size());
    Reply->set_number(Ret);
  } else
    Reply->set_number(-1);

  SERVER_DBG(
      "Exchanged Data from Device %d (%p) to Device %d (%p) of size %lu\n",
      mapHostRTLDeviceId(Request->src_dev_id()), (void *)Request->src_ptr(),
      mapHostRTLDeviceId(Request->dst_dev_id()), (void *)Request->dst_ptr(),
      Request->size());
  return Status::OK;
}

Status RemoteOffloadImpl::DataExchangeAsync(ServerContext *Context,
                                            const ExchangeDataAsync *Request,
                                            Scalar *Reply) {
  SERVER_DBG(
      "Exchanging Data asynchronously from Device %d (%p) to Device %d (%p) of "
      "size %lu\n",
      mapHostRTLDeviceId(Request->src_dev_id()), (void *)Request->src_ptr(),
      mapHostRTLDeviceId(Request->dst_dev_id()), (void *)Request->dst_ptr(),
      Request->size());

  if (PM->Devices[Request->src_dev_id()].RTL->data_exchange) {
    int32_t Ret = PM->Devices[Request->src_dev_id()].RTL->data_exchange(
        mapHostRTLDeviceId(Request->src_dev_id()), (void *)Request->src_ptr(),
        mapHostRTLDeviceId(Request->dst_dev_id()), (void *)Request->dst_ptr(),
        Request->size());
    Reply->set_number(Ret);
  } else
    Reply->set_number(-1);

  SERVER_DBG(
      "Exchanged Data asynchronously from Device %d (%p) to Device %d (%p) of "
      "size %lu\n",
      mapHostRTLDeviceId(Request->src_dev_id()), (void *)Request->src_ptr(),
      mapHostRTLDeviceId(Request->dst_dev_id()), (void *)Request->dst_ptr(),
      Request->size());
  return Status::OK;
}

Status RemoteOffloadImpl::DataDelete(ServerContext *Context,
                                     const DeleteData *Request, Scalar *Reply) {
  SERVER_DBG("Deleting data from (%p) on Device %d\n",
             (void *)Request->tgt_ptr(),
             mapHostRTLDeviceId(Request->device_id()));

  auto Ret = PM->Devices[Request->device_id()].RTL->data_delete(
      mapHostRTLDeviceId(Request->device_id()), (void *)Request->tgt_ptr());
  Reply->set_number(Ret);

  SERVER_DBG("Deleted data from (%p) on Device %d\n",
             (void *)Request->tgt_ptr(),
             mapHostRTLDeviceId(Request->device_id()));
  return Status::OK;
}

///////////////////////////////////////////////////////////////////////////////

Status RemoteOffloadImpl::RunTargetRegion(ServerContext *Context,
                                          const TargetRegion *Request,
                                          Scalar *Reply) {
  SERVER_DBG("Running TargetRegion on Device %d with %d args\n",
             mapHostRTLDeviceId(Request->device_id()), Request->arg_num());

  char **TgtArgs = (char **)malloc(sizeof(char *) * Request->arg_num());
  for (auto I = 0; I < Request->arg_num(); I++) {
    TgtArgs[I] = (char *)malloc(sizeof(char) * 2);
    TgtArgs[I] = (char *)Request->tgt_args()[I];
  }

  ptrdiff_t *TgtOffsets =
      (ptrdiff_t *)malloc(sizeof(ptrdiff_t) * Request->arg_num());
  const auto *TgtOffsetItr = Request->tgt_offsets().begin();
  for (auto I = 0; I < Request->arg_num(); I++, TgtOffsetItr++)
    TgtOffsets[I] = (ptrdiff_t)*TgtOffsetItr;

  void *TgtEntryPtr = ((__tgt_offload_entry *)Request->tgt_entry_ptr())->addr;

  auto Ret = PM->Devices[Request->device_id()].RTL->run_region(
      mapHostRTLDeviceId(Request->device_id()), TgtEntryPtr, (void **)TgtArgs,
      TgtOffsets, Request->arg_num());

  Reply->set_number(Ret);

  // for (auto I = 0; I < Request->arg_num(); I++)
    // free(TgtArgs[I]);

  free(TgtArgs);
  free(TgtOffsets);

  SERVER_DBG("Ran TargetRegion on Device %d with %d args\n",
             mapHostRTLDeviceId(Request->device_id()), Request->arg_num());
  return Status::OK;
}

Status RemoteOffloadImpl::RunTargetRegionAsync(ServerContext *Context,
                                               const TargetRegionAsync *Request,
                                               Scalar *Reply) {
  SERVER_DBG("Running TargetRegionAsync on Device %d with %d args\n",
             mapHostRTLDeviceId(Request->device_id()), Request->arg_num());

  char **TgtArgs = (char **)malloc(sizeof(char *) * Request->arg_num());
  for (auto I = 0; I < Request->arg_num(); I++) {
    TgtArgs[I] = (char *)malloc(sizeof(char) * 2);
    TgtArgs[I] = (char *)Request->tgt_args()[I];
  }

  ptrdiff_t *TgtOffsets =
      (ptrdiff_t *)malloc(sizeof(ptrdiff_t) * Request->arg_num());
  const auto *TgtOffsetItr = Request->tgt_offsets().begin();
  for (auto I = 0; I < Request->arg_num(); I++, TgtOffsetItr++)
    TgtOffsets[I] = (ptrdiff_t)*TgtOffsetItr;

  void *TgtEntryPtr = ((__tgt_offload_entry *)Request->tgt_entry_ptr())->addr;

  int32_t Ret = PM->Devices[Request->device_id()].RTL->run_region(
      mapHostRTLDeviceId(Request->device_id()), TgtEntryPtr, (void **)TgtArgs,
      TgtOffsets, Request->arg_num());

  Reply->set_number(Ret);

  // for (auto I = 0; I < Request->arg_num(); I++)
    // free(TgtArgs[I]);

  free(TgtArgs);
  free(TgtOffsets);

  SERVER_DBG("Ran TargetRegionAsync on Device %d with %d args\n",
             mapHostRTLDeviceId(Request->device_id()), Request->arg_num());
  return Status::OK;
}

Status RemoteOffloadImpl::RunTargetTeamRegion(ServerContext *Context,
                                              const TargetTeamRegion *Request,
                                              Scalar *Reply) {
  SERVER_DBG("Running TargetTeamRegion on Device %d with %d args\n",
             mapHostRTLDeviceId(Request->device_id()), Request->arg_num());

  char **TgtArgs = (char **)malloc(sizeof(char *) * Request->arg_num());
  for (auto I = 0; I < Request->arg_num(); I++) {
    TgtArgs[I] = (char *)malloc(sizeof(char) * 2);
    TgtArgs[I] = (char *)Request->tgt_args()[I];
  }

  ptrdiff_t *TgtOffsets =
      (ptrdiff_t *)malloc(sizeof(ptrdiff_t) * Request->arg_num());
  const auto *TgtOffsetItr = Request->tgt_offsets().begin();
  for (auto I = 0; I < Request->arg_num(); I++, TgtOffsetItr++)
    TgtOffsets[I] = (ptrdiff_t)*TgtOffsetItr;

  void *TgtEntryPtr = ((__tgt_offload_entry *)Request->tgt_entry_ptr())->addr;

  auto Ret = PM->Devices[Request->device_id()].RTL->run_team_region(
      mapHostRTLDeviceId(Request->device_id()), TgtEntryPtr, (void **)TgtArgs,
      TgtOffsets, Request->arg_num(), Request->team_num(),
      Request->thread_limit(), Request->loop_tripcount());

  Reply->set_number(Ret);

  // for (auto I = 0; I < Request->arg_num(); I++)
  //   free(TgtArgs[I]);

  free(TgtArgs);
  free(TgtOffsets);

  SERVER_DBG("Ran TargetTeamRegion on Device %d with %d args\n",
             mapHostRTLDeviceId(Request->device_id()), Request->arg_num());
  return Status::OK;
}

Status RemoteOffloadImpl::RunTargetTeamRegionAsync(
    ServerContext *Context, const TargetTeamRegionAsync *Request,
    Scalar *Reply) {
  SERVER_DBG("Running TargetTeamRegionAsync on Device %d with %d args\n",
             mapHostRTLDeviceId(Request->device_id()), Request->arg_num());

  char **TgtArgs = (char **)malloc(sizeof(char *) * Request->arg_num());
  for (auto I = 0; I < Request->arg_num(); I++) {
    TgtArgs[I] = (char *)malloc(sizeof(char) * 2);
    TgtArgs[I] = (char *)Request->tgt_args()[I];
  }

  ptrdiff_t *TgtOffsets =
      (ptrdiff_t *)malloc(sizeof(ptrdiff_t) * Request->arg_num());
  const auto *TgtOffsetItr = Request->tgt_offsets().begin();
  for (auto I = 0; I < Request->arg_num(); I++, TgtOffsetItr++)
    TgtOffsets[I] = (ptrdiff_t)*TgtOffsetItr;

  void *TgtEntryPtr = ((__tgt_offload_entry *)Request->tgt_entry_ptr())->addr;
  int32_t Ret = PM->Devices[Request->device_id()].RTL->run_team_region(
      mapHostRTLDeviceId(Request->device_id()), TgtEntryPtr, (void **)TgtArgs,
      TgtOffsets, Request->arg_num(), Request->team_num(),
      Request->thread_limit(), Request->loop_tripcount());

  Reply->set_number(Ret);

  // for (auto I = 0; I < Request->arg_num(); I++)
  //   free(TgtArgs[I]);

  free(TgtArgs);
  free(TgtOffsets);

  SERVER_DBG("Ran TargetTeamRegionAsync on Device %d with %d args\n",
             mapHostRTLDeviceId(Request->device_id()), Request->arg_num());
  return Status::OK;
}

///////////////////////////////////////////////////////////////////////////////

int32_t RemoteOffloadImpl::mapHostRTLDeviceId(int32_t RTLDeviceID) {
  for (auto &RTL : PM->RTLs.UsedRTLs) {
    if (RTLDeviceID - RTL->NumberOfDevices >= 0)
      RTLDeviceID -= RTL->NumberOfDevices;
    else
      break;
  }
  return RTLDeviceID;
}
