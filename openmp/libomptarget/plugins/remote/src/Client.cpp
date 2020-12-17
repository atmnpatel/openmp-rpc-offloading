//===----------------- Client.cpp - Client Implementation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// gRPC (Client) for the remote plugin.
//
//===----------------------------------------------------------------------===//

#include <chrono>
#include <cmath>
#include <tuple>

#include "Client.h"
#include "omptarget.h"
#include "openmp.pb.h"

using remoteoffloading::AllocData;
using remoteoffloading::Binary;
using remoteoffloading::Data;
using remoteoffloading::DeleteData;
using remoteoffloading::DevicePair;
using remoteoffloading::ExchangeData;
using remoteoffloading::ExchangeDataAsync;
using remoteoffloading::LongScalar;
using remoteoffloading::Null;
using remoteoffloading::Pointer;
using remoteoffloading::RetrieveData;
using remoteoffloading::RetrieveDataAsync;
using remoteoffloading::Scalar;
using remoteoffloading::SubmitData;
using remoteoffloading::SubmitDataAsync;
using remoteoffloading::SynchronizeDevice;
using remoteoffloading::TargetBinaryDescription;
using remoteoffloading::TargetDeviceImagePtr;
using remoteoffloading::TargetRegion;
using remoteoffloading::TargetRegionAsync;
using remoteoffloading::TargetTable;
using remoteoffloading::TargetTeamRegion;
using remoteoffloading::TargetTeamRegionAsync;

using namespace std::chrono;

using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientWriter;
using grpc::Status;

///////////////////////////////////////////////////////////////////////////////
// Registrations with the Remote.
///////////////////////////////////////////////////////////////////////////////

template <typename Pre, typename Post>
void RemoteOffloadClient::call(Pre Preprocess, Post Postprocess, bool Retry,
                               bool Deadline) {
  if (Arena->SpaceUsed() >= MaxSize)
    Arena->Reset();

  int Attempts = Shots;
  while (Attempts--) {
    ClientContext Context;
    if (Deadline) {
      auto Deadline =
          std::chrono::system_clock::now() + std::chrono::milliseconds(Timeout);
      Context.set_deadline(Deadline);
    }

    Status RPCStatus;
    auto Reply = Preprocess(RPCStatus, Context);

    if (!RPCStatus.ok()) {
      CLIENT_DBG("%s\n", RPCStatus.error_message().c_str());
      if (!Retry)
        break;
      if (Attempts)
        CLIENT_DBG("Trying again\n");
    } else {
      Postprocess(Reply);
      return;
      break;
    }
  }
  CLIENT_DBG("Failed\n");
}

template <typename Pre, typename Post, typename ReturnType>
auto RemoteOffloadClient::call(Pre Preprocess, Post Postprocess,
                               ReturnType Default, bool Retry, bool Deadline) {
  if (Arena->SpaceUsed() >= MaxSize)
    Arena->Reset();

  int Attempts = Shots;
  while (Attempts--) {
    ClientContext Context;
    if (Deadline) {
      auto Deadline =
          std::chrono::system_clock::now() + std::chrono::milliseconds(Timeout);
      Context.set_deadline(Deadline);
    }

    Status RPCStatus;
    auto Reply = Preprocess(RPCStatus, Context);

    if (!RPCStatus.ok()) {
      CLIENT_DBG("%s\n", RPCStatus.error_message().c_str());
      if (!Retry)
        break;
      if (Attempts)
        CLIENT_DBG("Trying again\n");
    } else {
      return Postprocess(Reply);
      break;
    }
  }
  CLIENT_DBG("Failed\n");
  return Default;
}

void RemoteOffloadClient::shutdown(void) {
  ClientContext Context;
  Null Request, Reply;
  CLIENT_DBG("Shutting down server.\n");
  auto Status = Stub->Shutdown(&Context, Request, &Reply);
}

void RemoteOffloadClient::registerLib(__tgt_bin_desc *Desc) {
  call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Request = protobuf::Arena::CreateMessage<TargetBinaryDescription>(
            Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<Null>(Arena.get());
        loadTargetBinaryDescription(Desc, *Request);

        CLIENT_RDBG("Registering Library");
        RPCStatus = Stub->RegisterLib(&Context, *Request, Reply);
        return Reply;
      },
      [&](const auto &Reply) { CLIENT_RDBG("Registered Library\n"); });
}

void RemoteOffloadClient::unregisterLib(__tgt_bin_desc *Desc) {
  call(
      [&](auto &RPCStatus, auto &Context) {
        // We do not pass the argument because only one library is registered.
        // FIXME: Can we simplify the API?
        auto *Request = protobuf::Arena::CreateMessage<Null>(Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<Null>(Arena.get());
        CLIENT_RDBG("Unregistering Library");
        RPCStatus = Stub->UnregisterLib(&Context, *Request, Reply);
        return Reply;
      },
      [&](const auto &Reply) {
        CLIENT_RDBG("Unregistered Library\n");
        if (Table) {
          free(Table->EntriesBegin);
          free(Table);
        }
      },
      false);
}

void RemoteOffloadClient::registerRequires(int64_t Flags) {
  call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Request = protobuf::Arena::CreateMessage<LongScalar>(Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<Null>(Arena.get());

        Request->set_number(Flags);

        CLIENT_RDBG("Registering Requires");
        RPCStatus = Stub->RegisterRequires(&Context, *Request, Reply);
        return Reply;
      },
      [&](const auto &Reply) { CLIENT_RDBG("Registered Requires\n"); });
}

