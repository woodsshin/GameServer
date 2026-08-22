#pragma once
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <string>
#include <sstream>
#include <iomanip>

// ============================================================
// 비밀번호 해싱 유틸
// - 실무에서는 bcrypt/argon2 라이브러리를 쓰는 게 정석이지만,
//   포트폴리오 의존성을 최소화하기 위해 OpenSSL SHA-256 + salt +
//   여러 라운드 스트레칭으로 간단히 구현.
// - salt는 계정마다 랜덤 생성해서 DB에 별도 컬럼으로 저장.
// ============================================================
namespace security {

inline std::string ToHex(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}

inline std::string GenerateSalt(size_t byteLen = 16) {
    std::string salt(byteLen, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), static_cast<int>(byteLen));
    return ToHex(reinterpret_cast<const unsigned char*>(salt.data()), byteLen);
}

// salt + password를 여러 라운드 SHA-256 스트레칭
inline std::string HashPassword(const std::string& password, const std::string& saltHex, int rounds = 10000) {
    std::string current = saltHex + password;
    unsigned char digest[SHA256_DIGEST_LENGTH];

    for (int i = 0; i < rounds; ++i) {
        SHA256(reinterpret_cast<const unsigned char*>(current.data()), current.size(), digest);
        current = ToHex(digest, SHA256_DIGEST_LENGTH);
    }
    return current;
}

inline bool VerifyPassword(const std::string& password, const std::string& saltHex,
                            const std::string& expectedHash, int rounds = 10000) {
    return HashPassword(password, saltHex, rounds) == expectedHash;
}

} // namespace security
