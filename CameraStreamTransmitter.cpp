// frameworks/av/services/camera/libcameraservice/device3/CameraStreamTransmitter.cpp

#define LOG_TAG "CameraStreamTransmitter"

#include "CameraStreamTransmitter.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netinet/tcp.h> // 定义 TCP_NODELAY
#include <log/log.h>
#include <algorithm>

namespace android {
namespace camera3 {

static const int TCP_PORT = 8888;
static const int UDP_PORT = 8889;
static const uint32_t MAGIC_NUMBER = 0xCAFEBABE;


CameraStreamTransmitter& CameraStreamTransmitter::getInstance() {
    static CameraStreamTransmitter instance;
    return instance;
}

CameraStreamTransmitter::CameraStreamTransmitter() 
    : running_(false), connected_(false), sockfd_(-1) {
    mTxBuffer.reserve(4 * 1024 * 1024);
    mRxBuffer.reserve(4 * 1024 * 1024);
}

CameraStreamTransmitter::~CameraStreamTransmitter() {
    stop();
}



// ===========================================================================
// 1. 异步启动 (保持不变，防止 CameraService 启动卡死)
// ===========================================================================

bool CameraStreamTransmitter::start(int port) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true; 
    }
    
    // 修改点：在 Log 中引用 port 变量，解决 "unused parameter" 报错
    ALOGI("MYJdebug5: Starting CameraStreamTransmitter with Auto-Discovery (initial port param=%d)...", port);
    
    connected_.store(false);
    
    networkThread_ = std::thread(&CameraStreamTransmitter::networkThreadFunc, this);
    sendThread_ = std::thread(&CameraStreamTransmitter::sendThreadFunc, this);
    receiveThread_ = std::thread(&CameraStreamTransmitter::receiveThreadFunc, this);
    
    return true;
}

void CameraStreamTransmitter::stop() {
    if (!running_.exchange(false)) return;
    
    ALOGI("MYJdebug5: Stopping CameraStreamTransmitter");
    connected_.store(false);
    
    sendCV_.notify_all();
    receiveCV_.notify_all();
    
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
        close(sockfd_);
        sockfd_ = -1;
    }
    
    if (networkThread_.joinable()) networkThread_.join();
    if (sendThread_.joinable()) sendThread_.join();
    if (receiveThread_.joinable()) receiveThread_.join();
}


// ===========================================================================
// 2. 网络连接与重连 (保持不变)
// ===========================================================================
void CameraStreamTransmitter::networkThreadFunc() {
    // ALOGI("MYJdebug5: Here1: Starting networkThreadFunc");
    while (running_.load()) {
        // ALOGI("MYJdebug5: Here2: In networkThreadFunc loop");
        if (!connected_.load()) {
            ALOGI("MYJdebug5: Here3: Not connected, attempting discovery...");
            // 第一步：通过 UDP 寻找 PC 的 IP 地址
            std::string pcIp = waitForPCBroadcast();
            ALOGI("MYJdebug5: Here4: waitForPCBroadcast returned IP");
            if (!pcIp.empty() && running_.load()) {
                ALOGI("MYJdebug5: Discovered PC at IP: %s", pcIp.c_str());
                
                // 第二步：尝试建立 TCP 连接
                if (connectToServer(pcIp.c_str(), TCP_PORT)) {
                    ALOGI("MYJdebug5: TCP Connected successfully!");
                    connected_.store(true);
                } else {
                    ALOGE("MYJdebug5: TCP Connect failed, retrying discovery...");
                    usleep(1000000);
                }
            } else {
                // 没收到广播（超时），继续循环重试
                usleep(500000);
            }
        } else {
            sleep(1);
        }
    }
}

// 核心：监听 UDP 广播获取 IP
std::string CameraStreamTransmitter::waitForPCBroadcast() {
    int udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd < 0) return "";

    // 设置 SO_REUSEADDR 方便快速重启
    int opt = 1;
    setsockopt(udpFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udpFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ALOGE("MYJdebug5: UDP bind failed: %s", strerror(errno));
        close(udpFd);
        return "";
    }

    // 设置接收超时，避免死等 (1秒超时)
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(udpFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buffer[64];
    struct sockaddr_in senderAddr;
    socklen_t senderLen = sizeof(senderAddr);

    ALOGV("MYJdebug5: Listening for PC broadcast...");
    ssize_t len = recvfrom(udpFd, buffer, sizeof(buffer) - 1, 0, 
                           (struct sockaddr*)&senderAddr, &senderLen);

    std::string ipStr = "";
    if (len > 0) {
        buffer[len] = '\0';
        if (strstr(buffer, "I_AM_PC")) {
            // 成功收到暗号，提取 IP
            char ipTmp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, ipTmp, INET_ADDRSTRLEN);
            ipStr = std::string(ipTmp);
        }
    }

    close(udpFd);
    return ipStr;
}




