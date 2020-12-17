//===---------------- Utils.cpp - Utilities for Remote RTL ----------------===//
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

#include <bits/stdint-intn.h>
#include <bits/stdint-uintn.h>
#include <cstdlib>
#include <string>

#include "Utils.h"
#include "omptarget.h"

void loadTargetBinaryDescription(const __tgt_bin_desc *Desc,
                                 TargetBinaryDescription &Request) {
  // Keeps track of entries which have already been deep copied.
  std::vector<void *> DeepCopiedEntryAddrs;

  // Copy Global Offload Entries
  for (auto *CurEntry = Desc->HostEntriesBegin;
       CurEntry != Desc->HostEntriesEnd; CurEntry++) {
    auto *NewEntry = Request.add_entries();
    copyOffloadEntry(CurEntry, NewEntry);

    // Copy the pointer of the offload entry of the image into the Request
    Request.add_entry_ptrs((uint64_t)CurEntry);
    DeepCopiedEntryAddrs.push_back(CurEntry);
  }

  // Copy Device Images and Device Offload Entries
  __tgt_device_image *CurImage = Desc->DeviceImages;
  for (auto I = 0; I < Desc->NumDeviceImages; I++, CurImage++) {
    auto *Image = Request.add_images();
    auto Size = (char *)CurImage->ImageEnd - (char *)CurImage->ImageStart;
    Image->set_binary(CurImage->ImageStart, Size);

    // Copy the pointer of the image into the Request
    auto *NewImagePtr = Request.add_image_ptrs();
    NewImagePtr->set_img_ptr((uint64_t)CurImage->ImageStart);

    // Copy Device Offload Entries
    for (auto *CurEntry = CurImage->EntriesBegin;
         CurEntry != CurImage->EntriesEnd; CurEntry++) {
      auto *NewEntry = Image->add_entries();

      auto Entry = std::find(DeepCopiedEntryAddrs.begin(),
                             DeepCopiedEntryAddrs.end(), CurEntry);
      if (Entry != DeepCopiedEntryAddrs.end()) {
        // Offload entry has already been loaded
        shallowCopyOffloadEntry(CurEntry, NewEntry);
      } else { // Offload Entry has not been loaded into the Request
        copyOffloadEntry(CurEntry, NewEntry);
        DeepCopiedEntryAddrs.push_back(CurEntry);
      }

      // Copy the pointer of the offload entry of the image into the Request
      NewImagePtr->add_entry_ptrs((uint64_t)CurEntry);
    }
  }
}

