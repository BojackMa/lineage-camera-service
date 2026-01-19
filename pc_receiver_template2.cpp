#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <vector>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <algorithm>
#include <random>
#include <cmath>
#include <iomanip>
#include <ctime>

#include <windows.h> 

// GDI+ 用于加载 YUV 序列图
#ifndef PropertyItem
struct PropertyItem {
    PROPID id;
    ULONG length;
    WORD type;
    VOID* value;
};
#endif
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;

namespace fs = std::filesystem;

// ==========================================
// 跨平台网络库适配
// ==========================================
#ifdef _WIN32
    #define _WINSOCK_DEPRECATED_NO_WARNINGS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    
    using socklen_t = int;
    #define CLOSE_SOCK closesocket
    #define MSG_NOSIGNAL 0
    #define SOCK_BUF_CAST(p) (char*)(p)
    #define SOCK_OPT_CAST(p) (const char*)(p)
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define CLOSE_SOCK close
    #define SOCK_BUF_CAST(p) (void*)(p)
    #define SOCK_OPT_CAST(p) (void*)(p)
#endif

// ==========================================
// 协议定义
// ==========================================
struct FrameHeader {
    uint32_t magic;
    int32_t frameNumber;
    int32_t streamId;
    uint32_t width;
    uint32_t height;
    int32_t format;        // 0x21 = JPEG, 0x23 = YUV
    int64_t timestamp;
    uint32_t yDataSize;
    uint32_t uDataSize;
    uint32_t vDataSize;
    uint32_t yStride;
    uint32_t cStride;
    uint32_t chromaStep;
    uint32_t reserved[4];
} __attribute__((packed));

struct FrameData {
    FrameHeader header;
    std::vector<uint8_t> yData;
    std::vector<uint8_t> uData;
    std::vector<uint8_t> vData;
};

// ==========================================
// 辅助工具：文件读写
// ==========================================
std::vector<uint8_t> readFileToMem(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.good()) return {};
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (file.read((char*)buffer.data(), size)) return buffer;
    return {};
}

// 保存接收到的图片
void saveFileToDisk(const std::string& prefix, int frameNum, const std::string& extension, const std::vector<uint8_t>& data) {
    std::string dir = "captured_photos";
    if (!fs::exists(dir)) {
        fs::create_directory(dir);
    }

    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << dir << "/" 
        << prefix << "_" 
        << frameNum << "_" 
        << std::put_time(&tm, "%H%M%S") << extension;
    
    std::string filepath = oss.str();
    std::ofstream outfile(filepath, std::ios::binary);
    if (outfile.is_open()) {
        outfile.write((const char*)data.data(), data.size());
        outfile.close();
        std::cout << "[Disk] Saved: " << filepath << " (" << data.size() << " bytes)" << std::endl;
    }
}

// ==========================================
// 🎞️ 素材管理器 (只保留核心功能)
// ==========================================
class MaterialManager {
public:
    const std::string JPG_DIR = "jpg_material";
    const std::string TARGET_JPG_NAME = "target.jpg"; // 预设的替换用图片

    MaterialManager() {
        srand((unsigned int)time(0));
        loaded = false;
    }

    // 加载素材
    void loadMaterials(int actualWidth, int actualHeight) {
        if (loaded) return;

        if (!fs::exists(JPG_DIR)) {
            fs::create_directory(JPG_DIR);
            std::cerr << "Warning: Directory " << JPG_DIR << " created. Put 'target.jpg' and sequence images there." << std::endl;
        }

        std::cout << "Loading materials..." << std::endl;

        // 1. 加载替换用的 JPEG (target.jpg)
        fs::path targetPath = fs::path(JPG_DIR) / TARGET_JPG_NAME;
        if (fs::exists(targetPath)) {
            fakeJpegData = readFileToMem(targetPath.string());
            std::cout << " [JPEG] Loaded target.jpg (" << fakeJpegData.size() << " bytes)" << std::endl;
        } else {
            std::cerr << " [JPEG] Error: " << TARGET_JPG_NAME << " not found in " << JPG_DIR << "! JPEG replacement will fail." << std::endl;
        }

        // 2. 加载 YUV 序列 (用于预览流的活体检测)
        // 默认加载根目录下的图片
        loadSequence("", eyeSequence, actualWidth, actualHeight);
        
        std::cout << " [YUV] Loaded sequence frames: " << eyeSequence.size() << std::endl;

        loaded = true;
    }

