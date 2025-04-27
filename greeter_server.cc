// /*
//  *
//  * Copyright 2015 gRPC authors.
//  *
//  * Licensed under the Apache License, Version 2.0 (the "License");
//  * you may not use this file except in compliance with the License.
//  * You may obtain a copy of the License at
//  *
//  *     http://www.apache.org/licenses/LICENSE-2.0
//  *
//  * Unless required by applicable law or agreed to in writing, software
//  * distributed under the License is distributed on an "AS IS" BASIS,
//  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  * See the License for the specific language governing permissions and
//  * limitations under the License.
//  *
//  */

// #include <grpcpp/ext/proto_server_reflection_plugin.h>
// #include <grpcpp/grpcpp.h>
// #include <grpcpp/health_check_service_interface.h>

// #include <memory>
// #include <string>
// #include <mutex>
// #include <unordered_set>
// #include <vector>
// #include <condition_variable>
// #include <iostream>

// #include "absl/flags/flag.h"
// #include "absl/flags/parse.h"
// #include "absl/strings/str_format.h"

// #include "broadcast.grpc.pb.h"

// using grpc::Server;
// using grpc::ServerBuilder;
// using grpc::ServerContext;
// using grpc::ServerReader;
// using grpc::ServerWriter;
// using grpc::Status;

// using broadcast::Broadcast;
// using broadcast::SubscribeReq;
// using broadcast::Frame;
// using broadcast::Ack;

// ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

// struct Subscriber {
//     ServerWriter<Frame>* writer;
//     int64_t next_seq_no;  // 下一条要发送的frame的序号
// };

// class BroadcastServiceImpl final : public Broadcast::Service {
// public:
//     Status Publish(ServerContext* context, ServerReader<Frame>* reader, Ack* ack) override {
//         Frame frame;
//         while (reader->Read(&frame)) {
//             std::cout << "[Server] Received frame seq_no=" << frame.seq_no() << std::endl;
//             AddFrame(frame);
//         }
//         ack->set_message("Frames received");
//         return Status::OK;
//     }

//     Status Subscribe(ServerContext* context, const SubscribeReq* request, ServerWriter<Frame>* writer) override {
//         int64_t start_seq = request->replay_seq_no_start();
//         Subscriber subscriber{writer, start_seq};

//         {
//             std::unique_lock<std::mutex> lock(mu_);
//             std::cout << "[Server] New subscriber, start from seq_no=" << start_seq << std::endl;
//             subscribers_.insert(&subscriber);

//             // 先发送已有历史Frame
//             SendPendingFrames(subscriber);
//         }

//         // 然后阻塞等待新Frame
//         while (!context->IsCancelled()) {
//             std::unique_lock<std::mutex> lock(mu_);
//             cv_.wait(lock, [this, &subscriber]() {
//                 return !frames_.empty() && frames_.back().seq_no() >= subscriber.next_seq_no;
//             });

//             if (context->IsCancelled()) {
//                 break;
//             }

//             SendPendingFrames(subscriber);
//         }

//         {
//             std::unique_lock<std::mutex> lock(mu_);
//             subscribers_.erase(&subscriber);
//             std::cout << "[Server] Subscriber disconnected." << std::endl;
//         }

//         return Status::OK;
//     }

// private:
//     void AddFrame(const Frame& frame) {
//         std::lock_guard<std::mutex> lock(mu_);
//         frames_.push_back(frame);

//         // 通知所有订阅者
//         cv_.notify_all();
//     }

//     void SendPendingFrames(Subscriber& subscriber) {
//         for (const auto& frame : frames_) {
//             if (frame.seq_no() >= subscriber.next_seq_no) {
//                 if (!subscriber.writer->Write(frame)) {
//                     std::cout << "[Server] Failed to write to subscriber, aborting." << std::endl;
//                     return;
//                 }
//                 subscriber.next_seq_no = frame.seq_no() + 1;
//             }
//         }
//     }

//     std::vector<Frame> frames_;
//     std::unordered_set<Subscriber*> subscribers_;
//     std::mutex mu_;
//     std::condition_variable cv_;
// };

// void RunServer(uint16_t port) {
//     std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
//     BroadcastServiceImpl service;

//     grpc::EnableDefaultHealthCheckService(true);
//     grpc::reflection::InitProtoReflectionServerBuilderPlugin();
//     ServerBuilder builder;
//     builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
//     builder.RegisterService(&service);
//     std::unique_ptr<Server> server(builder.BuildAndStart());
//     std::cout << "[Server] Listening on " << server_address << std::endl;
//     server->Wait();
// }

// int main(int argc, char** argv) {
//     absl::ParseCommandLine(argc, argv);
//     RunServer(absl::GetFlag(FLAGS_port));
//     return 0;
// }
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"

#include <grpcpp/grpcpp.h>
#include "broadcast.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::ServerReader;
using grpc::Status;

using broadcast::Broadcast;
using broadcast::Frame;
using broadcast::SubscribeReq;
using broadcast::Ack;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

class BroadcastServiceImpl final : public Broadcast::Service {
private:
    std::mutex mtx_;
    std::vector<Frame> frames_; 
    int next_client_id_ = 1;

    struct SubscriberSession {
        int replay_seq_no_start;//起始号
        ServerWriter<Frame>* writer;
    };

    std::unordered_map<int, SubscriberSession> subscribers_; 

    //处理序列号重复
    static bool FrameSort(const Frame& a, const Frame& b) {
        if (a.seq_no() != b.seq_no()) {
            return a.seq_no() < b.seq_no();
        }
        return a.timestamp() < b.timestamp();
    }

public:
    // 客户端调用Subscribe
    Status Subscribe(ServerContext* context, const SubscribeReq* request, ServerWriter<Frame>* writer) override {
        int client_id;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            client_id = next_client_id_++;
            subscribers_[client_id] = {request->replay_seq_no_start(), writer};

            //复制一下历史数据 防止读写冲突
            std::vector<Frame> sorted_frames = frames_;
            std::sort(sorted_frames.begin(), sorted_frames.end(), FrameSort);

            // 发送大雨replay_seq_no_start的历史frame
            for (const auto& frame : sorted_frames) {
                if (frame.seq_no() >= request->replay_seq_no_start()) {
                    writer->Write(frame);
                }
            }
        }

        while (!context->IsCancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // 客户端断开
        {
            std::lock_guard<std::mutex> lock(mtx_);
            subscribers_.erase(client_id);
            std::cout << "Client " << client_id << " disconnected." << std::endl;
        }

        return Status::OK;
    }

    // 客户端调用Publish
    Status Publish(ServerContext* context, ServerReader<Frame>* reader, Ack* response) override {
        Frame frame;
        while (reader->Read(&frame)) {
            std::lock_guard<std::mutex> lock(mtx_);

            frames_.push_back(frame);
            std::cout << "Received frame: seq_no=" << frame.seq_no() << std::endl;

            // 广播给所有订阅者
            for (auto& [client_id, session] : subscribers_) {
                if (frame.seq_no() >= session.replay_seq_no_start) {
                    //session.writer->Write(frame);
                    if (!session.writer->Write(frame)) {// 异常处理 
                        std::cerr << "Failed to write to client " << client_id << std::endl;
                        continue;  
                    }
                }

            }
        }

        response->set_message("Frames received");
        return Status::OK;
    }
};

void RunServer(uint16_t port) {
    // std::string server_address("0.0.0.0:50051");
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    BroadcastServiceImpl service;

    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
}

int main(int argc, char** argv) {
    absl::ParseCommandLine(argc, argv);
    RunServer(absl::GetFlag(FLAGS_port));
    return 0;
}

