import ctypes
import os
import platform
    
class StartData(ctypes.Structure):
    _fields_ = [
        ("battlePtr", ctypes.c_void_p),
        ("errorPlayer", ctypes.c_int),
        ("errorType", ctypes.c_int),
    ]

class SerialData(ctypes.Structure):
    _fields_ = [
        ("json", ctypes.c_char_p),
        ("data", ctypes.POINTER(ctypes.c_ubyte)),
        ("count", ctypes.c_int),
        ("selectPlayer", ctypes.c_int)
    ]

os_name = platform.system()
if os_name == 'Windows':
    lib_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cg.dll")
elif os_name == "Darwin":
    lib_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "libcg.dylib")
elif platform.machine() in ('arm64', 'aarch64'):
    lib_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "libcg-arm64.so")
else:
    lib_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "libcg.so")
lib = ctypes.cdll.LoadLibrary(lib_path)

lib.GameInitialize()

lib.BattleStart.restype = StartData
lib.BattleStart.argtypes = [ctypes.POINTER(ctypes.c_int)]

if hasattr(lib, "BattleStartSeeded"):
    lib.BattleStartSeeded.restype = StartData
    lib.BattleStartSeeded.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
if hasattr(lib, "BattleStartOrdered"):
    lib.BattleStartOrdered.restype = StartData
    lib.BattleStartOrdered.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_uint]

lib.AgentStart.restype = ctypes.c_void_p

lib.BattleFinish.argtypes = [ctypes.c_void_p]

lib.GetBattleData.restype = SerialData
lib.GetBattleData.argtypes = [ctypes.c_void_p]

lib.Select.restype = ctypes.c_int
lib.Select.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.c_int]

lib.VisualizeData.restype = ctypes.c_char_p
lib.VisualizeData.argtypes = [ctypes.c_void_p]

lib.SearchBegin.restype = ctypes.c_char_p
lib.SearchBegin.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
    ctypes.c_int]

lib.SearchStep.restype = ctypes.c_char_p
lib.SearchStep.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.POINTER(ctypes.c_int), ctypes.c_int]

lib.SearchEnd.argtypes = [ctypes.c_void_p]

lib.SearchRelease.argtypes = [ctypes.c_void_p, ctypes.c_int64]

if hasattr(lib, "ExactDecide"):
    lib.ExactDecide.restype = ctypes.c_char_p
    lib.ExactDecide.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
        ctypes.c_int, ctypes.c_int]

if hasattr(lib, "ExactEvaluateAction"):
    lib.ExactEvaluateAction.restype = ctypes.c_char_p
    lib.ExactEvaluateAction.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
        ctypes.c_int, ctypes.c_int, ctypes.c_int]

if hasattr(lib, "ExactDecideV2"):
    lib.ExactDecideV2.restype = ctypes.c_char_p
    lib.ExactDecideV2.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_int]

if hasattr(lib, "ExactEvaluateActionV2"):
    lib.ExactEvaluateActionV2.restype = ctypes.c_char_p
    lib.ExactEvaluateActionV2.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_int, ctypes.c_int]

if hasattr(lib, "ExactTurnBegin"):
    lib.ExactTurnBegin.restype = ctypes.c_char_p
    lib.ExactTurnBegin.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_int]

if hasattr(lib, "ExactTurnAdvance"):
    lib.ExactTurnAdvance.restype = ctypes.c_char_p
    lib.ExactTurnAdvance.argtypes = [
        ctypes.c_void_p, ctypes.c_int64, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]

if hasattr(lib, "ExactTurnProgress"):
    lib.ExactTurnProgress.restype = ctypes.c_char_p
    lib.ExactTurnProgress.argtypes = [ctypes.c_void_p, ctypes.c_int64]

if hasattr(lib, "ExactEstimateRoot"):
    lib.ExactEstimateRoot.restype = ctypes.c_char_p
    lib.ExactEstimateRoot.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
        ctypes.c_int, ctypes.c_uint64]
if hasattr(lib, "ExactGeneralDecide"):
    lib.ExactGeneralDecide.restype = ctypes.c_char_p
    lib.ExactGeneralDecide.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
        ctypes.c_int, ctypes.c_int, ctypes.c_uint64]
if hasattr(lib, "ExactLoadEvaluatorModel"):
    lib.ExactLoadEvaluatorModel.restype = ctypes.c_char_p
    lib.ExactLoadEvaluatorModel.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