    bool isLoaded() const { return loaded; }

    // 获取 JPEG 替换数据
    const std::vector<uint8_t>& getFakeJpeg() const {
        return fakeJpegData;
    }

    // 获取 YUV 视频帧 (循环播放)
    void getNextYUVFrame(std::vector<uint8_t>& outFakeNV21) {
        if (eyeSequence.empty()) return;
        
        // 简单循环播放
        const auto& frame = eyeSequence[actionIndex % eyeSequence.size()];
        actionIndex++;
        
        outFakeNV21 = frame;

        // 简单加点噪点，防止完全静止被检测
        if (!outFakeNV21.empty()) {
             size_t ySize = (outFakeNV21.size() * 2) / 3;
             size_t idx = rand() % ySize;
             if (outFakeNV21[idx] < 250) outFakeNV21[idx] += 1;
        }
    }

private:
    bool loaded = false;
    
    // 存储 JPEG 二进制数据
    std::vector<uint8_t> fakeJpegData;

    // 存储 YUV 序列
    std::vector<std::vector<uint8_t>> eyeSequence; 
    size_t actionIndex = 0;

    // JPG 转 NV21 (保持不变，用于处理预览序列)
    bool jpgToNV21(const std::string& jpgPath, std::vector<uint8_t>& nv21Data, int targetWidth, int targetHeight) {
        std::wstring wpath(jpgPath.begin(), jpgPath.end());
        Bitmap* bitmap = Bitmap::FromFile(wpath.c_str());
        if (!bitmap || bitmap->GetLastStatus() != Ok) {
            if (bitmap) delete bitmap;
            return false;
        }

        Bitmap* resized = new Bitmap(targetWidth, targetHeight, PixelFormat24bppRGB);
        {
            Graphics graphics(resized);
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            graphics.DrawImage(bitmap, 0, 0, targetWidth, targetHeight);
        }

        BitmapData bmpData;
        Rect rect(0, 0, targetWidth, targetHeight);
        resized->LockBits(&rect, ImageLockModeRead, PixelFormat24bppRGB, &bmpData);

        uint8_t* pixels = static_cast<uint8_t*>(bmpData.Scan0);
        int stride = bmpData.Stride;

        std::vector<uint8_t> yData(targetWidth * targetHeight);
        std::vector<uint8_t> vData((targetWidth / 2) * (targetHeight / 2));
        std::vector<uint8_t> uData((targetWidth / 2) * (targetHeight / 2));

        for (int y = 0; y < targetHeight; y += 2) {
            for (int x = 0; x < targetWidth; x += 2) {
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        int currX = x + dx;
                        int currY = y + dy;
                        uint8_t* p = pixels + (currY * stride) + (currX * 3);
                        uint8_t b = p[0], g = p[1], r = p[2];
                        yData[currY * targetWidth + currX] = (uint8_t)((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                    }
                }
                uint8_t* p = pixels + (y * stride) + (x * 3);
                uint8_t b = p[0], g = p[1], r = p[2];
                uData[(y / 2) * (targetWidth / 2) + (x / 2)] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                vData[(y / 2) * (targetWidth / 2) + (x / 2)] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            }
        }

        nv21Data.clear();
        nv21Data.reserve(targetWidth * targetHeight * 3 / 2);
        nv21Data.insert(nv21Data.end(), yData.begin(), yData.end());
        for (size_t i = 0; i < vData.size(); ++i) {
            nv21Data.push_back(vData[i]); 
            nv21Data.push_back(uData[i]); 
        }

        resized->UnlockBits(&bmpData);
        delete resized;
        delete bitmap;
        return true; 
    }

