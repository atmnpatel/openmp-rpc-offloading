//===----------------- Utils.h - Utilities for Remote RTL -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities for data movement and debugging.
//
//===----------------------------------------------------------------------===//

#include "Debug.h"
#include "omptarget.h"
#include "openmp.grpc.pb.h"
#include "openmp.pb.h"
#include "rtl.h"
#include <string>

#define CLIENT_DBG(...)                                                        \
  {                                                                            \
    if (DebugLevel > 0) {                                                      \
      fprintf(stderr, "[[Client]] --> ");                                      \
      fprintf(stderr, __VA_ARGS__);                                            \
    }                                                                          \
  }

#define CLIENT_RDBG(...)                                                       \
  {                                                                            \
    if (DebugLevel > 0) {                                                      \
      fprintf(stderr, "\33[2K\r[[Client]] --> ");                              \
      fprintf(stderr, __VA_ARGS__);                                            \
    }                                                                          \
  }

#define SERVER_DBG(...)                                                        \
  {                                                                            \
    if (DebugLevel > 0) {                                                      \
      fprintf(stderr, "[[Server]] --> ");                                      \
      fprintf(stderr, __VA_ARGS__);                                            \
    }                                                                          \
  }

using remoteoffloading::DeviceOffloadEntry;
using remoteoffloading::TargetBinaryDescription;
using remoteoffloading::TargetOffloadEntry;
using remoteoffloading::TargetTable;

/// Loads a __tgt_bin_desc * into a TargetBinaryDescription protobuf message for
/// transmission
void loadTargetBinaryDescription(const __tgt_bin_desc *Desc,
                                 TargetBinaryDescription &Request);

/// Loads an already malloc'd __tgt_bin_desc * with the data from a received
/// TargetBinaryDescription protobuf message
void unloadTargetBinaryDescription(
    const TargetBinaryDescription *Request, __tgt_bin_desc *Desc,
    std::map<const void *, __tgt_device_image *> &HostToRemoteDeviceImage,
    std::map<const void *, __tgt_offload_entry *> &HostToRemoteOffloadEntry);

/// Frees the __tgt_bin_desc * (struct and members) allocated by
/// LoadTargetBinaryDescription
void freeTargetBinaryDescription(__tgt_bin_desc *Desc);

/// Copies __tgt_offload_entry from TargetOffloadEntry protobuf message to
/// __tgt_bin_desc
void copyOffloadEntry(const TargetOffloadEntry &EntryResponse,
                      __tgt_offload_entry *Entry);

/// Copies __tgt_offload_entry from __tgt_bin_desc into the TargetOffloadEntry
/// protobuf message
void copyOffloadEntry(const __tgt_offload_entry *Entry,
                      TargetOffloadEntry *EntryResponse);

/// Copies the name of a __tgt_offload_entry from __tgt_bin_desc into the
/// TargetOffloadEntry protobuf message, this acts as a placeholder for an
/// entry that has already been deep copied.
void shallowCopyOffloadEntry(const __tgt_offload_entry *Entry,
                             TargetOffloadEntry *EntryResponse);

/// Copies Device Offload Entries, which are usual offload entries minus the
/// data
void copyOffloadEntry(const DeviceOffloadEntry &EntryResponse,
                      __tgt_offload_entry *Entry);

/// Loads a __tgt_target_table * into a TargetTable protobuf message for
/// transmission
void loadTargetTable(__tgt_target_table *Table, TargetTable &TableResponse,
                     __tgt_device_image *Image);

/// Loads an already malloc'd __tgt_target_table * with the data from a received
/// TargetTable protobuf message
void unloadTargetTable(TargetTable &TableResponse, __tgt_target_table *Table,
                       std::map<void *, void *> &HostToRemoteTargetTableMap);

/// Frees the __tgt_target_table * (struct and members) allocated by
/// LoadTargetTable
void freeTargetTable(__tgt_target_table *Table);

void dump(const void *Start, const void *End);

void dump(__tgt_offload_entry *Entry);
void dump(TargetOffloadEntry Entry);

void dump(__tgt_target_table *Table);
void dump(__tgt_device_image *Image);

void dump(std::map<void *, __tgt_offload_entry *> &Map);
