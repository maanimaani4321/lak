import omvll
from functools import lru_cache

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()

    # پیچیده کردن محاسبات ریاضی
    def obfuscate_arithmetic(self, mod: omvll.Module, fun: omvll.Function):
        return True

    # اسپاگتی کردن جریان کد
    def flatten_cfg(self, mod: omvll.Module, func: omvll.Function):
        return True

    # رمزنگاری رشته‌ها
    def obfuscate_string(self, mod: omvll.Module, func: omvll.Function, string: bytes):
        return omvll.StringEncOptGlobal()

    # فراخوانی غیرمستقیم توابع
    def indirect_call(self, mod: omvll.Module, func: omvll.Function):
        return omvll.ObfuscationConfig.default_config(self, mod, func, [], [], [], 10)

    # کنترل جریان برنامه
    def break_control_flow(self, mod: omvll.Module, func: omvll.Function):
        return omvll.ObfuscationConfig.default_config(self, mod, func, [], [], [], 10)

    # غیرفعال کردن عامل خطای Duplicate Symbol با احتمال 0
    def function_outline(self, mod: omvll.Module, func: omvll.Function):
        return omvll.FunctionOutlineWithProbability(0)

    # کپی کردن بلاک‌های کد با احتمال پایین برای پایداری بیشتر
    def basic_block_duplicate(self, mod: omvll.Module, func: omvll.Function):
        return omvll.BasicBlockDuplicateWithProbability(5)

# استفاده از *args برای جلوگیری از خطای TypeError در هر شرایطی
@lru_cache(maxsize=1)
def omvll_get_config():
    return MyConfig()