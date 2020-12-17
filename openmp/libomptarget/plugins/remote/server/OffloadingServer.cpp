//===------------- OffloadingServer.cpp - Server Application --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Offloading server for remote host.
//
//===----------------------------------------------------------------------===//

#include <future>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <thread>

#include "Server.h"

using grpc::Server;
using grpc::ServerBuilder;

std::promise<void> ShutdownPromise;

int main() {
  std::string ServerAddress = "0.0.0.0:50051";
  if (const char *Env = std::getenv("GRPC_ADDRESS"))
    ServerAddress = Env;
  uint64_t MaxSize = 1 << 30;
  if (const char *Env = std::getenv("GRPC_ALLOCATOR_MAX"))
    MaxSize = std::stoi(Env);
  int BlockSize = 1 << 20;
  if (const char *Env = std::getenv("GRPC_BLOCK_SIZE"))
    BlockSize = std::stoi(Env);

  RemoteOffloadImpl Service(MaxSize, BlockSize);

  ServerBuilder Builder;
  Builder.AddListeningPort(ServerAddress, grpc::InsecureServerCredentials());
  Builder.RegisterService(&Service);
  Builder.SetMaxMessageSize(INT_MAX);
  std::unique_ptr<Server> Server(Builder.BuildAndStart());
  std::cout << "Server listening on " << ServerAddress << std::endl;

  auto WaitForServer = [&] () {
    Server->Wait();
  };

  std::thread ServerThread(WaitForServer);

  auto ShutdownFuture = ShutdownPromise.get_future();
  ShutdownFuture.wait();
  Server->Shutdown();
  ServerThread.join();

  return 0;
}