///////////////////////////////////////////////////////////////////////////////
// Target Runtime Library Functions.
///////////////////////////////////////////////////////////////////////////////

int32_t RemoteOffloadClient::isValidBinary(__tgt_device_image *Image) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Request =
            protobuf::Arena::CreateMessage<TargetDeviceImagePtr>(Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());

        Request->set_image_ptr((uint64_t)Image->ImageStart);

        auto *EntryItr = Image->EntriesBegin;
        while (EntryItr != Image->EntriesEnd)
          Request->add_entry_ptrs((uint64_t)EntryItr++);

        CLIENT_RDBG("Validating binary");
        RPCStatus = Stub->IsValidBinary(&Context, *Request, Reply);
        return Reply;
      },
      [&](const auto &Reply) {
        if (Reply->number()) {
          CLIENT_RDBG("Validated binary\n");
        } else {
          CLIENT_RDBG("Could not validate binary\n");
        }
        return Reply->number();
      },
      0);
}

int32_t RemoteOffloadClient::getNumberOfDevices() {
  return call(
      [&](Status &RPCStatus, ClientContext &Context) {
        auto *Request = protobuf::Arena::CreateMessage<Null>(Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());

        CLIENT_RDBG("Getting number of devices");
        RPCStatus = Stub->GetNumberOfDevices(&Context, *Request, Reply);

        return Reply;
      },
      [&](const auto &Reply) {
        if (Reply->number()) {
          CLIENT_RDBG("Found %d devices\n", Reply->number());
        } else {
          CLIENT_RDBG("Could not get the number of devices\n");
        }
        return Reply->number();
      },
      0);
}

///////////////////////////////////////////////////////////////////////////////

int32_t RemoteOffloadClient::initDevice(int32_t DeviceId) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Request = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());

        Request->set_number(DeviceId);

        CLIENT_RDBG("Initializing device %d", DeviceId);
        RPCStatus = Stub->InitDevice(&Context, *Request, Reply);

        return Reply;
      },
      [&](const auto &Reply) {
        if (!Reply->number()) {
          CLIENT_RDBG("Initialized device %d\n", DeviceId);
        } else {
          CLIENT_RDBG("Could not initialize device %d\n", DeviceId);
        }
        return Reply->number();
      },
      -1);
}

int32_t RemoteOffloadClient::initRequires(int64_t RequiresFlags) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Request = protobuf::Arena::CreateMessage<LongScalar>(Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        Request->set_number(RequiresFlags);
        CLIENT_RDBG("Initializing requires");
        RPCStatus = Stub->InitRequires(&Context, *Request, Reply);
        return Reply;
      },
      [&](const auto &Reply) {
        if (Reply->number()) {
          CLIENT_RDBG("Initialized requires\n");
        } else {
          CLIENT_RDBG("Could not initialize requires\n");
        }
        return Reply->number();
      },
      -1);
}

///////////////////////////////////////////////////////////////////////////////

__tgt_target_table *RemoteOffloadClient::loadBinary(int32_t DeviceId,
                                                    __tgt_device_image *Image) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *ImageMessage =
            protobuf::Arena::CreateMessage<Binary>(Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<TargetTable>(Arena.get());
        ImageMessage->set_image_ptr((uint64_t)Image->ImageStart);
        ImageMessage->set_device_id(DeviceId);

        CLIENT_RDBG("Loading Image %p to device %d", Image, DeviceId);
        RPCStatus = Stub->LoadBinary(&Context, *ImageMessage, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (Reply->entries_size() == 0) {
          CLIENT_RDBG("Could not load image %p onto device %d", Image, DeviceId);
          return (__tgt_target_table *)nullptr;
        }
        Table = (__tgt_target_table *)malloc(sizeof(__tgt_target_table));
        unloadTargetTable(*Reply, Table, HostToRemoteTargetTableMap);

        CLIENT_RDBG("Loaded Image %p to device %d with %d entries\n", Image,
                   DeviceId, Reply->entries_size());

        return Table;
      },
      (__tgt_target_table *)nullptr, true, false);
}