void unloadTargetBinaryDescription(
    const TargetBinaryDescription *Request, __tgt_bin_desc *Desc,
    std::map<const void *, __tgt_device_image *> &HostToRemoteDeviceImage,
    std::map<const void *, __tgt_offload_entry *> &HostToRemoteOffloadEntry) {
  (*Desc).NumDeviceImages = Request->images_size();
  (*Desc).DeviceImages = (__tgt_device_image *)malloc(
      sizeof(__tgt_device_image) * (*Desc).NumDeviceImages);

  if (Request->entries_size())
    (*Desc).HostEntriesBegin = (__tgt_offload_entry *)malloc(
        sizeof(__tgt_offload_entry) * Request->entries_size());
  else {
    Desc->HostEntriesBegin = nullptr;
    Desc->HostEntriesEnd = nullptr;
  }

  // Copy Global Offload Entries
  __tgt_offload_entry *CurEntry = (*Desc).HostEntriesBegin;
  const auto *EntryPtrItr = Request->entry_ptrs().begin();
  for (auto Entry : Request->entries()) {
    copyOffloadEntry(Entry, CurEntry);
    HostToRemoteOffloadEntry[(void *)*EntryPtrItr] = CurEntry;
    CurEntry++;
    EntryPtrItr++;
  }
  (*Desc).HostEntriesEnd = CurEntry;

  // Copy Device Images and Device Offload Entries
  __tgt_device_image *CurImage = (*Desc).DeviceImages;
  auto ImageItr = Request->image_ptrs().begin();
  for (auto Image : Request->images()) {
    // Copy Device Offload Entries
    auto *CurEntry = (*Desc).HostEntriesBegin;
    const auto *EntryPtrItr = ImageItr->entry_ptrs().begin();
    bool Found = false;

    if (!Desc->HostEntriesBegin) {
      CurImage->EntriesBegin = nullptr;
      CurImage->EntriesEnd = nullptr;
    }

    for (auto Entry : Image.entries()) {
      auto TgtEntry = HostToRemoteOffloadEntry.find((void *)*EntryPtrItr);
      if (TgtEntry != HostToRemoteOffloadEntry.end()) {
        if (!Found)
          (*CurImage).EntriesBegin = CurEntry;

        Found = true;
        if (Found) {
          (*CurImage).EntriesEnd = CurEntry + 1;
        }
      } else {
        Found = false;
        copyOffloadEntry(Entry, CurEntry);
        HostToRemoteOffloadEntry[static_cast<const void *>(EntryPtrItr)] =
            CurEntry;
      }
      CurEntry++;
      EntryPtrItr++;
    }

    // Copy Device Image
    CurImage->ImageStart = malloc(Image.binary().size());
    memcpy(CurImage->ImageStart,
           static_cast<const void *>(Image.binary().data()),
           Image.binary().size());
    CurImage->ImageEnd =
        (void *)((char *)CurImage->ImageStart + Image.binary().size());
    fprintf(stderr, "Device Ptr: (%p) %p, Start (%p)\n",
            (void *)ImageItr->img_ptr(), CurImage, CurImage->ImageStart);

    HostToRemoteDeviceImage[(void *)ImageItr->img_ptr()] = CurImage;
    CurImage++;
    ImageItr++;
  }
}

void freeTargetBinaryDescription(__tgt_bin_desc *Desc) {
  __tgt_device_image *CurImage = Desc->DeviceImages;
  for (auto I = 0; I < Desc->NumDeviceImages; I++, CurImage++) {
    free(CurImage);
  }

  for (auto it = Desc->HostEntriesBegin; it != Desc->HostEntriesEnd; it++) {
    free (it->name);
    free (it->addr);
  }

  free(Desc->HostEntriesBegin);
  free(Desc);
}

void loadTargetTable(__tgt_target_table *Table, TargetTable &TableResponse,
                     __tgt_device_image *Image) {
  auto *ImageEntry = Image->EntriesBegin;
  for (__tgt_offload_entry *CurEntry = Table->EntriesBegin;
       CurEntry != Table->EntriesEnd; CurEntry++, ImageEntry++) {
    // We don't necessarily need to return all of this information, really just
    // the device addresses but we include everything for now.
    auto *NewEntry = TableResponse.add_entries();
    NewEntry->set_name(CurEntry->name);
    NewEntry->set_addr((uint64_t)CurEntry->addr);
    NewEntry->set_flags(CurEntry->flags);
    NewEntry->set_reserved(CurEntry->reserved);
    NewEntry->set_size(CurEntry->size);
    TableResponse.add_entry_ptrs((int64_t)CurEntry);
  }
}

void unloadTargetTable(TargetTable &TableResponse, __tgt_target_table *Table,
                       std::map<void *, void *> &HostToRemoteTargetTableMap) {
  (*Table).EntriesBegin = (__tgt_offload_entry *)malloc(
      sizeof(__tgt_offload_entry) * TableResponse.entries_size());

  auto *CurEntry = (*Table).EntriesBegin;
  const auto *EntryPtrItr = TableResponse.entry_ptrs().begin();
  for (auto Entry : TableResponse.entries()) {
    copyOffloadEntry(Entry, CurEntry);
    HostToRemoteTargetTableMap[CurEntry->addr] = (void *)*EntryPtrItr;
    CurEntry++;
    EntryPtrItr++;
  }
  (*Table).EntriesEnd = CurEntry;
}