// 【优化】使用 SOCK_CLOEXEC 
bool CameraStreamTransmitter::connectToServer(const char* ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) return false;
    
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &serverAddr.sin_addr) <= 0) {
        close(fd);
        return false;
    }
    
    if (connect(fd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        close(fd);
        return false;
    }
    
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    int bufsize = 10 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    
    sockfd_ = fd;
    return true;
}

// ===========================================================================
// 3. 主逻辑流程 (保持不变)
// ===========================================================================
bool CameraStreamTransmitter::sendAndReceiveFrame(
        buffer_handle_t buffer,
        int32_t frameNumber,
        int32_t streamId,
        uint32_t width,
        uint32_t height,
        int32_t format,
        int64_t timestamp,
        uint32_t timeout_ms) {
    
    if (!connected_.load()) return false;
    
    auto frameData = std::make_shared<FrameData>();
    frameData->frameNumber = frameNumber;
    frameData->streamId = streamId;
    frameData->format = format;
    frameData->timestamp = timestamp;
    
    // 提取原始数据 (无压缩)
    if (!extractFrameData(buffer, width, height, format, *frameData)) {
        return false;
    }
    
    
    // 加入发送队列
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (sendQueue_.size() >= MAX_QUEUE_SIZE) {
            sendQueue_.pop();
        }
        sendQueue_.push(frameData);
    }
    sendCV_.notify_one();
    
    // 等待接收
    std::unique_lock<std::mutex> lock(receiveMutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    bool received = receiveCV_.wait_until(lock, deadline, [&]() {
        if (!connected_.load()) return true;
        for (const auto& frame : receiveQueue_) {
            if (frame->frameNumber == frameNumber && frame->streamId == streamId) {
                return true;
            }
        }
        return false;
    });
    
    if (!received || !connected_.load()) return false;
    
    std::shared_ptr<FrameData> responseFrame;
    for (auto it = receiveQueue_.begin(); it != receiveQueue_.end(); ++it) {
        if ((*it)->frameNumber == frameNumber && (*it)->streamId == streamId) {
            responseFrame = *it;
            receiveQueue_.erase(it);
            break;
        }
    }

    if (!responseFrame) return false;
    
    // 写入原始数据 (无解压)
    return writeFrameData(buffer, width, height, *responseFrame);
}

