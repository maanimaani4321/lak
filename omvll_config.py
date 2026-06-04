import omvll

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()

    # رمزنگاری رشته‌ها (بسیار عالی برای مخفی کردن لایسنس و نام کلاس‌ها)
    def obfuscate_string(self, mod, func, string: bytes):
        return omvll.StringEncOptGlobal()

    # اسپاگتی کردن جریان کد (عالی برای از کار انداختن IDA Pro)
    def flatten_cfg(self, mod, func):
        return True

    # پیچیده کردن محاسبات ریاضی
    def obfuscate_arithmetic(self, mod, fun):
        return True

    # این سه مورد زیر معمولاً باعث خطای Duplicate Symbol می‌شوند، پس غیرفعال می‌کنیم
    def function_outline(self, mod, func):
        return False 

    def basic_block_duplicate(self, mod, func):
        return False

    def indirect_call(self, mod, func):
        return False

def omvll_get_config(module=None):
    return MyConfig()