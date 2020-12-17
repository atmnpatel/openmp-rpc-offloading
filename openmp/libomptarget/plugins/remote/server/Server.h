//===-------------------------- Server.h - Server -------------------------===//
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

#include <grpcpp/server_context.h>
#include <map>

#include "Utils.h"
#include "device.h"
#include "omptarget.h"
#include "openmp.grpc.pb.h"
#include "openmp.pb.h"
#include "rtl.h"

using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerWriter;
using grpc::Status;

using remoteoffloading::RemoteOffload;

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

using namespace google;

extern PluginManager *PM;

class RemoteOffloadImpl final : public RemoteOffload::Service {
private:
  int32_t mapHostRTLDeviceId(int32_t RTLDeviceID);

  std::map<const void *, __tgt_device_image *> HostToRemoteDeviceImage;
  std::map<const void *, __tgt_offload_entry *> HostToRemoteOffloadEntry;
  __tgt_bin_desc *Desc = nullptr;
  __tgt_target_table *Table = nullptr;

  int DebugLevel;
  int BlockSize;
  int MaxSize;
  std::unique_ptr<protobuf::Arena> Arena;

public:
  RemoteOffloadImpl(int MaxSize, int BlockSize): MaxSize(MaxSize), BlockSize(BlockSize) {
    DebugLevel = getDebugLevel();
    Arena = std::make_unique<protobuf::Arena>();
  }

  Status Shutdown(ServerContext *Context,
                     const Null *Request,
                     Null *Reply) override;

  /////////////////////////////////////////////////////////////////////////////
  // Remote Registrations.
  /////////////////////////////////////////////////////////////////////////////

  Status RegisterLib(ServerContext *Context,
                     const TargetBinaryDescription *Description,
                     Null *Reply) override;
  Status UnregisterLib(ServerContext *Context, const Null *Request,
                       Null *Reply) override;
  Status RegisterRequires(ServerContext *Context, const LongScalar *Request,
                          Null *Reply) override;

  /////////////////////////////////////////////////////////////////////////////

  Status IsValidBinary(ServerContext *Context,
                       const TargetDeviceImagePtr *Image,
                       Scalar *IsValid) override;
  Status GetNumberOfDevices(ServerContext *Context, const Null *Null,
                            Scalar *NumberOfDevices) override;

  /////////////////////////////////////////////////////////////////////////////

  Status InitDevice(ServerContext *Context, const Scalar *DeviceNum,
                    Scalar *Reply) override;
  Status InitRequires(ServerContext *Context, const LongScalar *RequiresFlag,
                      Scalar *Reply) override;

  /////////////////////////////////////////////////////////////////////////////

  Status LoadBinary(ServerContext *Context, const Binary *Binary,
                    TargetTable *Reply) override;
  Status Synchronize(ServerContext *Context, const SynchronizeDevice *Info,
                     Scalar *Reply) override;
  Status IsDataExchangeable(ServerContext *Context, const DevicePair *Request,
                            Scalar *Reply) override;

  /////////////////////////////////////////////////////////////////////////////

  Status DataAlloc(ServerContext *Context, const AllocData *Request,
                   Pointer *Reply) override;

  Status DataSubmit(ServerContext *Context, ServerReader<SubmitData> *Reader,
                    Scalar *Reply) override;
  Status DataSubmitAsync(ServerContext *Context, ServerReader<SubmitDataAsync> *Reader,
                         Scalar *Reply) override;

  Status DataRetrieve(ServerContext *Context, const RetrieveData *Request,
                      ServerWriter<Data> *Writer) override;
  Status DataRetrieveAsync(ServerContext *Context,
                           const RetrieveDataAsync *Request,
                           ServerWriter<Data> *Writer) override;

  Status DataExchange(ServerContext *Context, const ExchangeData *Request,
                      Scalar *Reply) override;
  Status DataExchangeAsync(ServerContext *Context,
                           const ExchangeDataAsync *Request,
                           Scalar *Reply) override;

  Status DataDelete(ServerContext *Context, const DeleteData *Request,
                    Scalar *Reply) override;

  /////////////////////////////////////////////////////////////////////////////

  Status RunTargetRegion(ServerContext *Context, const TargetRegion *Request,
                         Scalar *Reply) override;
  Status RunTargetRegionAsync(ServerContext *Context,
                              const TargetRegionAsync *Request,
                              Scalar *Reply) override;

  Status RunTargetTeamRegion(ServerContext *Context,
                             const TargetTeamRegion *Request,
                             Scalar *Reply) override;
  Status RunTargetTeamRegionAsync(ServerContext *Context,
                                  const TargetTeamRegionAsync *Request,
                                  Scalar *Reply) override;
};
