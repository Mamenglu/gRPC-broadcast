import grpc
import broadcast_pb2
import broadcast_pb2_grpc
import threading
import time

class BroadcastClient:
    def __init__(self, server_address='0.0.0.0:50051'):
        self.channel = grpc.insecure_channel(server_address)
        self.stub = broadcast_pb2_grpc.BroadcastStub(self.channel)

    def subscribe(self, replay_seq_no_start=0):#订阅
        response = self.stub.Subscribe(broadcast_pb2.SubscribeReq(replay_seq_no_start=replay_seq_no_start))
        return response

    def publish(self, frames):#发
        def frame_generator():
            for frame in frames:
                yield frame
                time.sleep(1) #每秒发一个
        
        ack = self.stub.Publish(frame_generator())
        return ack.message

    def create_frame(self, seq_no, payload):
        timestamp = int(time.time() * 1000) #加个事假戳 当前时间
        return broadcast_pb2.Frame(seq_no=seq_no, payload=payload.encode(),timestamp=timestamp)

    def run(self, subscribe_seq_no_start=0, frames_to_publish=None):

        # 订阅
        sub_thread = threading.Thread(target=self._subscribe_thread, args=(subscribe_seq_no_start,))
        sub_thread.start()

        # 发布
        pub_thread = threading.Thread(target=self._publish_thread, args=(frames_to_publish,))
        pub_thread.start()

        # 等待订阅和发布完成
        sub_thread.join()
        pub_thread.join()

    def _subscribe_thread(self, replay_seq_no_start):# 订阅线程
        for frame in self.subscribe(replay_seq_no_start):
            print("Received frame:", frame.seq_no, frame.payload)

    def _publish_thread(self, frames_to_publish):# 发布线程
        ack_message = self.publish(frames_to_publish)
        print("Publish finished:", ack_message)



if __name__ == '__main__':

    client = BroadcastClient(server_address='0.0.0.0:50051')

    # 定义帧
    frames = [client.create_frame(seq_no=i, payload=f"message {i}") for i in range(1, 6)]
    # 发送帧和subscribe_seq_no_start
    client.run(subscribe_seq_no_start=0, frames_to_publish=frames)

