import omvll
from functools import lru_cache

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()

    def obfuscate_arithmetic(self, mod: omvll.Module, fun: omvll.Function):
        return True

    def flatten_cfg(self, mod: omvll.Module, func: omvll.Function):
        return True

    def obfuscate_string(self, mod, func, string: bytes):
        return omvll.StringEncOptGlobal()

    def indirect_call(self, mod: omvll.Module, func: omvll.Function):
        return omvll.ObfuscationConfig.default_config(self, mod, func, [], [], [], 10)

    def break_control_flow(self, mod: omvll.Module, func: omvll.Function):
        return omvll.ObfuscationConfig.default_config(self, mod, func, [], [], [], 10)

    def function_outline(self, _, __):
        return False
        #return omvll.FunctionOutlineWithProbability(10)

    def basic_block_duplicate(self, _, __):
        return omvll.BasicBlockDuplicateWithProbability(10)

@lru_cache(maxsize=1)
def omvll_get_config():
    return MyConfig()