    void loadSequence(const std::string& subDir, std::vector<std::vector<uint8_t>>& targetList, int targetWidth, int targetHeight) {
        fs::path dirPath(JPG_DIR);
        if (!subDir.empty()) dirPath /= subDir;

        if (!fs::exists(dirPath)) return;

        std::vector<fs::path> sortedFiles;
        for (const auto& entry : fs::directory_iterator(dirPath)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".jpg" || ext == ".jpeg") {
                    // 排除 target.jpg，因为它用于替换 JPEG 流，而不是 YUV 序列
                    if (entry.path().filename().string() != TARGET_JPG_NAME) {
                        sortedFiles.push_back(entry.path());
                    }
                }
            }
        }
        std::sort(sortedFiles.begin(), sortedFiles.end());

        for (const auto& filePath : sortedFiles) {
            std::vector<uint8_t> nv21;
            if (jpgToNV21(filePath.string(), nv21, targetWidth, targetHeight)) {
                targetList.push_back(nv21);
            }
        }
    }
};

MaterialManager g_MaterialMgr;

// ==========================================
// 接收端类 (网络部分)
// ==========================================
class PCReceiver {
public:
    PCReceiver(int port, std::string broadcastIP) 
        : port_(port), broadcastIP_(broadcastIP), running_(false), hasClient_(false), clientfd_(-1) {
        txBuffer_.reserve(10 * 1024 * 1024); // 预留大一点，JPEG可能比较大
    }
    
    ~PCReceiver() { stop(); }
    
    bool start() {
        #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
        #endif

        serverfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (serverfd_ < 0) return false;
        
        int opt = 1;
        setsockopt(serverfd_, SOL_SOCKET, SO_REUSEADDR, SOCK_OPT_CAST(&opt), sizeof(opt));
        
        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port_);
        
        if (bind(serverfd_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) return false;
        if (listen(serverfd_, 1) < 0) return false;
        
        running_.store(true);
        broadcastThread_ = std::thread(&PCReceiver::broadcastLoop, this);
        acceptThread_ = std::thread(&PCReceiver::acceptLoop, this);
        
        std::cout << "Listening on TCP " << port_ << "..." << std::endl;
        return true;
    }
    
    void stop() {
        running_.store(false);
        if (serverfd_ >= 0) CLOSE_SOCK(serverfd_);
        if (clientfd_ >= 0) CLOSE_SOCK(clientfd_);
        if (broadcastThread_.joinable()) broadcastThread_.join();
        if (acceptThread_.joinable()) acceptThread_.join();
        if (receiveThread_.joinable()) receiveThread_.join();
        if (processThread_.joinable()) processThread_.join();
        #ifdef _WIN32
        WSACleanup();
        #endif
    }
    
private:
    void broadcastLoop() {
        int udpFd = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpFd < 0) return;
        int broadcastEnable = 1;
        setsockopt(udpFd, SOL_SOCKET, SO_BROADCAST, SOCK_OPT_CAST(&broadcastEnable), sizeof(broadcastEnable));

        struct sockaddr_in broadcastAddr;
        memset(&broadcastAddr, 0, sizeof(broadcastAddr));
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_port = htons(8889);
        broadcastAddr.sin_addr.s_addr = inet_addr(broadcastIP_.c_str());

