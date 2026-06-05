import omvll
from functools import lru_cache

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()

    def is_logic_code(self, mod: omvll.Module):
        path = mod.name.lower()
        # فقط پوشه Library و JNI را هدف قرار می‌دهیم
        if "library/teamtalklib" in path or "library/teamtalkjni" in path:
            # سوپاپ اطمینان: اگر فایلی باعث کرش می‌شود، نامش را اینجا استثنا کنید
            black_list = [
                "packetlayout", 
                "packethelper", 
                "audiocontainer", 
                "streamhandler", 
                "audiomuxer",
                "build/"
            ]
            if any(f in path for f in black_list):
                return False
            return True
        return False

    def obfuscate_arithmetic(self, mod, func):
        return True if self.is_logic_code(mod) else False

    def flatten_cfg(self, mod, func):
        return True if self.is_logic_code(mod) else False

    def obfuscate_string(self, mod, func, string: bytes):
        # رمزنگاری رشته‌ها سبک است، برای همه فعال می‌ماند
        return omvll.StringEncOptGlobal()

    def basic_block_duplicate(self, mod, func):
        if self.is_logic_code(mod):
            # شدت ۳۰٪ برای پایداری بیشتر
            return omvll.BasicBlockDuplicateWithProbability(30)
        return omvll.BasicBlockDuplicateWithProbability(0)

    def indirect_call(self, mod, func):
        if self.is_logic_code(mod):
            return omvll.ObfuscationConfig.default_config(self, mod, func, [], [], [], 20)
        return None

    def break_control_flow(self, mod, func):
        if self.is_logic_code(mod):
            return omvll.ObfuscationConfig.default_config(self, mod, func, [], [], [], 20)
        return None

    def function_outline(self, mod, func):
        # عامل اصلی خطای Duplicate Symbol حتماً باید 0 باشد
        return omvll.FunctionOutlineWithProbability(0)

@lru_cache(maxsize=1)
def omvll_get_config(*args, **kwargs):
    return MyConfig()