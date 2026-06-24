#pragma once
#include <cstdint>

namespace AppCore {
    // این تنها متغیری است که بقیه کدها برای محاسباتشان به آن نیاز دارند
    extern volatile uint64_t g_runtime_unit;

    // این تنها تابعی است که بقیه برنامه صدا می‌زنند
    // با استفاده از این تابع، کل پروسه چک کردن در آن نقطه کپی (Inline) می‌شود
    __attribute__((always_inline)) void sync_context();
}