int64_t RemoteOffloadClient::synchronize(int32_t DeviceId,
                                         __tgt_async_info *AsyncInfoPtr) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Info =
            protobuf::Arena::CreateMessage<SynchronizeDevice>(Arena.get());

        Info->set_device_id(DeviceId);
        Info->set_queue_ptr((uint64_t)AsyncInfoPtr);

        CLIENT_RDBG("Synchronizing device %d", DeviceId);
        RPCStatus = Stub->Synchronize(&Context, *Info, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (Reply->number()) {
          CLIENT_RDBG("Synchronized device %d\n", DeviceId);
        } else {
          CLIENT_RDBG("Could not synchronize device %d\n", DeviceId);
        }
        return Reply->number();
      },
      -1);
}

int32_t RemoteOffloadClient::isDataExchangeable(int32_t SrcDevId,
                                                int32_t DstDevId) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Request = protobuf::Arena::CreateMessage<DevicePair>(Arena.get());
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());

        Request->set_src_dev_id(SrcDevId);
        Request->set_dst_dev_id(DstDevId);

        CLIENT_RDBG("Asking if Data is exchangeable between %d, %d", SrcDevId,
                   DstDevId);
        RPCStatus = Stub->IsDataExchangeable(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (Reply->number()) {
          CLIENT_RDBG("Data is exchangeable between %d, %d\n", SrcDevId,
                     DstDevId);
        } else {
          CLIENT_RDBG("Data is not exchangeable between %d, %d\n", SrcDevId,
                     DstDevId);
        }
        return Reply->number();
      },
      -1);
}

///////////////////////////////////////////////////////////////////////////////

void *RemoteOffloadClient::dataAlloc(int32_t DeviceId, int64_t Size,
                                     void *HstPtr) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Pointer>(Arena.get());
        auto *Request = protobuf::Arena::CreateMessage<AllocData>(Arena.get());

        Request->set_device_id(DeviceId);
        Request->set_size(Size);
        Request->set_hst_ptr((uint64_t)HstPtr);

        CLIENT_RDBG("Allocating %ld bytes on device %d", Size, DeviceId);
        RPCStatus = Stub->DataAlloc(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (Reply->number()) {
          CLIENT_RDBG("Allocated %ld bytes on device %d at %p\n", Size, DeviceId,
                     (void *)Reply->number());
        } else {
          CLIENT_RDBG("Could not allocate %ld bytes on device %d at %p\n", Size,
                     DeviceId, (void *)Reply->number());
        }
        return (void *)Reply->number();
      },
      (void *)nullptr);
}

int32_t RemoteOffloadClient::dataSubmit(int32_t DeviceId, void *TgtPtr,
                                        void *HstPtr, int64_t Size) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        std::unique_ptr<ClientWriter<SubmitData>> Writer(
            Stub->DataSubmit(&Context, Reply));

        if (Size > BlockSize) {
          int64_t Start = 0, End = BlockSize;
          for (auto I = 0; I < ceil((float)Size / BlockSize); I++) {
            auto *Request =
                protobuf::Arena::CreateMessage<SubmitData>(Arena.get());

            Request->set_device_id(DeviceId);
            Request->set_data((char *)HstPtr + Start, End - Start);
            Request->set_hst_ptr((uint64_t)HstPtr);
            Request->set_tgt_ptr((uint64_t)TgtPtr);
            Request->set_start(Start);
            Request->set_size(Size);

            CLIENT_RDBG("Submitting %ld-%ld/%ld bytes on device %d at %p",
                        Start, End, Size, DeviceId, TgtPtr)

            if (!Writer->Write(*Request)) {
              CLIENT_RDBG("Broken stream when submitting data\n");
              Reply->set_number(0);
              return Reply;
            }

            Start += BlockSize;
            End += BlockSize;
            if (End >= Size)
              End = Size;
          }
        } else {
          auto *Request =
              protobuf::Arena::CreateMessage<SubmitData>(Arena.get());

          Request->set_device_id(DeviceId);
          Request->set_data(HstPtr, Size);
          Request->set_hst_ptr((uint64_t)HstPtr);
          Request->set_tgt_ptr((uint64_t)TgtPtr);
          Request->set_start(0);
          Request->set_size(Size);

          CLIENT_RDBG("Submitting %ld bytes on device %d at %p", Size,
                     DeviceId, TgtPtr)
          if (!Writer->Write(*Request)) {
            CLIENT_RDBG("Broken stream when submitting data\n");
            Reply->set_number(0);
            return Reply;
          }
        }

        Writer->WritesDone();
        RPCStatus = Writer->Finish();

        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->number()) {
          CLIENT_RDBG("Submitted %ld bytes on device %d at %p\n", Size,
                      DeviceId, TgtPtr)
        } else {
          CLIENT_RDBG("Could not submit %ld bytes on device %d at %p\n", Size,
                     DeviceId, TgtPtr)
        }
        return Reply->number();
      },
      -1, true, false);
}

