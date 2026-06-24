import omvll
from functools import lru_cache

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()

    def is_logic_code(self, mod: omvll.Module):
        path = mod.name.lower()
        # ۱. پوشه های هدف
        if "library/teamtalklib" in path or "library/teamtalkjni" in path:
            # ۲. لیست سیاه (کلاً بیخیال اینها شو)
            black_list = [
                "packetlayout", "packethelper", "audiocontainer", 
                "streamhandler", "audiomuxer", "mystd", "oggfileio",
                "build/"
            ]
            if any(f in path for f in black_list):
                return False
            return True
        return False

    def is_too_heavy(self, mod: omvll.Module):
        path = mod.name.lower()
        # این فایل‌ها بسیار بزرگ هستند و باعث کرش Clang می‌شوند
        heavy_files = ["clientnode.cpp", "clientuser.cpp", "teamtalk.cpp"]
        return any(f in path for f in heavy_files)

    def obfuscate_arithmetic(self, mod, func):
        return True if self.is_logic_code(mod) else False

    def flatten_cfg(self, mod, func):
        # برای فایل‌های سنگین Flattening را غیرفعال می‌کنیم تا کرش نکند
        if self.is_too_heavy(mod):
            return False
        return True if self.is_logic_code(mod) else False

    def obfuscate_string(self, mod, func, string: bytes):
        # رمزنگاری رشته‌ها همیشه و همه جا (بسیار امن و سبک)
        return omvll.StringEncOptGlobal()

    def basic_block_duplicate(self, mod, func):
        if self.is_logic_code(mod) and not self.is_too_heavy(mod):
            return omvll.BasicBlockDuplicateWithProbability(30)
        return omvll.BasicBlockDuplicateWithProbability(0)

    def function_outline(self, mod, func):
        return omvll.FunctionOutlineWithProbability(0)

@lru_cache(maxsize=1)
def omvll_get_config(module=None):
    return MyConfig()