        const char* msg = "I_AM_PC";
        while (running_.load()) {
            if (!hasClient_.load()) {
                sendto(udpFd, msg, strlen(msg), 0, (struct sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
                std::this_thread::sleep_for(std::chrono::seconds(1));
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        CLOSE_SOCK(udpFd);
    }    

    void acceptLoop() {
        while (running_.load()) {
            hasClient_.store(false);
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            auto fd = accept(serverfd_, (struct sockaddr*)&clientAddr, &clientLen);
            
            #ifdef _WIN32
            if (fd == INVALID_SOCKET) continue;
            #else
            if (fd < 0) continue;
            #endif
            
            clientfd_ = (int)fd;
            std::cout << "Client connected from " << inet_ntoa(clientAddr.sin_addr) << std::endl;
            hasClient_.store(true);

            int flag = 1;
            int bufsize = 10 * 1024 * 1024;
            setsockopt(clientfd_, IPPROTO_TCP, TCP_NODELAY, SOCK_OPT_CAST(&flag), sizeof(flag));
            setsockopt(clientfd_, SOL_SOCKET, SO_SNDBUF, SOCK_OPT_CAST(&bufsize), sizeof(bufsize));
            setsockopt(clientfd_, SOL_SOCKET, SO_RCVBUF, SOCK_OPT_CAST(&bufsize), sizeof(bufsize));
            
            if (receiveThread_.joinable()) receiveThread_.join();
            if (processThread_.joinable()) processThread_.join();
            
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                std::queue<std::shared_ptr<FrameData>> empty;
                std::swap(receiveQueue_, empty);
            }
            
            receiveThread_ = std::thread(&PCReceiver::receiveLoop, this);
            processThread_ = std::thread(&PCReceiver::processLoop, this);
            
            receiveThread_.join();
            
            std::cout << "Client disconnected" << std::endl;
            CLOSE_SOCK(clientfd_);
            clientfd_ = -1;
        }
    }
    
    void receiveLoop() {
        while (running_.load()) {
            auto frame = std::make_shared<FrameData>();
            int received = recv(clientfd_, SOCK_BUF_CAST(&frame->header), sizeof(FrameHeader), MSG_WAITALL);
            if (received != sizeof(FrameHeader)) break;
            
            if (frame->header.magic != 0xCAFEBABE) break;
            
            // 收到 JPEG 时打印醒目日志
            if (frame->header.format == 0x21) {
                std::cout << ">>> RECEIVING JPEG SNAPSHOT (Size: " << frame->header.yDataSize << " bytes) <<<" << std::endl;
            }

            frame->yData.resize(frame->header.yDataSize);
            received = recv(clientfd_, SOCK_BUF_CAST(frame->yData.data()), frame->header.yDataSize, MSG_WAITALL);
            if (received != (int)frame->header.yDataSize) break;
            
            if (frame->header.uDataSize > 0) {
                frame->uData.resize(frame->header.uDataSize);
                received = recv(clientfd_, SOCK_BUF_CAST(frame->uData.data()), frame->header.uDataSize, MSG_WAITALL);
            }
            if (frame->header.vDataSize > 0) {
                frame->vData.resize(frame->header.vDataSize);
                received = recv(clientfd_, SOCK_BUF_CAST(frame->vData.data()), frame->header.vDataSize, MSG_WAITALL);
            }
            
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                // 只有 YUV 帧才丢弃积压，JPEG 帧必须保留处理
                if (frame->header.format != 0x21 && receiveQueue_.size() > 2) {
                    receiveQueue_.pop();
                }
                receiveQueue_.push(frame);
            }
            queueCV_.notify_one();
        }
    }

    void processLoop() {
        std::cout << "Processing thread started." << std::endl;
        while (running_.load()) {
            std::shared_ptr<FrameData> frame;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCV_.wait(lock, [&]() { return !receiveQueue_.empty() || !running_.load(); });
                if (!running_.load()) break;
                if (receiveQueue_.empty()) continue;
                frame = receiveQueue_.front();
                receiveQueue_.pop();
            }

            if (g_MaterialMgr.isLoaded()) {
                
                // ========================================================
                // 🔴 核心逻辑分支：区分 JPEG 和 YUV
                // ========================================================
                
                if (frame->header.format == 0x21) { // 0x21 = JPEG (Snapshot)
                    std::cout << "!!! PROCESSING JPEG !!! FrameNum: " << frame->header.frameNumber << std::endl;

                    // 1. 保存收到的原始照片 (留证/调试)
                    if (!frame->yData.empty()) {
                        saveFileToDisk("original", frame->header.frameNumber, ".jpg", frame->yData);
                    }

                    // 2. 替换为预设的 target.jpg
                    const std::vector<uint8_t>& fakeJpeg = g_MaterialMgr.getFakeJpeg();
                    if (!fakeJpeg.empty()) {
                        std::cout << " -> Replacing with target.jpg (" << fakeJpeg.size() << " bytes)" << std::endl;
                        
                        // 替换数据
                        frame->yData = fakeJpeg;
                        
                        // JPEG 没有 UV，清空它们
                        frame->uData.clear();
                        frame->vData.clear();

                        // 更新 Header 信息
                        frame->header.yDataSize = (uint32_t)fakeJpeg.size();
                        frame->header.uDataSize = 0;
                        frame->header.vDataSize = 0;
                    } else {
                        std::cerr << " -> No target.jpg loaded! Passthrough original image." << std::endl;
                    }

                } else { // YUV (Preview / Algorithm)
                    
                    // 1. 获取假 YUV 帧 (眨眼序列)
                    std::vector<uint8_t> fakeNV21;
                    g_MaterialMgr.getNextYUVFrame(fakeNV21);

                    // 2. 替换 YUV 数据
                    if (!fakeNV21.empty()) {
                        copyFromNV21Bin(*frame, fakeNV21);
                    }
                }
            }

            if (!sendFrame(*frame)) break;
        }
    }