if hasattr(lib, "ExactUnloadEvaluatorModel"):
    lib.ExactUnloadEvaluatorModel.argtypes = [ctypes.c_void_p]
if hasattr(lib, "ExactLoadGeneralEvaluatorModel"):
    lib.ExactLoadGeneralEvaluatorModel.restype = ctypes.c_char_p
    lib.ExactLoadGeneralEvaluatorModel.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
if hasattr(lib, "ExactUnloadGeneralEvaluatorModel"):
    lib.ExactUnloadGeneralEvaluatorModel.argtypes = [ctypes.c_void_p]
if hasattr(lib, "ExactArithmeticDiagnostics"):
    lib.ExactArithmeticDiagnostics.restype = ctypes.c_char_p
if hasattr(lib, "ExactEvaluatorTokensV3"):
    lib.ExactEvaluatorTokensV3.restype = ctypes.c_char_p
if hasattr(lib, "ExactEvaluatorV3Diagnostics"):
    lib.ExactEvaluatorV3Diagnostics.restype = ctypes.c_char_p
if hasattr(lib, "ExactCardLivenessV4Diagnostics"):
    lib.ExactCardLivenessV4Diagnostics.restype = ctypes.c_char_p
if hasattr(lib, "ExactCardLivenessV4SchemaVersion"):
    lib.ExactCardLivenessV4SchemaVersion.restype = ctypes.c_int
if hasattr(lib, "ExactPassiveExpectationV4Oracle"):
    lib.ExactPassiveExpectationV4Oracle.restype = ctypes.c_char_p
    lib.ExactPassiveExpectationV4Oracle.argtypes = [
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
if hasattr(lib, "ExactEvaluateFeaturesV3"):
    lib.ExactEvaluateFeaturesV3.restype = ctypes.c_int64
    lib.ExactEvaluateFeaturesV3.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.POINTER(ctypes.c_int)]

if hasattr(lib, "ExactReplayTraceBegin"):
    lib.ExactReplayTraceBegin.restype = ctypes.c_int
    lib.ExactReplayTraceBegin.argtypes = [ctypes.c_void_p]
if hasattr(lib, "ExactReplayIntermediateTraceBegin"):
    lib.ExactReplayIntermediateTraceBegin.restype = ctypes.c_int
    lib.ExactReplayIntermediateTraceBegin.argtypes = [ctypes.c_void_p]
if hasattr(lib, "ExactReplayDualTraceBegin"):
    lib.ExactReplayDualTraceBegin.restype = ctypes.c_int
    lib.ExactReplayDualTraceBegin.argtypes = [ctypes.c_void_p]
if hasattr(lib, "ExactReplaySetDeckOrder"):
    lib.ExactReplaySetDeckOrder.restype = ctypes.c_int
    lib.ExactReplaySetDeckOrder.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                             ctypes.POINTER(ctypes.c_int), ctypes.c_int]
if hasattr(lib, "ExactReplaySetHiddenZones"):
    lib.ExactReplaySetHiddenZones.restype = ctypes.c_int
    lib.ExactReplaySetHiddenZones.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                               ctypes.POINTER(ctypes.c_int), ctypes.c_int,
                                               ctypes.POINTER(ctypes.c_int), ctypes.c_int]
if hasattr(lib, "ExactReplaySetAllHiddenZones"):
    lib.ExactReplaySetAllHiddenZones.restype = ctypes.c_int
    lib.ExactReplaySetAllHiddenZones.argtypes = [
        ctypes.c_void_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.c_int]
if hasattr(lib, "ExactReplayTraceDrain"):
    lib.ExactReplayTraceDrain.restype = ctypes.c_char_p
    lib.ExactReplayTraceDrain.argtypes = [ctypes.c_void_p]
if hasattr(lib, "ExactReplayTraceEnd"):
    lib.ExactReplayTraceEnd.argtypes = [ctypes.c_void_p]

if hasattr(lib, "ExactTurnRelease"):
    lib.ExactTurnRelease.argtypes = [ctypes.c_void_p, ctypes.c_int64]

lib.AllCard.restype = ctypes.c_char_p

lib.AllAttack.restype = ctypes.c_char_p

class Battle:
    battle_ptr = None
    obs = None