// ===========================================================================
// 4. 提取原始 NV21 数据 (去除 Stride Padding，不缩小)
// ===========================================================================
bool CameraStreamTransmitter::extractFrameData(
        buffer_handle_t buffer, uint32_t width, uint32_t height, int format,
        FrameData& frameData) {
    
    GraphicBufferMapper& mapper = GraphicBufferMapper::get();
    android::Rect bounds(width, height);

    if (format == 0x21) { // 0x21 = HAL_PIXEL_FORMAT_BLOB
        void* vaddr = nullptr;
        // JPEG 必须使用普通的 lock，不能用 lockYCbCr
        if (mapper.lock(buffer, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &vaddr) != 0) {
            ALOGE("MYJDebug: Failed to lock JPEG buffer");
            return false;
        }

        // JPEG 是一块连续的内存。
        // 虽然 JPEG 文件通常比 bitmap 小很多，但为了安全，
        // 我们通常假设 buffer 大小足够容纳最大可能的图像。
        // 简单粗暴的估算：width * height * 1.5 (按照YUV的大小估算，肯定够装JPEG了)
        // 实际上 HAL 分配的 buffer 大小可能就是 max_jpeg_size
        //size_t estimatedSize = width * height * 3 / 2; 
        
        uint8_t* dataPtr = static_cast<uint8_t*>(vaddr);
        
        // 1. 先计算最大容量 (还是按原来的算，作为边界防止越界)
        size_t maxCapacity = width * height * 3 / 2; 
        
        // 2. [关键优化] 寻找 JPEG 的结束标记 (FF D9)
        // 从 buffer 尾部开始往前扫描，找到真实大小
        size_t realSize = maxCapacity;
        bool foundEOI = false;
        
        // 为了防止死循环，我们只扫描最后 90% 的区域，或者扫描整个区域
        // 通常 JPEG 的结尾离 buffer 尾部很远，全是 0
        for (size_t i = maxCapacity - 2; i > 0; i--) {
            // 检测 EOI 标记: FF D9
            if (dataPtr[i] == 0xFF && dataPtr[i+1] == 0xD9) {
                realSize = i + 2; // 真实大小 = 索引 + 2字节
                foundEOI = true;
                break;
            }
        }








        // 调整容器大小
        //frameData.yData.resize(estimatedSize);
        frameData.yData.resize(realSize);
        frameData.uData.clear(); // JPEG 没有独立的 UV
        frameData.vData.clear();

        // 直接内存拷贝：把整个 buffer 当作一个文件拷走
        // vaddr 指向的就是 jpg 文件的开头 (FF D8 ...)
        //memcpy(frameData.yData.data(), vaddr, estimatedSize);
        memcpy(frameData.yData.data(), dataPtr, realSize);


        
        // 设置 metadata
        frameData.width = width;
        frameData.height = height;
        frameData.format = format; // 标记这是 JPEG
        frameData.yStride = width; // 对于 JPEG 来说无实际意义，但为了兼容性填上
        //frameData.yDataSize = estimatedSize;
        mapper.unlock(buffer);
        return true;
    }


    // 非 JPEG 格式，假设是 YUV NV21
    android_ycbcr ycbcr;
    if (mapper.lockYCbCr(buffer, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &ycbcr) != 0) {
        return false;
    }
    if (ycbcr.y == nullptr) {
        mapper.unlock(buffer);
        return false;
    }

    // 设置原始尺寸
    frameData.width = width;
    frameData.height = height;
    // 发送时我们把数据紧凑排列（无 padding），所以 yStride = width
    frameData.yStride = width; 
    frameData.cStride = width / 2;
    frameData.chromaStep = 2; // NV21 step

    // 1. 提取 Y 平面
    // 注意：GraphicBuffer可能有 stride padding，我们需要逐行拷贝去除它
    frameData.yData.resize(width * height);
    uint8_t* srcY = static_cast<uint8_t*>(ycbcr.y);
    
    for (uint32_t r = 0; r < height; r++) {
        // 源地址：包含 padding
        const uint8_t* srcRow = srcY + r * ycbcr.ystride;
        // 目标地址：紧凑排列
        uint8_t* dstRow = &frameData.yData[r * width];
        memcpy(dstRow, srcRow, width);
    }

    // 2. 提取 UV 平面
    if (ycbcr.cb && ycbcr.cr) {
        uint32_t chromaH = height / 2;
        uint32_t chromaW = width / 2; // UV数据量
        frameData.uData.resize(chromaH * chromaW); // 实际上这里存的是 interleaved UV 的一半
        frameData.vData.resize(chromaH * chromaW);
        
        uint8_t* srcCb = static_cast<uint8_t*>(ycbcr.cb);
        uint8_t* srcCr = static_cast<uint8_t*>(ycbcr.cr);

        // 即使不压缩，我们也按照 UV 分离或者保持 Interleaved 发送
        // 你的 PC 端期望收到 uData 和 vData，所以我们这里还是拆开比较好
        // 或者更简单的：如果 PC 端支持 NV21 原始格式，可以直接拷 Interleaved 数据
        // 为了兼容之前的协议，我们这里还是拆分一下 UV (虽然稍微慢一点点)
        
        for (uint32_t r = 0; r < chromaH; r++) {
            const uint8_t* srcRowCb = srcCb + r * ycbcr.cstride;
            const uint8_t* srcRowCr = srcCr + r * ycbcr.cstride;
            
            uint8_t* dstRowU = &frameData.uData[r * chromaW];
            uint8_t* dstRowV = &frameData.vData[r * chromaW];

            for (uint32_t c = 0; c < chromaW; c++) {
                size_t offset = c * ycbcr.chroma_step;
                dstRowU[c] = srcRowCb[offset];
                dstRowV[c] = srcRowCr[offset];
            }
        }
    }
    
    mapper.unlock(buffer);
    return true;
}