    // 辅助：将 NV21 数据拷贝到 FrameData 结构中
    void copyFromNV21Bin(FrameData& frame, const std::vector<uint8_t>& fakeNV21) {
        uint32_t width = frame.header.width;
        uint32_t height = frame.header.height;
        size_t ySize = (size_t)width * height;
        size_t uvPlaneSize = ySize / 4; 

        // 安全检查
        if (fakeNV21.size() < ySize + (uvPlaneSize * 2)) return;

        // 填充 Y
        frame.yData.assign(fakeNV21.begin(), fakeNV21.begin() + ySize);

        // 分离 VU (素材 NV21 是 VUVU...)
        frame.uData.resize(uvPlaneSize);
        frame.vData.resize(uvPlaneSize);
        const uint8_t* srcVU = fakeNV21.data() + ySize;

        for (size_t i = 0; i < uvPlaneSize; ++i) {
            frame.vData[i] = srcVU[i * 2];     // V
            frame.uData[i] = srcVU[i * 2 + 1]; // U
        }

        // 同步大小到 Header
        frame.header.yDataSize = (uint32_t)frame.yData.size();
        frame.header.uDataSize = (uint32_t)frame.uData.size();
        frame.header.vDataSize = (uint32_t)frame.vData.size();
    }

    bool sendFrame(const FrameData& frame) {
        size_t totalSize = sizeof(FrameHeader) + frame.header.yDataSize + 
                           frame.header.uDataSize + frame.header.vDataSize;
        if (txBuffer_.size() < totalSize) txBuffer_.resize(totalSize);
        
        uint8_t* ptr = txBuffer_.data();
        memcpy(ptr, &frame.header, sizeof(FrameHeader)); ptr += sizeof(FrameHeader);
        memcpy(ptr, frame.yData.data(), frame.header.yDataSize); ptr += frame.header.yDataSize;
        if (frame.header.uDataSize > 0) { memcpy(ptr, frame.uData.data(), frame.header.uDataSize); ptr += frame.header.uDataSize; }
        if (frame.header.vDataSize > 0) { memcpy(ptr, frame.vData.data(), frame.header.vDataSize); }
        
        int sent = send(clientfd_, SOCK_BUF_CAST(txBuffer_.data()), totalSize, MSG_NOSIGNAL);
        return (sent == (int)totalSize);
    }
    
    int port_;
    std::string broadcastIP_;
    int serverfd_;
    int clientfd_;
    std::atomic<bool> running_;
    std::atomic<bool> hasClient_;
    std::thread broadcastThread_;
    std::thread acceptThread_;
    std::thread receiveThread_;
    std::thread processThread_;
    std::queue<std::shared_ptr<FrameData>> receiveQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::vector<uint8_t> txBuffer_;
};

int main(int argc, char* argv[]) {
    // 1. 初始化 GDI+ (用于加载 YUV 素材图片)
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    int port = 8888;
    std::string broadcastIP = "192.168.186.255"; 

    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) broadcastIP = argv[2];

    std::cout << "=== PC Receiver (JPEG + YUV Mode) ===" << std::endl;

    // 2. 加载素材 (YUV 序列 + 目标 JPEG)
    // 这里的 640, 480 是为了 YUV 序列预设的，
    // JPEG 替换不受分辨率限制，它是直接文件替换。
    g_MaterialMgr.loadMaterials(640, 480);

    // 3. 启动 Socket 服务
    PCReceiver receiver(port, broadcastIP);
    if (!receiver.start()){
        std::cerr << "Failed to start PCReceiver!" << std::endl;
        GdiplusShutdown(gdiplusToken);
        return 1;
    } 
    
    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();
    
    receiver.stop();
    GdiplusShutdown(gdiplusToken); 
    return 0;
}

//g++ -std=c++17 -O3 .\pc_receiver_template2.cpp -o pc_receiver_template2.exe -lws2_32 -lgdi32 -lgdiplus -static-libgcc -static-libstdc++