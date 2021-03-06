/*
 *
 * Copyright 2015-2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <mutex>
#include <thread>

#include <grpc++/channel.h>
#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc/grpc.h>
#include <grpc/support/thd.h>
#include <grpc/support/time.h>
#include <gtest/gtest.h>

#include "src/core/surface/api_trace.h"
#include "src/proto/grpc/testing/duplicate/echo_duplicate.grpc.pb.h"
#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

using grpc::testing::EchoRequest;
using grpc::testing::EchoResponse;
using std::chrono::system_clock;

const int kNumThreads = 100;  // Number of threads
const int kNumAsyncSendThreads = 2;
const int kNumAsyncReceiveThreads = 50;
const int kNumRpcs = 1000;  // Number of RPCs per thread

namespace grpc {
namespace testing {

namespace {

// When echo_deadline is requested, deadline seen in the ServerContext is set in
// the response in seconds.
void MaybeEchoDeadline(ServerContext* context, const EchoRequest* request,
                       EchoResponse* response) {
  if (request->has_param() && request->param().echo_deadline()) {
    gpr_timespec deadline = gpr_inf_future(GPR_CLOCK_REALTIME);
    if (context->deadline() != system_clock::time_point::max()) {
      Timepoint2Timespec(context->deadline(), &deadline);
    }
    response->mutable_param()->set_request_deadline(deadline.tv_sec);
  }
}

}  // namespace

class TestServiceImpl : public ::grpc::testing::EchoTestService::Service {
 public:
  TestServiceImpl() : signal_client_(false) {}

  Status Echo(ServerContext* context, const EchoRequest* request,
              EchoResponse* response) GRPC_OVERRIDE {
    response->set_message(request->message());
    MaybeEchoDeadline(context, request, response);
    if (request->has_param() && request->param().client_cancel_after_us()) {
      {
        unique_lock<mutex> lock(mu_);
        signal_client_ = true;
      }
      while (!context->IsCancelled()) {
        gpr_sleep_until(gpr_time_add(
            gpr_now(GPR_CLOCK_REALTIME),
            gpr_time_from_micros(request->param().client_cancel_after_us(),
                                 GPR_TIMESPAN)));
      }
      return Status::CANCELLED;
    } else if (request->has_param() &&
               request->param().server_cancel_after_us()) {
      gpr_sleep_until(gpr_time_add(
          gpr_now(GPR_CLOCK_REALTIME),
          gpr_time_from_micros(request->param().server_cancel_after_us(),
                               GPR_TIMESPAN)));
      return Status::CANCELLED;
    } else {
      EXPECT_FALSE(context->IsCancelled());
    }
    return Status::OK;
  }

  // Unimplemented is left unimplemented to test the returned error.

  Status RequestStream(ServerContext* context,
                       ServerReader<EchoRequest>* reader,
                       EchoResponse* response) GRPC_OVERRIDE {
    EchoRequest request;
    response->set_message("");
    while (reader->Read(&request)) {
      response->mutable_message()->append(request.message());
    }
    return Status::OK;
  }

  // Return 3 messages.
  // TODO(yangg) make it generic by adding a parameter into EchoRequest
  Status ResponseStream(ServerContext* context, const EchoRequest* request,
                        ServerWriter<EchoResponse>* writer) GRPC_OVERRIDE {
    EchoResponse response;
    response.set_message(request->message() + "0");
    writer->Write(response);
    response.set_message(request->message() + "1");
    writer->Write(response);
    response.set_message(request->message() + "2");
    writer->Write(response);

    return Status::OK;
  }

  Status BidiStream(ServerContext* context,
                    ServerReaderWriter<EchoResponse, EchoRequest>* stream)
      GRPC_OVERRIDE {
    EchoRequest request;
    EchoResponse response;
    while (stream->Read(&request)) {
      gpr_log(GPR_INFO, "recv msg %s", request.message().c_str());
      response.set_message(request.message());
      stream->Write(response);
    }
    return Status::OK;
  }

  bool signal_client() {
    unique_lock<mutex> lock(mu_);
    return signal_client_;
  }

 private:
  bool signal_client_;
  mutex mu_;
};

class TestServiceImplDupPkg
    : public ::grpc::testing::duplicate::EchoTestService::Service {
 public:
  Status Echo(ServerContext* context, const EchoRequest* request,
              EchoResponse* response) GRPC_OVERRIDE {
    response->set_message("no package");
    return Status::OK;
  }
};

class CommonStressTest {
 public:
  CommonStressTest() : kMaxMessageSize_(8192) {}
  void SetUp() {
    int port = grpc_pick_unused_port_or_die();
    server_address_ << "localhost:" << port;
    // Setup server
    ServerBuilder builder;
    builder.AddListeningPort(server_address_.str(),
                             InsecureServerCredentials());
    builder.RegisterService(&service_);
    builder.SetMaxMessageSize(
        kMaxMessageSize_);  // For testing max message size.
    builder.RegisterService(&dup_pkg_service_);
    server_ = builder.BuildAndStart();
  }
  void TearDown() { server_->Shutdown(); }
  void ResetStub() {
    std::shared_ptr<Channel> channel =
        CreateChannel(server_address_.str(), InsecureChannelCredentials());
    stub_ = grpc::testing::EchoTestService::NewStub(channel);
  }
  grpc::testing::EchoTestService::Stub* GetStub() { return stub_.get(); }

 private:
  std::unique_ptr<grpc::testing::EchoTestService::Stub> stub_;
  std::unique_ptr<Server> server_;
  std::ostringstream server_address_;
  const int kMaxMessageSize_;
  TestServiceImpl service_;
  TestServiceImplDupPkg dup_pkg_service_;
};

class End2endTest : public ::testing::Test {
 protected:
  End2endTest() {}
  void SetUp() GRPC_OVERRIDE { common_.SetUp(); }
  void TearDown() GRPC_OVERRIDE { common_.TearDown(); }
  void ResetStub() { common_.ResetStub(); }

  CommonStressTest common_;
};

static void SendRpc(grpc::testing::EchoTestService::Stub* stub, int num_rpcs) {
  EchoRequest request;
  EchoResponse response;
  request.set_message("Hello");

  for (int i = 0; i < num_rpcs; ++i) {
    ClientContext context;
    Status s = stub->Echo(&context, request, &response);
    EXPECT_EQ(response.message(), request.message());
    EXPECT_TRUE(s.ok());
  }
}

TEST_F(End2endTest, ThreadStress) {
  common_.ResetStub();
  std::vector<std::thread*> threads;
  for (int i = 0; i < kNumThreads; ++i) {
    threads.push_back(new std::thread(SendRpc, common_.GetStub(), kNumRpcs));
  }
  for (int i = 0; i < kNumThreads; ++i) {
    threads[i]->join();
    delete threads[i];
  }
}

class AsyncClientEnd2endTest : public ::testing::Test {
 protected:
  AsyncClientEnd2endTest() : rpcs_outstanding_(0) {}

  void SetUp() GRPC_OVERRIDE { common_.SetUp(); }
  void TearDown() GRPC_OVERRIDE {
    void* ignored_tag;
    bool ignored_ok;
    while (cq_.Next(&ignored_tag, &ignored_ok))
      ;
    common_.TearDown();
  }

  void Wait() {
    unique_lock<mutex> l(mu_);
    while (rpcs_outstanding_ != 0) {
      cv_.wait(l);
    }

    cq_.Shutdown();
  }

  struct AsyncClientCall {
    EchoResponse response;
    ClientContext context;
    Status status;
    std::unique_ptr<ClientAsyncResponseReader<EchoResponse>> response_reader;
  };

  void AsyncSendRpc(int num_rpcs) {
    for (int i = 0; i < num_rpcs; ++i) {
      AsyncClientCall* call = new AsyncClientCall;
      EchoRequest request;
      request.set_message("Hello: " + std::to_string(i));
      call->response_reader =
          common_.GetStub()->AsyncEcho(&call->context, request, &cq_);
      call->response_reader->Finish(&call->response, &call->status,
                                    (void*)call);

      unique_lock<mutex> l(mu_);
      rpcs_outstanding_++;
    }
  }

  void AsyncCompleteRpc() {
    while (true) {
      void* got_tag;
      bool ok = false;
      if (!cq_.Next(&got_tag, &ok)) break;
      AsyncClientCall* call = static_cast<AsyncClientCall*>(got_tag);
      if (!ok) {
        gpr_log(GPR_DEBUG, "Error: %d", call->status.error_code());
      }
      delete call;

      bool notify;
      {
        unique_lock<mutex> l(mu_);
        rpcs_outstanding_--;
        notify = (rpcs_outstanding_ == 0);
      }
      if (notify) {
        cv_.notify_all();
      }
    }
  }

  CommonStressTest common_;
  CompletionQueue cq_;
  mutex mu_;
  condition_variable cv_;
  int rpcs_outstanding_;
};

TEST_F(AsyncClientEnd2endTest, ThreadStress) {
  common_.ResetStub();
  std::vector<std::thread *> send_threads, completion_threads;
  for (int i = 0; i < kNumAsyncReceiveThreads; ++i) {
    completion_threads.push_back(new std::thread(
        &AsyncClientEnd2endTest_ThreadStress_Test::AsyncCompleteRpc, this));
  }
  for (int i = 0; i < kNumAsyncSendThreads; ++i) {
    send_threads.push_back(
        new std::thread(&AsyncClientEnd2endTest_ThreadStress_Test::AsyncSendRpc,
                        this, kNumRpcs));
  }
  for (int i = 0; i < kNumAsyncSendThreads; ++i) {
    send_threads[i]->join();
    delete send_threads[i];
  }

  Wait();
  for (int i = 0; i < kNumAsyncReceiveThreads; ++i) {
    completion_threads[i]->join();
    delete completion_threads[i];
  }
}

}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  grpc_test_init(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
