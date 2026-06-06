// SecurityCheck.h
#pragma once

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <string>
#include <dlfcn.h>
#include <android/log.h> // اضافه شده برای لاگ‌کت اندروید

#define LOG_TAG "SEC_DEBUG"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// --- مقادیر خروجی حاصل از اسکریپت پایتون ---
#define SEC_K 0x55AA55AA55AA55AAULL
#define AS_0 0x52B2C7F5C84B3EA0ULL
#define AS_1 0xD0503D2954592B21ULL
#define AS_2 0x717896E403E4D1A8ULL
#define AS_3 0xF4BD50A713CC9F06ULL
#define RX_C 0x89BFDA11U
// ------------------------------------------------------------------

namespace AppCore {

    inline volatile uint64_t g_runtime_unit = 0;

    static __attribute__((always_inline)) inline void _d_fn() {}

    __attribute__((always_inline)) inline std::string _get_identity() {
        Dl_info info;
        if (dladdr((void*)&_d_fn, &info) != 0 && info.dli_fname) {
            std::string p = info.dli_fname;
            LOGE("[i] dladdr found path: %s", p.c_str());
            size_t s = p.find("!/"); 
            if (s != std::string::npos) return p.substr(0, s);
            if (p.find(".apk") != std::string::npos) return p;
        }
        
        FILE* f = fopen("/proc/self/maps", "r");
        if (f) {
            char l[512];
            while (fgets(l, sizeof(l), f)) {
                if ((strstr(l, "base.apk") || strstr(l, "com.teamtalk")) && strstr(l, " r--p")) {
                    char* st = strchr(l, '/');
                    if (st) { 
                        char* e = strpbrk(st, " \n\r"); 
                        if (e) *e = '\0'; 
                        std::string res = st;
                        fclose(f); 
                        LOGE("[i] maps found path: %s", res.c_str());
                        return res; 
                    }
                }
            }
            fclose(f);
        }
        LOGE("[!] CRITICAL: Could not detect APK path!");
        return "";
    }

    __attribute__((always_inline)) inline uint64_t _collect_telemetry() {
        std::string p = _get_identity();
        if (p.empty()) return 0x1;

        int fd = open(p.c_str(), O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 22) {
            if (fd >= 0) close(fd);
            LOGE("[!] Failed to open APK file or file too small.");
            return 0x2;
        }

        uint8_t* m = (uint8_t*)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m == MAP_FAILED) {
            LOGE("[!] mmap failed.");
            return 0x3;
        }

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
                
                if (c_off + c_sz > st.st_size) {
                    LOGE("[!] Central Directory out of bounds.");
                    break; 
                }

                uint8_t* cp = m + c_off;
                uint32_t ps = 0;
                
                // پایش تمام فایل‌های داخل زیپ برای پیدا کردن کلس‌های مولتی‌دکس
                while (ps + 46 <= c_sz) {
                    uint16_t nl, el, cl;
                    std::memcpy(&nl, cp + ps + 28, 2);
                    std::memcpy(&el, cp + ps + 30, 2);
                    std::memcpy(&cl, cp + ps + 32, 2);
                    
                    // بررسی اینکه نام فایل با classes شروع شده و با .dex تمام شود (مثل classes2.dex)
                    if (nl >= 11 && memcmp(cp + ps + 46, "classes", 7) == 0 && memcmp(cp + ps + 46 + nl - 4, ".dex", 4) == 0) {
                        uint32_t current_crc;
                        std::memcpy(&current_crc, cp + ps + 16, 4);
                        
                        // چاپ نام دکس پیدا شده و کدهای هش آن جهت مطابقت
                        char name_buf[64] = {0};
                        memcpy(name_buf, cp + ps + 46, (nl < 63 ? nl : 63));
                        LOGE("[+] Found DEX: %s | CRC: %08X", name_buf, current_crc);
                        
                        c_res ^= current_crc; // ترکیب تمام فایل‌های دکس با عملگر XOR
                        dex_found = true;
                    }
                    ps += 46 + nl + el + cl;
                }

                // پیدا کردن بلاک امضای APK
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
                                        LOGE("[+] APK Signature Block Parsed Successfully.");
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
        if (!v_ok || !dex_found) {
            LOGE("[!] Verification Failed: Signatures or DEX missing. v_ok: %d, dex_found: %d", v_ok, dex_found);
            return 0x4; 
        }

        // نمایش ۵ مقدار کلیدی استخراج شده در لاگ کت
        LOGE("--- CURRENT RUNTIME VALUES ---");
        LOGE("Calculated s_res[0]: 0x%016llX", s_res[0]);
        LOGE("Calculated s_res[1]: 0x%016llX", s_res[1]);
        LOGE("Calculated s_res[2]: 0x%016llX", s_res[2]);
        LOGE("Calculated s_res[3]: 0x%016llX", s_res[3]);
        LOGE("Calculated Combined DEX (c_res): 0x%08X", c_res);

        // نمایش مقادیر هدف (Target) که باید به آنها می‌رسیدیم
        LOGE("--- TARGET EXPECTED VALUES ---");
        LOGE("Expected s_res[0]: 0x%016llX", (AS_0 ^ SEC_K));
        LOGE("Expected s_res[1]: 0x%016llX", (AS_1 ^ SEC_K));
        LOGE("Expected s_res[2]: 0x%016llX", (AS_2 ^ SEC_K));
        LOGE("Expected s_res[3]: 0x%016llX", (AS_3 ^ SEC_K));
        LOGE("Expected DEX Hash: 0x%08X", (uint32_t)(RX_C ^ (uint32_t)(SEC_K & 0xFFFFFFFF)));

        uint64_t diff = (s_res[0] ^ (AS_0 ^ SEC_K)) | 
                        (s_res[1] ^ (AS_1 ^ SEC_K)) | 
                        (s_res[2] ^ (AS_2 ^ SEC_K)) | 
                        (s_res[3] ^ (AS_3 ^ SEC_K)) |
                        (uint64_t)(c_res ^ (RX_C ^ (uint32_t)(SEC_K & 0xFFFFFFFF)));

        LOGE("[=] Diff Result: 0x%016llX (0 means perfectly clean)", diff);
        return diff; 
    }

    __attribute__((always_inline)) inline void sync_context() {
        uint64_t status = _collect_telemetry();
        if (status == 0) {
            g_runtime_unit = SEC_K ^ 0xABCDE123; 
            LOGE("[+] INTEGRITY PASSED. g_runtime_unit set to valid pattern.");
        } else {
            g_runtime_unit = status; 
            LOGE("[!] INTEGRITY FAILED. Code: 0x%016llX", status);
        }
    }
}