int32_t RemoteOffloadClient::dataSubmitAsync(int32_t DeviceId, void *TgtPtr,
                                             void *HstPtr, int64_t Size,
                                             __tgt_async_info *AsyncInfoPtr) {

  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        std::unique_ptr<ClientWriter<SubmitDataAsync>> Writer(
            Stub->DataSubmitAsync(&Context, Reply));

        if (Size > BlockSize) {
          int64_t Start = 0, End = BlockSize;
          for (auto I = 0; I < ceil((float)Size / BlockSize); I++) {
            auto *Request =
                protobuf::Arena::CreateMessage<SubmitDataAsync>(Arena.get());

            Request->set_device_id(DeviceId);
            Request->set_data((char *)HstPtr + Start, End - Start);
            Request->set_hst_ptr((uint64_t)HstPtr);
            Request->set_tgt_ptr((uint64_t)TgtPtr);
            Request->set_start(Start);
            Request->set_size(Size);
            Request->set_queue_ptr((uint64_t)AsyncInfoPtr);

            CLIENT_RDBG("Submitting %ld-%ld/%ld bytes async on device %d at %p",
                        Start, End, Size, DeviceId, TgtPtr)

            if (!Writer->Write(*Request)) {
              CLIENT_RDBG("Broken stream when submitting data");
              Reply->set_number(0);
              return Reply;
            }

            Start += BlockSize;
            End += BlockSize;
            if (End >= Size)
              End = Size;
          }
        } else {
          auto *Request =
              protobuf::Arena::CreateMessage<SubmitDataAsync>(Arena.get());

          Request->set_device_id(DeviceId);
          Request->set_data(HstPtr, Size);
          Request->set_hst_ptr((uint64_t)HstPtr);
          Request->set_tgt_ptr((uint64_t)TgtPtr);
          Request->set_start(0);
          Request->set_size(Size);

          CLIENT_RDBG("Submitting %ld bytes async on device %d at %p", Size,
                     DeviceId, TgtPtr)
          if (!Writer->Write(*Request)) {
            CLIENT_RDBG("Broken stream when submitting data");
            Reply->set_number(0);
            return Reply;
          }
        }

        Writer->WritesDone();
        RPCStatus = Writer->Finish();

        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->number()) {
          CLIENT_RDBG("Async Submitted %ld bytes on device %d at %p\n", Size,
                      DeviceId, TgtPtr)
        } else {
          CLIENT_RDBG("Could not async submit %ld bytes on device %d at %p\n",
                     Size, DeviceId, TgtPtr)
        }
        return Reply->number();
      },
      -1, true, false);
}

int32_t RemoteOffloadClient::dataRetrieve(int32_t DeviceId, void *HstPtr,
                                          void *TgtPtr, int64_t Size) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Request =
            protobuf::Arena::CreateMessage<RetrieveData>(Arena.get());

        Request->set_device_id(DeviceId);
        Request->set_size(Size);
        Request->set_hst_ptr((int64_t)HstPtr);
        Request->set_tgt_ptr((int64_t)TgtPtr);

        auto *Reply = protobuf::Arena::CreateMessage<Data>(Arena.get());
        std::unique_ptr<ClientReader<Data>> Reader(
            Stub->DataRetrieve(&Context, *Request));
        Reader->WaitForInitialMetadata();
        while (Reader->Read(Reply)) {
          if (Reply->ret()) {
            return Reply;
          }

          if (Reply->start() == 0 && Reply->size() == Reply->data().size()) {
            CLIENT_RDBG("Retrieving %ld bytes on device %d at %p for %p", Size,
                        DeviceId, TgtPtr, HstPtr)

            memcpy(HstPtr, Reply->data().data(), Reply->data().size());

            return Reply;
          } else {
            CLIENT_RDBG(
                "Retrieving %lu-%lu/%lu bytes from (%p) to (%p) on Device %d",
                Reply->start(), Reply->start() + Reply->data().size(),
                Reply->size(), (void *)Request->tgt_ptr(), HstPtr,
                Request->device_id());

            memcpy((void *)((char *)HstPtr + Reply->start()),
                   Reply->data().data(), Reply->data().size());
          }
        }
        RPCStatus = Reader->Finish();

        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->ret()) {
          CLIENT_RDBG("Retrieved %ld bytes on Device %d\n", Size, DeviceId);
        } else {
          CLIENT_RDBG("Could not retrieve %ld bytes on Device %d\n", Size,
                     DeviceId);
        }
        return Reply->ret();
      },
      -1, true, false);
}

