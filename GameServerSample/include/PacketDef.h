#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

// ============================================================
// 패킷 프로토콜 정의
// [4 byte: PacketSize][2 byte: PacketType][Body...]
// 모든 정수는 리틀엔디안(x86 기준) 그대로 사용 (포트폴리오 단순화)
// ============================================================

namespace proto {

constexpr size_t HEADER_SIZE = sizeof(uint32_t) + sizeof(uint16_t);
constexpr size_t MAX_PACKET_SIZE = 8192;

enum class PacketType : uint16_t {
    // 인증
    C2S_REGISTER = 1001,   // 계정 생성 요청
    S2C_REGISTER_RESULT = 1002,
    C2S_LOGIN = 1003,      // 로그인 요청
    S2C_LOGIN_RESULT = 1004,

    // 채팅
    C2S_CHAT_MESSAGE = 2001,   // 클라이언트 -> 서버 채팅 전송
    S2C_CHAT_BROADCAST = 2002, // 서버 -> 전체(혹은 채널) 브로드캐스트

    // 하트비트 / 접속 관리
    C2S_HEARTBEAT = 3001,
    S2C_HEARTBEAT_ACK = 3002,
    S2C_FORCE_DISCONNECT = 3003,
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t totalSize;   // 헤더 포함 전체 패킷 크기
    uint16_t type;        // PacketType
};
#pragma pack(pop)

// ------------------------------------------------------------
// 직렬화 헬퍼: 가변 길이 문자열을 담기 위해 간단한 바이너리 writer/reader 사용
// ------------------------------------------------------------
class PacketWriter {
public:
    explicit PacketWriter(PacketType type) {
        buffer_.resize(HEADER_SIZE);
        auto* header = reinterpret_cast<PacketHeader*>(buffer_.data());
        header->type = static_cast<uint16_t>(type);
    }

    void WriteInt32(int32_t v) { WriteRaw(&v, sizeof(v)); }
    void WriteUInt64(uint64_t v) { WriteRaw(&v, sizeof(v)); }

    void WriteString(const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        WriteRaw(&len, sizeof(len));
        WriteRaw(s.data(), len);
    }

    // 최종 버퍼 완성 (헤더의 totalSize 채움)
    std::vector<char> Finalize() {
        auto* header = reinterpret_cast<PacketHeader*>(buffer_.data());
        header->totalSize = static_cast<uint32_t>(buffer_.size());
        return buffer_;
    }

private:
    void WriteRaw(const void* data, size_t len) {
        size_t offset = buffer_.size();
        buffer_.resize(offset + len);
        std::memcpy(buffer_.data() + offset, data, len);
    }
    std::vector<char> buffer_;
};

// body(헤더 이후 데이터)만을 대상으로 읽는 리더.
// NetServer가 콜백에 헤더를 뺀 body 포인터/크기를 넘겨주므로 그대로 사용.
class PacketReader {
public:
    PacketReader(const char* body, size_t bodySize)
        : data_(body), size_(bodySize), offset_(0) {}

    int32_t ReadInt32() {
        int32_t v = 0;
        ReadRaw(&v, sizeof(v));
        return v;
    }

    uint64_t ReadUInt64() {
        uint64_t v = 0;
        ReadRaw(&v, sizeof(v));
        return v;
    }

    std::string ReadString() {
        uint16_t len = 0;
        ReadRaw(&len, sizeof(len));
        if (offset_ + len > size_) {
            throw std::runtime_error("PacketReader: 버퍼 범위 초과");
        }
        std::string s(data_ + offset_, len);
        offset_ += len;
        return s;
    }

private:
    void ReadRaw(void* out, size_t len) {
        if (offset_ + len > size_) {
            throw std::runtime_error("PacketReader: 버퍼 범위 초과");
        }
        std::memcpy(out, data_ + offset_, len);
        offset_ += len;
    }

    const char* data_;
    size_t size_;
    size_t offset_;
};

} // namespace proto
