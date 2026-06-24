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
            "savefile",
            "pushinternalaudio",
            "feedtoinsertaudioblock",
            "startinternalvideotransmission",
            "stopinternalvideotransmission",
            "pushinternalvideo",
            "feedtoinsertvideoframe"
        ]

    def is_target_function(self, func: omvll.Function) -> bool:
        """بررسی نام تابع با ایمنی کامل در برابر توابع بدون نام"""
        # اگر تابع یا نام تابع وجود نداشت، مبهم‌سازی اختصاصی انجام نده
        if not func or not hasattr(func, 'name') or func.name is None:
            return False
            
        func_name = func.name.lower()
        return any(target in func_name for target in self.target_functions)

    def is_logic_code(self, mod: omvll.Module):
        # بررسی ایمنی برای ماژول
        if not mod or not hasattr(mod, 'name') or mod.name is None:
            return False
            
        path = mod.name.lower()
        # ۱. پوشه های هدف
        if "library/teamtalklib" in path or "library/teamtalkjni" in path:
            # ۲. لیست سیاه
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
        if not mod or not hasattr(mod, 'name') or mod.name is None:
            return False
            
        path = mod.name.lower()
        heavy_files = ["clientnode.cpp", "clientuser.cpp", "teamtalk.cpp", "servernode.cpp", "serverchannel.cpp"]
        return any(f in path for f in heavy_files)

    def obfuscate_arithmetic(self, mod, func):
        if self.is_target_function(func):
            return True
        return True if self.is_logic_code(mod) else False

    def flatten_cfg(self, mod, func):
        if self.is_target_function(func):
            return True
        if self.is_too_heavy(mod):
            return False
        return True if self.is_logic_code(mod) else False

    def obfuscate_string(self, mod, func, string: bytes):
        return omvll.StringEncOptGlobal()

    def basic_block_duplicate(self, mod, func):
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