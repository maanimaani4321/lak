#pragma once
#include <cstdint>

namespace AppCore {
    extern volatile uint64_t g_runtime_unit;

    // تابع را به صورت معمولی تعریف می‌کنیم
    void sync_context();
}