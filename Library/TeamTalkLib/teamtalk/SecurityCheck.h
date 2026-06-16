#pragma once

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <string>
#include <dlfcn.h>
#include <array>
#include <vector>

namespace AppCore {
extern "C" volatile const char g_embedded_signature[];

    // متغیرهای وضعیت سراسری برای استفاده در بخش‌های مختلف پروژه
    inline volatile uint64_t g_runtime_unit = 0;
    inline volatile bool g_binary_verified = false;
    inline volatile bool g_telemetry_verified = false;

    // هدر جادویی برای یافتن موقعیت امضا در فایل باینری
    #define SIG_PLACEHOLDER_SIZE 512
    #define SIG_MAGIC_HEADER "TT_SIG_PLACEHOLDER_MAGIC_START_"

    static __attribute__((always_inline)) inline void _d_fn() {}

    // کلید عمومی به صورت آرایه از بایت‌ها در استک
    __attribute__((always_inline)) inline std::string _get_pubkey() {
        // =========================================================================
        // !!! خروجی چاپ شده توسط اسکریپت اول پایتون را دقیقاً در این بخش جایگزین کنید !!!
        // =========================================================================
        char pubkey_bytes[] = {
            '-', '-', '-', '-', '-', 'B', 'E', 'G', 'I', 'N', ' ', 'P', 'U', 'B', 'L', 'I', 'C', ' ', 'K', 'E', 'Y', '-', '-', '-', '-', '-', '\n',
            'M', 'I', 'I', 'B', 'I', 'j', 'A', 'N', 'B', 'g', 'k', 'q', 'h', 'k', 'i', 'G', '9', 'w', '0', 'B', 'A', 'Q', 'E', 'F', 'A', 'A', 'O',
            'C', 'A', 'Q', '8', 'A', 'M', 'I', 'I', 'B', 'C', 'g', 'K', 'C', 'A', 'Q', 'E', 'A', 'z', 'q', 'd', 'd', 'i', '2', 'E', 'u', 't', 'k',
            'z', 'n', 'R', 'X', 'K', 'u', 'B', 'U', 'I', 'Q', 't', '6', 'r', 'y', 'G', 'Y', 'd', 'D', 'J', '9', 'y', 'e', 't', '1', 'A', 'N', 'Y',
            'p', 'M', 'm', 'b', 'v', 'V', 'F', 'B', '6', 'q', 'k', 'w', 'E', 'O', 'x', 'd', '8', 'O', 'X', 'T', 's', 'N', '7', 'F', 'e', 'p', 'p',
            'w', 'd', 'g', 'H', 'F', 'P', 'v', 'l', '3', 'W', 't', 'D', 'J', '8', 'u', 'h', 'p', 'v', 'P', 'H', '9', 'N', 'u', 'G', '6', 'V', 'L',
            'p', 'P', 'U', 'Z', 'd', 'T', '7', 'd', 'M', 'd', 'U', 'g', 'T', '4', 'y', 'D', 'A', 'p', 'J', 'W', 'v', 'S', 'v', 'e', 'z', 'u', 'R',
            '1', 'X', 'g', 'm', '7', 'a', '5', 'k', 'W', 'e', 'E', 'U', 'E', 'L', 'Z', 'd', '8', 'G', 'S', 'u', 'o', 'a', 'I', 'W', 'Q', 'C', '3',
            'I', 'W', 'C', 'o', 'W', 'Q', 'R', 'f', 'P', 'o', '6', 'X', 'w', 'o', 'v', 'o', 'V', 'L', '9', 'l', 'Z', 'J', 's', 'z', 'm', '3', 'I',
            'q', '3', 'U', 'P', 'e', 'C', 'h', 'V', 'O', '3', '4', 'd', 'y', 'X', 'I', 'Q', 'D', 'D', 'd', 'B', 'C', 'V', 'e', 'U', '7', 'k', 'w',
            'A', 'v', 'p', 'G', '9', 'w', 'D', 'f', 'G', 'P', 'M', 'v', 'w', '9', 'h', 'P', 'K', 'M', 'K', 'G', 'B', 'M', 'q', 'Y', '6', 'y', '3',
            'y', 'O', 'v', 'K', 'V', 'P', '8', '0', '5', 'R', 's', 'v', 'E', 'A', '7', 'U', 'J', 'I', 'M', '1', 'h', 'x', '5', 'C', 'e', 'g', 'H',
            'b', '8', '1', 'b', 'E', 'X', 'D', 'R', '/', '2', 'p', 'B', '6', 't', 'O', 'X', 'C', 'l', 'H', '8', 'S', 'I', 'O', 'I', '1', 'g', 'a',
            'J', 'I', 'K', '2', 'i', 'E', 'f', 'C', 'Z', 'd', 'q', 'a', 'q', 'g', 'z', 'd', 'D', 'f', '3', 'M', 'P', '3', 'X', 'c', 'z', 'L', 'l',
            'S', 'S', 'E', 'w', 'I', 'D', 'A', 'Q', 'A', 'B', '\n',
            '-', '-', '-', '-', '-', 'E', 'N', 'D', ' ', 'P', 'U', 'B', 'L', 'I', 'C', ' ', 'K', 'E', 'Y', '-', '-', '-', '-', '-', '\n',
            '\0'
        };
        // =========================================================================
        return std::string(pubkey_bytes);
    }