int32_t RemoteOffloadClient::dataRetrieveAsync(int32_t DeviceId, void *HstPtr,
                                               void *TgtPtr, int64_t Size,
                                               __tgt_async_info *AsyncInfoPtr) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Request =
            protobuf::Arena::CreateMessage<RetrieveDataAsync>(Arena.get());

        Request->set_device_id(DeviceId);
        Request->set_size(Size);
        Request->set_hst_ptr((int64_t)HstPtr);
        Request->set_tgt_ptr((int64_t)TgtPtr);
        Request->set_queue_ptr((uint64_t)AsyncInfoPtr);

        auto *Reply = protobuf::Arena::CreateMessage<Data>(Arena.get());
        std::unique_ptr<ClientReader<Data>> Reader(
            Stub->DataRetrieveAsync(&Context, *Request));
        Reader->WaitForInitialMetadata();
        while (Reader->Read(Reply)) {
          if (Reply->ret()) {
            CLIENT_RDBG(
                "Could not async retrieve %ld bytes on device %d at %p "
                "for %p",
                Size, DeviceId, TgtPtr, HstPtr)
            return Reply;
          }

          if (Reply->start() == 0 && Reply->size() == Reply->data().size()) {
            CLIENT_RDBG("Async Retrieving %ld bytes on device %d at %p for %p",
                       Size, DeviceId, TgtPtr, HstPtr)

            memcpy(HstPtr, Reply->data().data(), Reply->data().size());

            return Reply;
          } else {
            CLIENT_RDBG("Retrieving %lu-%lu/%lu bytes async from (%p) to (%p) "
                        "on Device %d",
                        Reply->start(), Reply->start() + Reply->data().size(),
                        Reply->size(), (void *)Request->tgt_ptr(), HstPtr,
                        Request->device_id());

            memcpy((void *)((char *)HstPtr + Reply->start()),
                   Reply->data().data(), Reply->data().size());
          }
        }
        RPCStatus = Reader->Finish();

        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->ret()) {
          CLIENT_RDBG("Async retrieve %ld bytes on Device %d\n", Size, DeviceId);
        } else {
          CLIENT_RDBG("Could not async retrieve %ld bytes on Device %d\n",
                     Size, DeviceId);
        }
        return Reply->ret();
      },
      -1, true, false);
}

int32_t RemoteOffloadClient::dataExchange(int32_t SrcDevId, void *SrcPtr,
                                          int32_t DstDevId, void *DstPtr,
                                          int64_t Size) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Request =
            protobuf::Arena::CreateMessage<ExchangeData>(Arena.get());

        Request->set_src_dev_id(SrcDevId);
        Request->set_src_ptr((uint64_t)SrcPtr);
        Request->set_dst_dev_id(DstDevId);
        Request->set_dst_ptr((uint64_t)DstPtr);
        Request->set_size(Size);

        CLIENT_RDBG(
            "Exchanging %ld bytes on device %d at %p for %p on device %d",
            Size, SrcDevId, SrcPtr, DstPtr, DstDevId)
        RPCStatus = Stub->DataExchange(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (Reply->number()) {
          CLIENT_RDBG(
              "Exchanged %ld bytes on device %d at %p for %p on device %d\n",
              Size, SrcDevId, SrcPtr, DstPtr, DstDevId)
        } else {
          CLIENT_RDBG("Could not exchange %ld bytes on device %d at %p for %p "
                     "on device %d\n",
                     Size, SrcDevId, SrcPtr, DstPtr, DstDevId)
        }
        return Reply->number();
      },
      -1);
}

int32_t RemoteOffloadClient::dataExchangeAsync(int32_t SrcDevId, void *SrcPtr,
                                               int32_t DstDevId, void *DstPtr,
                                               int64_t Size,
                                               __tgt_async_info *AsyncInfoPtr) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Request =
            protobuf::Arena::CreateMessage<ExchangeDataAsync>(Arena.get());

        Request->set_src_dev_id(SrcDevId);
        Request->set_src_ptr((uint64_t)SrcPtr);
        Request->set_dst_dev_id(DstDevId);
        Request->set_dst_ptr((uint64_t)DstPtr);
        Request->set_size(Size);
        Request->set_queue_ptr((uint64_t)AsyncInfoPtr);

        CLIENT_RDBG(
            "Exchanging %ld bytes on device %d at %p for %p on device %d",
            Size, SrcDevId, SrcPtr, DstPtr, DstDevId);
        RPCStatus = Stub->DataExchangeAsync(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (Reply->number()) {
          CLIENT_RDBG(
              "Exchanged %ld bytes on device %d at %p for %p on device %d\n",
              Size, SrcDevId, SrcPtr, DstPtr, DstDevId);
        } else {
          CLIENT_RDBG("Could not exchange %ld bytes on device %d at %p for %p "
                     "on device %d\n",
                     Size, SrcDevId, SrcPtr, DstPtr, DstDevId);
        }
        return Reply->number();
      },
      -1);
}

int32_t RemoteOffloadClient::dataDelete(int32_t DeviceId, void *TgtPtr) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Request = protobuf::Arena::CreateMessage<DeleteData>(Arena.get());

        Request->set_device_id(DeviceId);
        Request->set_tgt_ptr((uint64_t)TgtPtr);

        CLIENT_RDBG("Deleting data at %p on device %d", TgtPtr, DeviceId)
        RPCStatus = Stub->DataDelete(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->number()) {
          CLIENT_RDBG("Deleted data at %p on device %d\n", TgtPtr, DeviceId)
        } else {
          CLIENT_RDBG("Could not deleted data at %p on device %d\n", TgtPtr,
                     DeviceId)
        }
        return Reply->number();
      },
      -1);
}