// ===========================================================================
// 5. 写入原始 NV21 数据 (添加 Stride Padding，不放大)
// ===========================================================================
bool CameraStreamTransmitter::writeFrameData(
        buffer_handle_t buffer, uint32_t width, uint32_t height,
        const FrameData& frameData) {
    
    if (frameData.format == 0x21) {
        GraphicBufferMapper& mapper = GraphicBufferMapper::get();
        android::Rect bounds(width, height);
        void* vaddr = nullptr;

        ALOGE("MYJDebug8: writing picture!!!");

        // JPEG 必须用 lock，不能用 lockYCbCr
        // 使用 GRALLOC_USAGE_SW_WRITE_OFTEN
        if (mapper.lock(buffer, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &vaddr) != 0) {
            ALOGE("MYJDebug: Failed to lock JPEG buffer for writing");
            return false;
        }

        // 安全检查：确保我们有数据可写
        if (!frameData.yData.empty()) {
            // 直接将收到的二进制数据（完整的 .jpg 文件数据）覆盖到 Buffer 内存中
            // 假设 Buffer 足够大（通常 HAL 分配的 JPEG Buffer 是按 MaxJpegSize 分配的，很大）
            memcpy(vaddr, frameData.yData.data(), frameData.yData.size());
            
            // [可选] 打印一下写入的大小，方便调试
            // ALOGE("MYJDebug: Overwrote JPEG buffer with %zu bytes", frameData.yData.size());
        } else {
            ALOGE("MYJDebug: Received empty JPEG data from PC!");
        }

        mapper.unlock(buffer);
        return true;
    }
            
    // 校验：必须是原始尺寸
    if (frameData.width != width || frameData.height != height) {
        ALOGE("MYJdebug5:Frame size mismatch! Expected %dx%d, got %dx%d", width, height, frameData.width, frameData.height);
        return false;
    }

    GraphicBufferMapper& mapper = GraphicBufferMapper::get();
    android::Rect bounds(width, height);
    android_ycbcr ycbcr;
    
    if (mapper.lockYCbCr(buffer, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &ycbcr) != 0) {
        return false;
    }
    if (ycbcr.y == nullptr) {
        mapper.unlock(buffer);
        return false;
    }

    // 1. 写入 Y 平面
    uint8_t* dstY = static_cast<uint8_t*>(ycbcr.y);
    const uint8_t* srcY = frameData.yData.data();
    
    for (uint32_t r = 0; r < height; r++) {
        const uint8_t* srcRow = srcY + r * width;
        uint8_t* dstRow = dstY + r * ycbcr.ystride;
        memcpy(dstRow, srcRow, width);
    }

    // 2. 写入 UV 平面
    if (ycbcr.cb && ycbcr.cr && !frameData.uData.empty()) {
        uint8_t* dstCb = static_cast<uint8_t*>(ycbcr.cb);
        uint8_t* dstCr = static_cast<uint8_t*>(ycbcr.cr);
        
        const uint8_t* srcU = frameData.uData.data();
        const uint8_t* srcV = frameData.vData.data();
        
        uint32_t chromaH = height / 2;
        uint32_t chromaW = width / 2;

        for (uint32_t r = 0; r < chromaH; r++) {
            uint8_t* dstRowCb = dstCb + r * ycbcr.cstride;
            uint8_t* dstRowCr = dstCr + r * ycbcr.cstride;
            
            uint8_t* srcRowU = (uint8_t*)srcU + r * chromaW;
            uint8_t* srcRowV = (uint8_t*)srcV + r * chromaW;

            for (uint32_t c = 0; c < chromaW; c++) {
                size_t offset = c * ycbcr.chroma_step;
                dstRowCb[offset] = srcRowU[c];
                dstRowCr[offset] = srcRowV[c];
            }
        }
    }

    mapper.unlock(buffer);
    return true;
}



