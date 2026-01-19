// frameworks/av/services/camera/libcameraservice/device3/CameraStreamTransmitter.h

#ifndef ANDROID_SERVERS_CAMERA_CAMERA3_STREAM_TRANSMITTER_H
#define ANDROID_SERVERS_CAMERA_CAMERA3_STREAM_TRANSMITTER_H

#include <stdint.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>

#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <system/graphics.h>

namespace android {
namespace camera3 {

// 帧数据结构
struct FrameData {
    int32_t frameNumber;
    int32_t streamId;
    uint32_t width;
    uint32_t height;
    int32_t format;
    int64_t timestamp;
    
    // YUV数据
    std::vector<uint8_t> yData;
    std::vector<uint8_t> uData;
    std::vector<uint8_t> vData;
    
    size_t yStride;
    size_t cStride;
    size_t chromaStep;
};

// 网络协议头部
struct FrameHeader {
    uint32_t magic;          // 魔数：0xCAFEBABE
    int32_t frameNumber;
    int32_t streamId;
    uint32_t width;
    uint32_t height;
    int32_t format;
    int64_t timestamp;
    
    uint32_t yDataSize;
    uint32_t uDataSize;
    uint32_t vDataSize;
    
    uint32_t yStride;
    uint32_t cStride;
    uint32_t chromaStep;
    
    uint32_t reserved[4];    // 保留字段，未来扩展
} __attribute__((packed));

class CameraStreamTransmitter {
public:
    static CameraStreamTransmitter& getInstance();
    
    ~CameraStreamTransmitter();
    
    // 启动/停止传输器
    bool start(int port = 8888);
    void stop();
    
    // 发送帧到PC，返回修改后的帧（阻塞，有超时）
    bool sendAndReceiveFrame(
        buffer_handle_t buffer,
        int32_t frameNumber,
        int32_t streamId,
        uint32_t width,
        uint32_t height,
        int32_t format,
        int64_t timestamp,
        uint32_t timeout_ms = 100  // 100ms超时
    );
    
    // 检查是否已连接
    bool isConnected() const { return connected_.load(); }
    
private:
    CameraStreamTransmitter();
    CameraStreamTransmitter(const CameraStreamTransmitter&) = delete;
    CameraStreamTransmitter& operator=(const CameraStreamTransmitter&) = delete;
    
    std::vector<uint8_t> mTxBuffer; // 新增发送缓存
    std::vector<uint8_t> mRxBuffer; // 新增接收缓存
    std::mutex mTxMutex;            // 发送缓存锁（可选，如果单线程发送）

    // 网络线程函数
    void networkThreadFunc();
    std::string waitForPCBroadcast(); // 新增
    void sendThreadFunc();
    void receiveThreadFunc();
    
    // 辅助函数
    bool connectToServer(const char* ip, int port);
    bool extractFrameData(buffer_handle_t buffer, uint32_t width, uint32_t height, int format,
                         FrameData& frameData);
    bool writeFrameData(buffer_handle_t buffer, uint32_t width, uint32_t height,
                       const FrameData& frameData);
    
    bool sendFrame(const FrameData& frame);
    bool receiveFrame(FrameData& frame);
    
    // 成员变量
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    
    int sockfd_;
    
    // 发送队列
    std::queue<std::shared_ptr<FrameData>> sendQueue_;
    std::mutex sendMutex_;
    std::condition_variable sendCV_;
    
    // 接收队列
    std::deque<std::shared_ptr<FrameData>> receiveQueue_;
    std::mutex receiveMutex_;
    std::condition_variable receiveCV_;
    
    // 等待响应的帧
    std::map<int32_t, std::shared_ptr<FrameData>> pendingFrames_;
    std::mutex pendingMutex_;
    
    // 线程
    std::thread networkThread_;
    std::thread sendThread_;
    std::thread receiveThread_;
    
    static constexpr uint32_t MAGIC_NUMBER = 0xCAFEBABE;
    static constexpr size_t MAX_QUEUE_SIZE = 30;  // 最多缓存30帧
};

} // namespace camera3
} // namespace android

#endif // ANDROID_SERVERS_CAMERA_CAMERA3_STREAM_TRANSMITTER_H