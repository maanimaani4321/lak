import omvll
from functools import lru_cache

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()
        # لیست توابعی که باید به شدت مبهم‌سازی شوند
        self.target_functions = [
            "connect",
            "initsoundinputdevice",
            "setvoicegainlevel",
            "processaudioframe",
            "savefile"  # این تابع صراحتاً هدف قرار گرفته است
        ]

    def is_target_function(self, func: omvll.Function) -> bool:
        """بررسی دقیق نام تابع بدون حساسیت به حروف بزرگ و کوچک"""
        func_name = func.name.lower()
        return any(target in func_name for target in self.target_functions)

    def is_logic_code(self, mod: omvll.Module):
        path = mod.name.lower()
        # ۱. پوشه های هدف
        if "library/teamtalklib" in path or "library/teamtalkjni" in path:
            # ۲. لیست سیاه (audiomuxer برگشت سر جایش تا فایل کلاً دست نخورده بماند)
            black_list = [
                "packetlayout", "packethelper", "audiocontainer", 
                "streamhandler", "audiomuxer", "mystd", "oggfileio",
                "servernode", "serveruser", "build/"
            ]
            if any(f in path for f in black_list):
                return False
            return True
        return False

    def is_too_heavy(self, mod: omvll.Module):
        path = mod.name.lower()
        heavy_files = ["clientnode.cpp", "clientuser.cpp", "teamtalk.cpp", "servernode.cpp", "serverchannel.cpp"]
        return any(f in path for f in heavy_files)

    def obfuscate_arithmetic(self, mod, func):
        # اولویت اول: اگر تابع هدف ما بود (حتی در فایل لیست سیاه)، حتماً مبهم‌سازی کن
        if self.is_target_function(func):
            return True
        return True if self.is_logic_code(mod) else False

    def flatten_cfg(self, mod, func):
        # اولویت اول: اگر تابع هدف ما بود، بدون توجه به لیست سیاه یا سنگین بودن فایل، فلت کن
        if self.is_target_function(func):
            return True
        if self.is_too_heavy(mod):
            return False
        return True if self.is_logic_code(mod) else False

    def obfuscate_string(self, mod, func, string: bytes):
        return omvll.StringEncOptGlobal()

    def basic_block_duplicate(self, mod, func):
        # اولویت اول: برای توابع هدف، شدت را بالا ببر
        if self.is_target_function(func):
            return omvll.BasicBlockDuplicateWithProbability(50)
        if self.is_logic_code(mod) and not self.is_too_heavy(mod):
            return omvll.BasicBlockDuplicateWithProbability(30)
        return omvll.BasicBlockDuplicateWithProbability(0)

    def function_outline(self, mod, func):
        if self.is_target_function(func):
            return omvll.FunctionOutlineWithProbability(25)
        return omvll.FunctionOutlineWithProbability(0)

@lru_cache(maxsize=1)
def omvll_get_config(module=None):
    return MyConfig()