///////////////////////////////////////////////////////////////////////////////

int32_t RemoteOffloadClient::runTargetRegion(int32_t DeviceId,
                                             void *TgtEntryPtr, void **TgtArgs,
                                             ptrdiff_t *TgtOffsets,
                                             int32_t ArgNum) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Request =
            protobuf::Arena::CreateMessage<TargetRegion>(Arena.get());

        Request->set_device_id(DeviceId);

        assert(HostToRemoteTargetTableMap.count(TgtEntryPtr) == 1);
        Request->set_tgt_entry_ptr(
            (uint64_t)HostToRemoteTargetTableMap[TgtEntryPtr]);

        char **ArgPtr = (char **)TgtArgs;
        for (auto I = 0; I < ArgNum; I++, ArgPtr++)
          Request->add_tgt_args((uint64_t)*ArgPtr);

        char *OffsetPtr = (char *)TgtOffsets;
        for (auto I = 0; I < ArgNum; I++, OffsetPtr++)
          Request->add_tgt_offsets((uint64_t)*OffsetPtr);

        Request->set_arg_num(ArgNum);

        CLIENT_RDBG("Running target region on device %d", DeviceId);
        RPCStatus = Stub->RunTargetRegion(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->number()) {
          CLIENT_RDBG("Ran target region on device %d\n", DeviceId);
        } else {
          CLIENT_RDBG("Could not run target region on device %d\n", DeviceId);
        }
        return Reply->number();
      },
      -1, false, false);
}

int32_t RemoteOffloadClient::runTargetRegionAsync(
    int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs, ptrdiff_t *TgtOffsets,
    int32_t ArgNum, __tgt_async_info *AsyncInfoPtr) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Request =
            protobuf::Arena::CreateMessage<TargetRegionAsync>(Arena.get());

        Request->set_device_id(DeviceId);
        Request->set_queue_ptr((uint64_t)AsyncInfoPtr);

        assert(HostToRemoteTargetTableMap.count(TgtEntryPtr) == 1);
        Request->set_tgt_entry_ptr(
            (uint64_t)HostToRemoteTargetTableMap[TgtEntryPtr]);

        char **ArgPtr = (char **)TgtArgs;
        for (auto I = 0; I < ArgNum; I++, ArgPtr++)
          Request->add_tgt_args((uint64_t)*ArgPtr);

        char *OffsetPtr = (char *)TgtOffsets;
        for (auto I = 0; I < ArgNum; I++, OffsetPtr++)
          Request->add_tgt_offsets((uint64_t)*OffsetPtr);

        Request->set_arg_num(ArgNum);

        CLIENT_RDBG("Running target region async on device %d", DeviceId);
        RPCStatus = Stub->RunTargetRegionAsync(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->number()) {
          CLIENT_RDBG("Ran target region async on device %d\n", DeviceId);
        } else {
          CLIENT_RDBG("Could not run target region async on device %d\n",
                     DeviceId);
        }
        return Reply->number();
      },
      -1, false, false);
}

int32_t RemoteOffloadClient::runTargetTeamRegion(
    int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs, ptrdiff_t *TgtOffsets,
    int32_t ArgNum, int32_t TeamNum, int32_t ThreadLimit,
    uint64_t LoopTripcount) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Request =
            protobuf::Arena::CreateMessage<TargetTeamRegion>(Arena.get());

        Request->set_device_id(DeviceId);

        assert(HostToRemoteTargetTableMap.count(TgtEntryPtr) == 1);
        Request->set_tgt_entry_ptr(
            (uint64_t)HostToRemoteTargetTableMap[TgtEntryPtr]);

        char **ArgPtr = (char **)TgtArgs;
        for (auto I = 0; I < ArgNum; I++, ArgPtr++)
          Request->add_tgt_args((uint64_t)*ArgPtr);

        char *OffsetPtr = (char *)TgtOffsets;
        for (auto I = 0; I < ArgNum; I++, OffsetPtr++)
          Request->add_tgt_offsets((uint64_t)*OffsetPtr);

        Request->set_arg_num(ArgNum);
        Request->set_team_num(TeamNum);
        Request->set_thread_limit(ThreadLimit);
        Request->set_loop_tripcount(LoopTripcount);

        CLIENT_RDBG("Running target team region on device %d", DeviceId);
        RPCStatus = Stub->RunTargetTeamRegion(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->number()) {
          CLIENT_RDBG("Ran target team region on device %d\n", DeviceId);
        } else {
          CLIENT_RDBG("Could not run target team region on device %d\n",
                     DeviceId);
        }
        return Reply->number();
      },
      -1, false, false);
}