// ===========================================================================
// 6. 发送线程 (保持 System Call 合并 + MSG_NOSIGNAL)
// ===========================================================================
void CameraStreamTransmitter::sendThreadFunc() {
    while (running_.load()) {
        std::shared_ptr<FrameData> frame;
        {
            std::unique_lock<std::mutex> lock(sendMutex_);
            sendCV_.wait(lock, [&]() {
                return !sendQueue_.empty() || !running_.load();
            });
            if (!running_.load()) break;
            if (sendQueue_.empty()) continue;
            
            frame = sendQueue_.front();
            sendQueue_.pop();
        }
        
        if (connected_.load()) {
            if (!sendFrame(*frame)) {
                ALOGE("MYJdebug5:Send error, closing connection");
                close(sockfd_);
                sockfd_ = -1;
                connected_.store(false);
            }
        }
    }
}

// ===========================================================================
// 7. 接收线程 (保持内存复用)
// ===========================================================================
void CameraStreamTransmitter::receiveThreadFunc() {
    while (running_.load()) {
        if (!connected_.load()) {
            usleep(100000); 
            continue;
        }

        FrameData frame;
        if (receiveFrame(frame)) {
            std::lock_guard<std::mutex> lock(receiveMutex_);
            if (receiveQueue_.size() > MAX_QUEUE_SIZE) {
                receiveQueue_.pop_front();
            }
            receiveQueue_.push_back(std::make_shared<FrameData>(std::move(frame)));
            receiveCV_.notify_all();
        } else {
            if (running_.load()) {
                ALOGE("MYJdebug5:Receive error, closing connection");
                close(sockfd_);
                sockfd_ = -1;
                connected_.store(false);
            }
        }
    }
}

// ===========================================================================
// 8. 协议层 (合并发送 / 内存复用)
// ===========================================================================
bool CameraStreamTransmitter::sendFrame(const FrameData& frame) {
    if (sockfd_ < 0) return false;
    
    FrameHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = MAGIC_NUMBER;
    header.frameNumber = frame.frameNumber;
    header.streamId = frame.streamId;
    header.width = frame.width;
    header.height = frame.height;
    header.format = frame.format;
    header.timestamp = frame.timestamp;
    
    header.yDataSize = frame.yData.size();
    header.uDataSize = frame.uData.size();
    header.vDataSize = frame.vData.size();
    header.yStride = frame.yStride;
    
    size_t totalSize = sizeof(header) + header.yDataSize + header.uDataSize + header.vDataSize;
    
    if (mTxBuffer.size() < totalSize) mTxBuffer.resize(totalSize);
    
    uint8_t* ptr = mTxBuffer.data();
    memcpy(ptr, &header, sizeof(header)); ptr += sizeof(header);
    memcpy(ptr, frame.yData.data(), header.yDataSize); ptr += header.yDataSize;
    if (header.uDataSize > 0) { memcpy(ptr, frame.uData.data(), header.uDataSize); ptr += header.uDataSize; }
    if (header.vDataSize > 0) { memcpy(ptr, frame.vData.data(), header.vDataSize); }
    
    ssize_t sent = send(sockfd_, mTxBuffer.data(), totalSize, MSG_NOSIGNAL);
    return sent == (ssize_t)totalSize;
}

bool CameraStreamTransmitter::receiveFrame(FrameData& frame) {
    if (sockfd_ < 0) return false;
    
    FrameHeader header;
    if (recv(sockfd_, &header, sizeof(header), MSG_WAITALL) != sizeof(header)) return false;
    if (header.magic != MAGIC_NUMBER) return false;
    
    frame.frameNumber = header.frameNumber;
    frame.streamId = header.streamId;
    frame.width = header.width;
    frame.height = header.height;
    frame.format = header.format;

    size_t bodySize = header.yDataSize + header.uDataSize + header.vDataSize;
    if (mRxBuffer.size() < bodySize) mRxBuffer.resize(bodySize);
    
    if (recv(sockfd_, mRxBuffer.data(), bodySize, MSG_WAITALL) != (ssize_t)bodySize) return false;
    
    uint8_t* ptr = mRxBuffer.data();
    frame.yData.assign(ptr, ptr + header.yDataSize); ptr += header.yDataSize;
    if (header.uDataSize > 0) { frame.uData.assign(ptr, ptr + header.uDataSize); ptr += header.uDataSize; }
    if (header.vDataSize > 0) { frame.vData.assign(ptr, ptr + header.vDataSize); }
    
    return true;
}

} // namespace camera3
} // namespace android