import omvll
from functools import lru_cache

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()

    # تابع تشخیص هوشمند مسیر فایل
    def is_logic_code(self, mod: omvll.Module):
        path = mod.name.lower()
        # فقط فایل‌هایی که در پوشه‌های اصلی کد ما هستند محافظت شوند
        if "library/teamtalklib" in path or "library/teamtalkjni" in path:
            return True
        return False

    # ۱. به‌هم‌ریختن محاسبات ریاضی (Arithmetic) - ۱۰۰٪ برای کدهای خودمان
    def obfuscate_arithmetic(self, mod: omvll.Module, func: omvll.Function):
        return True if self.is_logic_code(mod) else False

    # ۲. اسپاگتی کردن جریان کد (Control Flow Flattening) - ۱۰۰٪ برای کدهای خودمان
    def flatten_cfg(self, mod: omvll.Module, func: omvll.Function):
        return True if self.is_logic_code(mod) else False

    # ۳. رمزنگاری رشته‌ها (String Encoding) - برای کل پروژه (امن و موثر)
    def obfuscate_string(self, mod: omvll.Module, func: omvll.Function, string: bytes):
        return omvll.StringEncOptGlobal()

    # ۴. تکرار بلاک‌های کد (Basic Block Duplicate) - شدت ۵۰٪
    def basic_block_duplicate(self, mod: omvll.Module, func: omvll.Function):
        if self.is_logic_code(mod):
            return omvll.BasicBlockDuplicateWithProbability(50)
        return omvll.BasicBlockDuplicateWithProbability(0)

    # ۵. فراخوانی غیرمستقیم توابع (Indirect Calls) - شدت ۵۰٪
    def indirect_call(self, mod: omvll.Module, func: omvll.Function):
        if self.is_logic_code(mod):
            return omvll.ObfuscationConfig.default_config(self, mod, func, [], [], [], 50)
        return None # در این متد None برای لایه میانی امن است

    # ۶. به‌هم‌ریختن ترتیب بلاک‌ها (Break Control Flow) - شدت ۵۰٪
    def break_control_flow(self, mod: omvll.Module, func: omvll.Function):
        if self.is_logic_code(mod):
            return omvll.ObfuscationConfig.default_config(self, mod, func, [], [], [], 50)
        return None

    # ۷. غیرفعال کردن عامل اصلی خطای Duplicate Symbol با احتمال ۰
    def function_outline(self, mod: omvll.Module, func: omvll.Function):
        return omvll.FunctionOutlineWithProbability(0)

# ورودی *args باعث می‌شود در تمام نسخه‌ها بدون خطا اجرا شود
@lru_cache(maxsize=1)
def omvll_get_config(*args, **kwargs):
    return MyConfig()