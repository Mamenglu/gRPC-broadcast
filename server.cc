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
    
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    BroadcastServiceImpl service;

    ServerBuilder builder;
   
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
   
    builder.RegisterService(&service);
   
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    server->Wait();
}

int main(int argc, char** argv) {
    absl::ParseCommandLine(argc, argv);
    RunServer(absl::GetFlag(FLAGS_port));
    return 0;
}

