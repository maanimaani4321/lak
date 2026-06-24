import omvll
from functools import lru_cache

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()
        # تفکیک توابع حساس عملکردی از توابع غیرحساس
        self.performance_critical_funcs = [
            "processaudioframe",
            "feedtoinsertaudioblock",
            "pushinternalaudio",
            "pushinternalvideo",
            "feedtoinsertvideoframe"
        ]
        self.security_critical_funcs = [
            "verify_binary_signature",
            "collect_telemetry", 
            "get_pubkey",
            "sync_context",
            "_anti_debug_ptrace",
            "_detect_frida_agent",
            "_is_being_traced",
            "connect",
            "doping",
            "initsoundinputdevice",
            "setvoicegainlevel",
            "savefile",
            "startinternalvideotransmission",
            "stopinternalvideotransmission"
        ]

    def is_perf_critical(self, func: omvll.Function) -> bool:
        if not func or not hasattr(func, 'name') or func.name is None:
            return False
        name = func.name.lower()
        return any(target in name for target in self.performance_critical_funcs)

    def is_security_critical(self, func: omvll.Function) -> bool:
        if not func or not hasattr(func, 'name') or func.name is None:
            return False
        name = func.name.lower()
        return any(target in name for target in self.security_critical_funcs)

    def is_logic_code(self, mod: omvll.Module):
        if not mod or not hasattr(mod, 'name') or mod.name is None:
            return False
        path = mod.name.lower()
        if "library/teamtalklib" in path or "library/teamtalkjni" in path:
            black_list = [
                "packetlayout", "packethelper", "audiocontainer", 
                "streamhandler", "audiomuxer", "mystd", "oggfileio", "signaturespace",
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
        return True if self.is_logic_code(mod) else False

    def flatten_cfg(self, mod, func):
        if self.is_perf_critical(func):
            return False
        if self.is_security_critical(func):
            return True
        if self.is_too_heavy(mod):
            return False
        return True if self.is_logic_code(mod) else False

    def obfuscate_string(self, mod, func, string: bytes):
        if mod and hasattr(mod, 'name') and mod.name is not None:
            path = mod.name.lower()
            if "signaturespace" in path:
                return None
        if self.is_security_critical(func):
            return omvll.StringEncOptLocal()
        return omvll.StringEncOptGlobal()

    def basic_block_duplicate(self, mod, func):
        if self.is_perf_critical(func):
            return omvll.BasicBlockDuplicateWithProbability(10) # کاهش برای حفظ سرعت پردازش مدیا
        if self.is_security_critical(func):
            return omvll.BasicBlockDuplicateWithProbability(60)
        if self.is_logic_code(mod) and not self.is_too_heavy(mod):
            return omvll.BasicBlockDuplicateWithProbability(30)
        return omvll.BasicBlockDuplicateWithProbability(0)

    def function_outline(self, mod, func):
        if self.is_security_critical(func):
            return omvll.FunctionOutlineWithProbability(30)
        return omvll.FunctionOutlineWithProbability(0)

@lru_cache(maxsize=1)
def omvll_get_config(module=None):
    return MyConfig()