void copyOffloadEntry(const TargetOffloadEntry &EntryResponse,
                      __tgt_offload_entry *Entry) {
  Entry->name = strdup(EntryResponse.name().c_str());
  Entry->reserved = EntryResponse.reserved();
  Entry->flags = EntryResponse.flags();
  Entry->addr = strdup(EntryResponse.data().c_str());
  Entry->size = EntryResponse.data().size();
}

void copyOffloadEntry(const DeviceOffloadEntry &EntryResponse,
                      __tgt_offload_entry *Entry) {
  Entry->name = strdup(EntryResponse.name().c_str());
  Entry->reserved = EntryResponse.reserved();
  Entry->flags = EntryResponse.flags();
  Entry->addr = (void *)EntryResponse.addr();
  Entry->size = EntryResponse.size();
}

/// We shallow copy with just the name because it is a convenient identifier, we
/// do actually just match off of the address.
void shallowCopyOffloadEntry(const __tgt_offload_entry *Entry,
                             TargetOffloadEntry *EntryResponse) {
  EntryResponse->set_name(Entry->name);
}

void copyOffloadEntry(const __tgt_offload_entry *Entry,
                      TargetOffloadEntry *EntryResponse) {
  shallowCopyOffloadEntry(Entry, EntryResponse);
  EntryResponse->set_reserved(Entry->reserved);
  EntryResponse->set_flags(Entry->flags);
  EntryResponse->set_data(Entry->addr, Entry->size);
}

void dump(const void *Start, const void *End) {
  unsigned char Line[17];
  const unsigned char *PrintCharacter = (const unsigned char *)Start;

  unsigned int I = 0;
  for (; I < ((const int *)End - (const int *)Start); I++) {
    if ((I % 16) == 0) {
      if (I != 0)
        printf("  %s\n", Line);

      printf("  %04x ", I);
    }

    printf(" %02x", PrintCharacter[I]);

    if ((PrintCharacter[I] < 0x20) || (PrintCharacter[I] > 0x7e))
      Line[I % 16] = '.';
    else
      Line[I % 16] = PrintCharacter[I];

    Line[(I % 16) + 1] = '\0';
  }

  while ((I % 16) != 0) {
    printf("   ");
    I++;
  }

  printf("  %s\n", Line);
}

void dump(__tgt_offload_entry *Entry) {
  fprintf(stderr, "Entry (%p):\n", (void *)Entry);
  fprintf(stderr, "  Name: %s (%p)\n", Entry->name, (void *)&Entry->name);
  fprintf(stderr, "  Reserved: %d (%p)\n", Entry->reserved,
          (void *)&Entry->reserved);
  fprintf(stderr, "  Flags: %d (%p)\n", Entry->flags, (void *)&Entry->flags);
  fprintf(stderr, "  Addr: %p\n", Entry->addr);
  fprintf(stderr, "  Size: %lu\n", Entry->size);
}

void dump(__tgt_target_table *Table) {
  for (auto *CurEntry = Table->EntriesBegin; CurEntry != Table->EntriesEnd;
       CurEntry++)
    dump(CurEntry);
}

void dump(TargetOffloadEntry Entry) {
  fprintf(stderr, "Entry: ");
  fprintf(stderr, "    %s\n", Entry.name().c_str());
  fprintf(stderr, "    %d\n", Entry.reserved());
  fprintf(stderr, "    %d\n", Entry.flags());
  fprintf(stderr, "    %ld\n", Entry.data().size());
  dump(static_cast<const void *>(Entry.data().data()),
       static_cast<const void *>((Entry.data().c_str() + Entry.data().size())));
}

void dump(__tgt_device_image *Image) {
  dump(Image->ImageStart, Image->ImageEnd);
  __tgt_offload_entry *EntryItr = Image->EntriesBegin;
  for (; EntryItr != Image->EntriesEnd; EntryItr++)
    dump(EntryItr);
}

void dump(std::map<void *, __tgt_offload_entry *> &Map) {
  fprintf(stderr, "Host to Remote Entry Map:\n");
  for (auto Entry : Map)
    fprintf(stderr, "  Host (%p) -> Tgt (%p): Addr((%p))\n", Entry.first,
            (void *)Entry.second, (void *)Entry.second->addr);
}