int32_t RemoteOffloadClient::runTargetTeamRegionAsync(
    int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs, ptrdiff_t *TgtOffsets,
    int32_t ArgNum, int32_t TeamNum, int32_t ThreadLimit,
    uint64_t LoopTripcount, __tgt_async_info *AsyncInfoPtr) {
  return call(
      [&](auto &RPCStatus, auto &Context) {
        auto *Reply = protobuf::Arena::CreateMessage<Scalar>(Arena.get());
        auto *Request =
            protobuf::Arena::CreateMessage<TargetTeamRegionAsync>(Arena.get());

        Request->set_device_id(DeviceId);
        Request->set_queue_ptr((uint64_t)AsyncInfoPtr);

        assert(HostToRemoteTargetTableMap.count(TgtEntryPtr) == 1);
        Request->set_tgt_entry_ptr(
            (uint64_t)HostToRemoteTargetTableMap[TgtEntryPtr]);

        char **ArgPtr = (char **)TgtArgs;
        for (auto I = 0; I < ArgNum; I++, ArgPtr++) {
          Request->add_tgt_args((uint64_t)*ArgPtr);
        }

        char *OffsetPtr = (char *)TgtOffsets;
        for (auto I = 0; I < ArgNum; I++, OffsetPtr++)
          Request->add_tgt_offsets((uint64_t)*OffsetPtr);

        Request->set_arg_num(ArgNum);
        Request->set_team_num(TeamNum);
        Request->set_thread_limit(ThreadLimit);
        Request->set_loop_tripcount(LoopTripcount);

        CLIENT_RDBG("Running target team region async on device %d", DeviceId);
        RPCStatus = Stub->RunTargetTeamRegionAsync(&Context, *Request, Reply);
        return Reply;
      },
      [&](auto &Reply) {
        if (!Reply->number()) {
          CLIENT_RDBG("Ran target team region async on device %d\n", DeviceId);
        } else {
          CLIENT_RDBG("Could not run target team region async on device %d\n",
                     DeviceId);
        }
        return Reply->number();
      },
      -1, false, false);
}

/////////////////////////////////////////////////////////////////////////////

void RemoteClientManager::shutdown(void) {
  for (auto &Client : Clients)
    Client.shutdown();
}

void RemoteClientManager::registerLib(__tgt_bin_desc *Desc) {
  for (auto &Client : Clients)
    Client.registerLib(Desc);
}

void RemoteClientManager::unregisterLib(__tgt_bin_desc *Desc) {
  for (auto &Client : Clients)
    Client.unregisterLib(Desc);
}

void RemoteClientManager::registerRequires(int64_t Flags) {
  for (auto &Client : Clients)
    Client.registerRequires(Flags);
}

int32_t RemoteClientManager::isValidBinary(__tgt_device_image *Image) {
  int32_t ClientIdx = 0;
  for (auto &Client : Clients) {
    if (auto Ret = Client.isValidBinary(Image))
      return Ret;
    ClientIdx++;
  }
  return 0;
}

int32_t RemoteClientManager::getNumberOfDevices() {
  auto ClientIdx = 0;
  for (auto &Client : Clients) {
    if (auto NumDevices = Client.getNumberOfDevices()) {
      Devices.push_back(NumDevices);
    }
    ClientIdx++;
  }

  return std::accumulate(Devices.begin(), Devices.end(), 0);
}

std::pair<int32_t, int32_t> RemoteClientManager::mapDeviceId(int32_t DeviceId) {
  for (size_t ClientIdx = 0; ClientIdx < Devices.size(); ClientIdx++) {
    if (!(DeviceId >= Devices[ClientIdx]))
      return {ClientIdx, DeviceId};
    DeviceId -= Devices[ClientIdx];
  }
  return {-1, -1};
}

/////////////////////////////////////////////////////////////////////////////

int32_t RemoteClientManager::initDevice(int32_t DeviceId) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].initDevice(DeviceIdx);
}

int32_t RemoteClientManager::initRequires(int64_t RequiresFlags) {
  for (auto &Client : Clients)
    Client.initRequires(RequiresFlags);

  return RequiresFlags;
}

/////////////////////////////////////////////////////////////////////////////

__tgt_target_table *RemoteClientManager::loadBinary(int32_t DeviceId,
                                                    __tgt_device_image *Image) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].loadBinary(DeviceIdx, Image);
}

int64_t RemoteClientManager::synchronize(int32_t DeviceId,
                                         __tgt_async_info *AsyncInfoPtr) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].synchronize(DeviceIdx, AsyncInfoPtr);
}

int32_t RemoteClientManager::isDataExchangeable(int32_t SrcDevId,
                                                int32_t DstDevId) {
  int32_t SrcClientIdx, SrcDeviceIdx, DstClientIdx, DstDeviceIdx;
  std::tie(SrcClientIdx, SrcDeviceIdx) = mapDeviceId(SrcDevId);
  std::tie(DstClientIdx, DstDeviceIdx) = mapDeviceId(DstDevId);
  return Clients[SrcClientIdx].isDataExchangeable(SrcDeviceIdx, DstDeviceIdx);
}

/////////////////////////////////////////////////////////////////////////////