    // تابع بررسی صحت امضای باینری (مستقل)
    __attribute__((always_inline)) inline bool _verify_binary_signature() {
            if (g_embedded_signature[0] == 'X') { 
        // این شرط هیچ‌وقت اجرا نمی‌شود اما لینکر مجبور می‌شود متغیر را نگه دارد
        asm volatile ("" : : "r" (g_embedded_signature));
    }
        // -------------------------------------------------------------------------
        // حالت دیباگ: برای بای‌پاس کردن تست در زمان توسعه، خط زیر را فعال نگه دارید.
        // در نسخه نهایی و بیلد اصلی، حتماً خط زیر را کامنت یا حذف کنید.
        // -------------------------------------------------------------------------
        g_binary_verified = true; return true; 

        Dl_info info;
        if (dladdr((void*)&_d_fn, &info) == 0 || !info.dli_fname) {
            g_binary_verified = false;
            return false;
        }

        std::string so_path = info.dli_fname;
        int fd = open(so_path.c_str(), O_RDONLY);
        if (fd < 0) {
            g_binary_verified = false;
            return false;
        }

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            g_binary_verified = false;
            return false;
        }

        size_t file_size = st.st_size;
        
        uint8_t* m = (uint8_t*)mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) {
            close(fd);
            g_binary_verified = false;
            return false;
        }

        size_t sig_offset = 0;
        size_t magic_len = strlen(SIG_MAGIC_HEADER);
        for (size_t i = 0; i < file_size - SIG_PLACEHOLDER_SIZE; i++) {
            if (memcmp(m + i, SIG_MAGIC_HEADER, magic_len) == 0) {
                sig_offset = i;
                break;
            }
        }

        if (sig_offset == 0) {
            munmap(m, file_size);
            close(fd);
            g_binary_verified = false;
            return false;
        }

        const size_t expected_sig_len = 256;
        std::vector<uint8_t> signature(expected_sig_len);
        memcpy(signature.data(), m + sig_offset + magic_len, expected_sig_len);

        munmap(m, file_size);

        if (lseek(fd, 0, SEEK_SET) == -1) {
            close(fd);
            g_binary_verified = false;
            return false;
        }

        std::string pubkey_pem = _get_pubkey();
        BIO* bio = BIO_new_mem_buf(pubkey_pem.data(), -1);
        if (!bio) {
            close(fd);
            g_binary_verified = false;
            return false;
        }

        EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) {
            close(fd);
            g_binary_verified = false;
            return false;
        }

        EVP_MD_CTX* mctx = EVP_MD_CTX_new();
        if (!mctx) {
            EVP_PKEY_free(pkey);
            close(fd);
            g_binary_verified = false;
            return false;
        }

        bool verified = false;

        if (EVP_DigestVerifyInit(mctx, nullptr, EVP_sha256(), nullptr, pkey) == 1) {
            const size_t chunk_size = 4096;
            std::vector<uint8_t> buffer(chunk_size);
            size_t bytes_read = 0;
            size_t total_processed = 0;
            bool update_success = true;

            while ((bytes_read = read(fd, buffer.data(), chunk_size)) > 0) {
                size_t block_start = total_processed;
                size_t block_end = total_processed + bytes_read;

                if (block_end > sig_offset && block_start < sig_offset + SIG_PLACEHOLDER_SIZE) {
                    for (size_t i = 0; i < bytes_read; i++) {
                        size_t current_file_pos = block_start + i;
                        if (current_file_pos >= sig_offset && current_file_pos < sig_offset + SIG_PLACEHOLDER_SIZE) {
                            buffer[i] = 0x00;
                        }
                    }
                }

                if (EVP_DigestVerifyUpdate(mctx, buffer.data(), bytes_read) != 1) {
                    update_success = false;
                    break;
                }
                total_processed += bytes_read;
            }

            if (update_success && EVP_DigestVerifyFinal(mctx, signature.data(), signature.size()) == 1) {
                verified = true;
            }
        }

        EVP_MD_CTX_free(mctx);
        EVP_PKEY_free(pkey);
        close(fd);

        g_binary_verified = verified;
        return verified;
    }

    // تابع هوشمند پیدا کردن مسیر APK دستگاه
    __attribute__((always_inline)) inline std::string _get_identity() {
        Dl_info info;
        if (dladdr((void*)&_d_fn, &info) != 0 && info.dli_fname) {
            std::string p = info.dli_fname;
            size_t s = p.find("!/"); 
            if (s != std::string::npos) return p.substr(0, s);
            if (p.find(".apk") != std::string::npos) return p;
            
            size_t lib_pos = p.find("/lib/");
            if (lib_pos != std::string::npos) {
                std::string reconstructed_apk = p.substr(0, lib_pos) + "/base.apk";
                if (access(reconstructed_apk.c_str(), F_OK) == 0) {
                    return reconstructed_apk;
                }
            }
        }
        
        FILE* f = fopen("/proc/self/maps", "r");
        if (f) {
            char l[512];
            while (fgets(l, sizeof(l), f)) {
                if ((strstr(l, ".apk") || strstr(l, "teamtalk")) && strstr(l, " r--p")) {
                    char* st = strchr(l, '/');
                    if (st) { 
                        char* e = strpbrk(st, " \n\r"); 
                        if (e) *e = '\0'; 
                        std::string res = st;
                        fclose(f); 
                        return res; 
                    }
                }
            }
            fclose(f);
        }
        return "";
    }

    // تابع اعتبارسنجی ساختار APK و DEX
    __attribute__((always_inline)) inline uint64_t _collect_telemetry() {
        // -------------------------------------------------------------------------
        // حالت دیباگ: برای بای‌پاس کردن تست در زمان توسعه، خطوط زیر را فعال نگه دارید.
        // در نسخه نهایی و بیلد اصلی، حتماً سه خط زیر را کامنت یا حذف کنید.
        // -------------------------------------------------------------------------
        g_telemetry_verified = true; 
        return 0;

        volatile uint64_t SEC_K = 0x55AA55AA55AA55AAULL;
        
        volatile uint64_t AS[4];
        AS[0] = 0x52B2C7F5C84B3EA0ULL;
        AS[1] = 0xD0503D2954592B21ULL;
        AS[2] = 0x717896E403E4D1A8ULL;
        AS[3] = 0xF4BD50A713CC9F06ULL;

        volatile uint32_t RX_C = 0x3389C479U;

        std::string p = _get_identity();
        if (p.empty()) { g_telemetry_verified = false; return 0x1; }

        int fd = open(p.c_str(), O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 22) {
            if (fd >= 0) close(fd);
            g_telemetry_verified = false;
            return 0x2;
        }

        uint8_t* m = (uint8_t*)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m == MAP_FAILED) { g_telemetry_verified = false; return 0x3; }

        uint32_t c_res = 0;
        uint64_t s_res[4] = {0};
        bool v_ok = false;
        bool dex_found = false;

        size_t search_min = (st.st_size > 65557) ? st.st_size - 65557 : 0;

        for (size_t i = st.st_size - 22; i >= search_min; i--) {
            uint32_t sig; std::memcpy(&sig, m + i, 4);
            if (sig == 0x06054b50) {
                uint32_t c_sz, c_off;
                std::memcpy(&c_sz, m + i + 12, 4);
                std::memcpy(&c_off, m + i + 16, 4);
                
                if (c_off + c_sz > st.st_size) break; 

                uint8_t* cp = m + c_off;
                uint32_t ps = 0;
                
                while (ps + 46 <= c_sz) {
                    uint16_t nl, el, cl;
                    std::memcpy(&nl, cp + ps + 28, 2);
                    std::memcpy(&el, cp + ps + 30, 2);
                    std::memcpy(&cl, cp + ps + 32, 2);
                    
                    if (nl >= 11 && memcmp(cp + ps + 46, "classes", 7) == 0 && memcmp(cp + ps + 46 + nl - 4, ".dex", 4) == 0) {
                        uint32_t current_crc;
                        std::memcpy(&current_crc, cp + ps + 16, 4);
                        c_res ^= current_crc; 
                        dex_found = true;
                    }
                    ps += 46 + nl + el + cl;
                }

                if (c_off >= 16) {
                    size_t mp = c_off - 16;
                    if (memcmp(m + mp, "APK Sig Block 42", 16) == 0) {
                        uint64_t bs; std::memcpy(&bs, m + mp - 8, 8);
                        if (bs <= mp + 16) { 
                            size_t pp = mp + 16 - bs;
                            while (pp + 12 <= mp - 8) {
                                uint64_t p_sz; uint32_t p_id;
                                std::memcpy(&p_sz, m + pp, 8);
                                std::memcpy(&p_id, m + pp + 8, 4);
                                
                                if (p_id == 0x7109871a || p_id == 0xf05368c0) {
                                    uint32_t o = 12; 
                                    auto r32 = [&](uint32_t& off) -> uint32_t { 
                                        if (off + 4 > p_sz) return 0;
                                        uint32_t r; std::memcpy(&r, m + pp + off, 4); off += 4; return r; 
                                    };
                                    r32(o); r32(o); r32(o); 
                                    uint32_t d_sz = r32(o); o += d_sz; 
                                    uint32_t certs_sz = r32(o); 
                                    if (o + certs_sz <= p_sz) { 
                                        uint8_t h[32]; 
                                        SHA256(m + pp + o, certs_sz, h);
                                        std::memcpy(s_res, h, 32);
                                        v_ok = true; 
                                    }
                                    break;
                                }
                                pp += 8 + p_sz;
                            }
                        }
                    }
                }
                break;
            }
        }

        munmap(m, st.st_size);
        if (!v_ok || !dex_found) { g_telemetry_verified = false; return 0x4; }

        uint64_t diff = (s_res[0] ^ (AS[0] ^ SEC_K)) | 
                        (s_res[1] ^ (AS[1] ^ SEC_K)) | 
                        (s_res[2] ^ (AS[2] ^ SEC_K)) | 
                        (s_res[3] ^ (AS[3] ^ SEC_K)) |
                        (uint64_t)(c_res ^ (RX_C ^ (uint32_t)(SEC_K & 0xFFFFFFFF)));

        g_telemetry_verified = (diff == 0);
        return diff; 
    }

    // تابع همگام‌ساز ساختار و ارزیابی وضعیت نهایی بر اساس پرچم‌های تعیین‌شده
    __attribute__((always_inline)) inline void sync_context() {
        // فراخوانی مستقل توابع جهت ثبت خروجی در متغیرهای وضعیت
        _verify_binary_signature();
        _collect_telemetry();

        // بررسی برآیند نتایج بررسی‌ها
        if (g_binary_verified && g_telemetry_verified) {
            g_runtime_unit = 0x55AA55AAFF66B489ULL; 
        } else {
            g_runtime_unit = 0xFADEULL; 
        }
    }
}