void *RemoteClientManager::dataAlloc(int32_t DeviceId, int64_t Size,
                                     void *HstPtr) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].dataAlloc(DeviceIdx, Size, HstPtr);
}

int32_t RemoteClientManager::dataSubmit(int32_t DeviceId, void *TgtPtr,
                                        void *HstPtr, int64_t Size) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].dataSubmit(DeviceIdx, TgtPtr, HstPtr, Size);
}

int32_t RemoteClientManager::dataRetrieve(int32_t DeviceId, void *HstPtr,
                                          void *TgtPtr, int64_t Size) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].dataRetrieve(DeviceIdx, HstPtr, TgtPtr, Size);
}

int32_t RemoteClientManager::dataDelete(int32_t DeviceId, void *TgtPtr) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].dataDelete(DeviceIdx, TgtPtr);
}

int32_t RemoteClientManager::dataSubmitAsync(int32_t DeviceId, void *TgtPtr,
                                             void *HstPtr, int64_t Size,
                                             __tgt_async_info *AsyncInfoPtr) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].dataSubmitAsync(DeviceIdx, TgtPtr, HstPtr, Size,
                                            AsyncInfoPtr);
}

int32_t RemoteClientManager::dataRetrieveAsync(int32_t DeviceId, void *HstPtr,
                                               void *TgtPtr, int64_t Size,
                                               __tgt_async_info *AsyncInfoPtr) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].dataRetrieveAsync(DeviceIdx, HstPtr, TgtPtr, Size,
                                              AsyncInfoPtr);
}

int32_t RemoteClientManager::dataExchange(int32_t SrcDevId, void *SrcPtr,
                                          int32_t DstDevId, void *DstPtr,
                                          int64_t Size) {
  int32_t SrcClientIdx, SrcDeviceIdx, DstClientIdx, DstDeviceIdx;
  std::tie(SrcClientIdx, SrcDeviceIdx) = mapDeviceId(SrcDevId);
  std::tie(DstClientIdx, DstDeviceIdx) = mapDeviceId(DstDevId);
  return Clients[SrcClientIdx].dataExchange(SrcDeviceIdx, SrcPtr, DstDeviceIdx,
                                            DstPtr, Size);
}
int32_t RemoteClientManager::dataExchangeAsync(int32_t SrcDevId, void *SrcPtr,
                                               int32_t DstDevId, void *DstPtr,
                                               int64_t Size,
                                               __tgt_async_info *AsyncInfoPtr) {
  int32_t SrcClientIdx, SrcDeviceIdx, DstClientIdx, DstDeviceIdx;
  std::tie(SrcClientIdx, SrcDeviceIdx) = mapDeviceId(SrcDevId);
  std::tie(DstClientIdx, DstDeviceIdx) = mapDeviceId(DstDevId);
  return Clients[SrcClientIdx].dataExchangeAsync(
      SrcDeviceIdx, SrcPtr, DstDeviceIdx, DstPtr, Size, AsyncInfoPtr);
}

///////////////////////////////////////////////////////////////////////////////

int32_t RemoteClientManager::runTargetRegion(int32_t DeviceId,
                                             void *TgtEntryPtr, void **TgtArgs,
                                             ptrdiff_t *TgtOffsets,
                                             int32_t ArgNum) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].runTargetRegion(DeviceIdx, TgtEntryPtr, TgtArgs,
                                            TgtOffsets, ArgNum);
}

int32_t RemoteClientManager::runTargetRegionAsync(
    int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs, ptrdiff_t *TgtOffsets,
    int32_t ArgNum, __tgt_async_info *AsyncInfoPtr) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].runTargetRegionAsync(
      DeviceIdx, TgtEntryPtr, TgtArgs, TgtOffsets, ArgNum, AsyncInfoPtr);
}

int32_t RemoteClientManager::runTargetTeamRegion(
    int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs, ptrdiff_t *TgtOffsets,
    int32_t ArgNum, int32_t TeamNum, int32_t ThreadLimit,
    uint64_t LoopTripCount) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].runTargetTeamRegion(DeviceIdx, TgtEntryPtr, TgtArgs,
                                                TgtOffsets, ArgNum, TeamNum,
                                                ThreadLimit, LoopTripCount);
}

int32_t RemoteClientManager::runTargetTeamRegionAsync(
    int32_t DeviceId, void *TgtEntryPtr, void **TgtArgs, ptrdiff_t *TgtOffsets,
    int32_t ArgNum, int32_t TeamNum, int32_t ThreadLimit,
    uint64_t LoopTripCount, __tgt_async_info *AsyncInfoPtr) {
  int32_t ClientIdx, DeviceIdx;
  std::tie(ClientIdx, DeviceIdx) = mapDeviceId(DeviceId);
  return Clients[ClientIdx].runTargetTeamRegionAsync(
      DeviceIdx, TgtEntryPtr, TgtArgs, TgtOffsets, ArgNum, TeamNum, ThreadLimit,
      LoopTripCount, AsyncInfoPtr);
}
