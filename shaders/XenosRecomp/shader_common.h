#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
// MASSEFFECT: constants arrive through a dynamic UBO (a constant bank on Maxwell) instead of being read
// through a 64-bit pointer. High bit so as not to clash with the UNLEASHED_RECOMP ones.
#define SPEC_CONSTANT_CONSTANTS_UBO    (1 << 8)
// The tfetch offset is scaled with 1/size taken from the shared constants instead of querying the
// texture for its size. A size query is another texture unit operation.
#define SPEC_CONSTANT_INV_TEX_SIZE    (1 << 9)
// sa2-nx (banjo_gpu_blend_skip): the pixel's blend would leave the target exactly as it was (a UNORM target, no depth or
// stencil write, a blend that keeps the destination when the source alpha, or the whole source, is 0 or less), so it
// is discarded before the blend. Set by the renderer per pipeline; a NaN output is never discarded.
#define SPEC_CONSTANT_BLEND_SKIP_ALPHA (1 << 10)
#define SPEC_CONSTANT_BLEND_SKIP_ZERO  (1 << 11)
// sa2-nx GPU round 2 (banjo_gpu_structured_cf): the shader's forward-only control flow as nested if/else instead of the
// per-instruction skipTo tests (the translator emits both when it has proven them equivalent; the renderer picks).
#define SPEC_CONSTANT_STRUCTURED_CF    (1 << 12)
// sa2-nx GPU round 2, per-pipeline facts the native renderer knows, so the shaders stop testing them per vertex or
// pixel (bits 5, 6 and 13 are free in the MASSEFFECT_RECOMP build):
// banjo_gpu_texcoords_unswapped: the renderer never swaps texcoords (g_SwappedTexcoords is always 0).
#define SPEC_CONSTANT_TEXCOORDS_UNSWAPPED (1 << 5)
// banjo_gpu_no_texture_signs: no texture of the draw has a signed, biased or gamma component (packed signs all 0).
#define SPEC_CONSTANT_NO_TEXTURE_SIGNS (1 << 6)
// banjo_gpu_identity_remap: every vertex input of the pipeline keeps its component order (g_InputRemap is 0xFFF).
#define SPEC_CONSTANT_IDENTITY_REMAP   (1 << 13)
// banjo_gpu_index_base_held: the translator proved this vertex shader never reads the vertex index r0 starts with: with
// the bit, r0 starts at 0 and the renderer may hold g_VertexIndexBase (and the shared block) across draws.
#define SPEC_CONSTANT_VERTEX_INDEX_UNUSED (1 << 19)
// banjo-nx: the old forms of two translator changes, for their keys' A/Bs (set = as before 2026-10-15; clear, the
// default, = the new form, so pipelines keep the keys the prewarm records have).
// banjo_gpu_vfetch_block: computed-index vertex fetches (bone palettes) word by word, a branch around every load.
#define SPEC_CONSTANT_VFETCH_WORDWISE (1 << 7)
// banjo_gpu_branchless_select: the translator's conditional values as short-circuit ?: and && (a branch each).
#define SPEC_CONSTANT_SHORT_CIRCUIT (1 << 3)
// banjo_gpu_pred_select (2026-10-16): a vertex shader's predicated blocks (if (p0) { ... }) as branches, as before. Clear:
// the blocks the translator could flatten run for every vertex and keep their results only where the predicate holds
// (banjoFlattenPredicated in shader_recompiler.cpp). Bit 2 is free in the MASSEFFECT_RECOMP build.
#define SPEC_CONSTANT_PRED_BRANCHES (1 << 2)
// banjo_gpu_pred_select_ps: the same for a pixel shader's blocks (bit 4, also free here), with its own key and A/B: in a
// pixel shader a flattened block also runs for the warps the branch skipped whole.
#define SPEC_CONSTANT_PRED_BRANCHES_PS (1 << 4)
/*
 * The alpha test function, specialized (bits 16-18).
 *
 * g_AlphaFunction used to come from the shared constants, that is, at run time, so alphaTestValue
 * was a 7-case switch: ~8 branches in every pixel shader that does alpha testing. It was not needed
 * at all: the app already knows the function when it creates the pipeline (it comes from RB_COLORCONTROL, which is already in the key), so with three more bits the compiler
 * keeps the one comparison that applies and drops the other six. The output is bit-identical.
 */
// Bit 15 is reserved (unused here; left free for the app's own use); bit 19 is VERTEX_INDEX_UNUSED.
// banjo-nx (banjo_gpu_early_alpha): the alpha-tested pixel shader runs the copy of its alpha computation that
// tools/early_alpha.py placed right after the last instruction the alpha needs, and a 2x2 quad whose four pixels all
// fail the test there is discarded before the rest of the shader (see banjoAlphaFailsEarly). The renderer sets it only
// with SPEC_CONSTANT_ALPHA_TEST, for shaders that have the copy.
#define SPEC_CONSTANT_EARLY_ALPHA      (1 << 23)
#define SPEC_CONSTANT_ALPHA_FUNC_SHIFT  16
#define SPEC_CONSTANT_ALPHA_FUNC_MASK   (7 << 16)

// banjo-nx (package 25, 2026-10-16): specialization constant 2 (g_SpecConstants2, the pipeline key's spec2 word) holds
// the OLD forms of this round's translator changes: set = as before, clear (the default) = the new form, so pipelines
// of older prewarm records get the new forms. Constant 0 has no free bit left. docs/shader-translator-ideas.md.
// banjo_gpu_clamp_cbuf (item 3): the Xenos RCP / RSQ / LOG clamps take -FLT_MAX / FLT_MAX from the shared block (a free
// constant-buffer operand on Maxwell) instead of two 32-bit immediates NAK moves into registers before each use.
#define SPEC2_CLAMP_IMMEDIATE    (1u << 0)
// banjo_gpu_sign_mask_skip (item 5): no `& 0x00FFFFFF` on a slot without effective signs (the renderer clears the
// top byte of such a slot's word: the packed sign 1 is fetched as sampled, like 0).
#define SPEC2_SIGN_MASK_ALWAYS   (1u << 1)
// banjo_gpu_vs_outputs_unwritten (item 16): a vertex output the pixel shader does not read is not written at all
// (instead of written as 0), so the driver drops its store and its attribute.
#define SPEC2_OUTPUTS_WRITE_ZERO (1u << 2)
// banjo_gpu_bone_padding (item 15): the renderer zero-pads every indexed fetch buffer, so an indexed fetch is a clamp
// and a load, with no compare and select per word.
#define SPEC2_BONES_SELECT       (1u << 3)
// banjo_gpu_literals_cbuf (package 26): the shader's literal constants that NAK cannot encode as immediates come from
// the constant registers Direct3D loaded them into (the renderer checks those registers at every draw).
#define SPEC2_LITERALS_IMMEDIATE (1u << 4)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

// MASSEFFECT: the same blocks of the upload buffer, also as dynamic UBOs in set 4. With
// -fvk-use-dx-layout each float4 takes 16 contiguous bytes, so any 4-byte word of the shared block is
// a component: v[B / 16][(B % 16) / 4], and asuint reads it without changing a bit.
struct MassEffectBlockVs { float4 v[256]; };
struct MassEffectBlockPs { float4 v[256]; };  // banjo-nx: 256 (Nuts & Bolts reads up to c255)
struct MassEffectSharedBlock { float4 v[50]; };  // banjo-nx (package 25): v[49].xy = -FLT_MAX, FLT_MAX (banjo_gpu_clamp_cbuf)  // banjo-nx: v[35..40] = the vertex shader texture slots, v[41..44] = the memory export streams, v[45] = tessellation, v[46..48] = pixel shader s16 / s17  // banjo-nx: + bool constants, indexed fetch buffers, vertex index base
[[vk::binding(0, 4)]] ConstantBuffer<MassEffectBlockVs> g_UboVertex;
[[vk::binding(1, 4)]] ConstantBuffer<MassEffectBlockPs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<MassEffectSharedBlock> g_UboShared;
// banjo-nx: the renderer always reads the constants through set 4 (it forces masseffect_native_constants_ubo on),
// so the choice is made here at compile time. As a specialization constant every constant read was a branch
// pair (UBO load / 64-bit pointer load) in the SPIR-V: twice the words and blocks for the driver to parse and
// optimize before the dead side disappeared (NVK on the Switch: ~0.5 s per material pipeline). Same loads.
#define MASSEFFECT_UBO true
#define MASSEFFECT_SHARED_UINT(B)  asuint(g_UboShared.v[(B) / 16][((B) % 16) / 4])
#define MASSEFFECT_SHARED_FLOAT(B) g_UboShared.v[(B) / 16][((B) % 16) / 4]

#define g_Booleans                 (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(256) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256))
// banjo-nx: all 256 bool constants (the 8 SHADER_CONSTANT_BOOL words, 0x4900..0x4907) at bytes 488..519 of the
// shared block; bool N is bit N % 32 of word N / 32, as on the Xenos. Nuts & Bolts' shaders (most without a CTAB)
// test bools anywhere in that range (pixel shaders up to b254), beyond g_Booleans' 16 bits per stage.
#define g_BoolWord(I)              (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(488 + (I) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 488 + (I) * 4))
#define BANJO_BOOL_RT(A)           ((g_BoolWord((A) >> 5) >> ((A) & 31)) & 1)
// banjo-nx (banjo_gpu_bool_spec): bool constant N can be specialised per pipeline: constant 1000 + N = its value (0 or
// 1), or 2 (the default) to read it at run time as before. The renderer sets the bools the pipeline's shaders test
// (DXC keeps only the constants a shader references, so the module's SpecIds 1000-1255 are that list), and the code
// behind a bool that is off in the pipeline disappears with its branch.
[[vk::constant_id(1000)]] const uint BANJO_BOOL_SPEC_0 = 2;
[[vk::constant_id(1001)]] const uint BANJO_BOOL_SPEC_1 = 2;
[[vk::constant_id(1002)]] const uint BANJO_BOOL_SPEC_2 = 2;
[[vk::constant_id(1003)]] const uint BANJO_BOOL_SPEC_3 = 2;
[[vk::constant_id(1004)]] const uint BANJO_BOOL_SPEC_4 = 2;
[[vk::constant_id(1005)]] const uint BANJO_BOOL_SPEC_5 = 2;
[[vk::constant_id(1006)]] const uint BANJO_BOOL_SPEC_6 = 2;
[[vk::constant_id(1007)]] const uint BANJO_BOOL_SPEC_7 = 2;
[[vk::constant_id(1008)]] const uint BANJO_BOOL_SPEC_8 = 2;
[[vk::constant_id(1009)]] const uint BANJO_BOOL_SPEC_9 = 2;
[[vk::constant_id(1010)]] const uint BANJO_BOOL_SPEC_10 = 2;
[[vk::constant_id(1011)]] const uint BANJO_BOOL_SPEC_11 = 2;
[[vk::constant_id(1012)]] const uint BANJO_BOOL_SPEC_12 = 2;
[[vk::constant_id(1013)]] const uint BANJO_BOOL_SPEC_13 = 2;
[[vk::constant_id(1014)]] const uint BANJO_BOOL_SPEC_14 = 2;
[[vk::constant_id(1015)]] const uint BANJO_BOOL_SPEC_15 = 2;
[[vk::constant_id(1016)]] const uint BANJO_BOOL_SPEC_16 = 2;
[[vk::constant_id(1017)]] const uint BANJO_BOOL_SPEC_17 = 2;
[[vk::constant_id(1018)]] const uint BANJO_BOOL_SPEC_18 = 2;
[[vk::constant_id(1019)]] const uint BANJO_BOOL_SPEC_19 = 2;
[[vk::constant_id(1020)]] const uint BANJO_BOOL_SPEC_20 = 2;
[[vk::constant_id(1021)]] const uint BANJO_BOOL_SPEC_21 = 2;
[[vk::constant_id(1022)]] const uint BANJO_BOOL_SPEC_22 = 2;
[[vk::constant_id(1023)]] const uint BANJO_BOOL_SPEC_23 = 2;
[[vk::constant_id(1024)]] const uint BANJO_BOOL_SPEC_24 = 2;
[[vk::constant_id(1025)]] const uint BANJO_BOOL_SPEC_25 = 2;
[[vk::constant_id(1026)]] const uint BANJO_BOOL_SPEC_26 = 2;
[[vk::constant_id(1027)]] const uint BANJO_BOOL_SPEC_27 = 2;
[[vk::constant_id(1028)]] const uint BANJO_BOOL_SPEC_28 = 2;
[[vk::constant_id(1029)]] const uint BANJO_BOOL_SPEC_29 = 2;
[[vk::constant_id(1030)]] const uint BANJO_BOOL_SPEC_30 = 2;
[[vk::constant_id(1031)]] const uint BANJO_BOOL_SPEC_31 = 2;
[[vk::constant_id(1032)]] const uint BANJO_BOOL_SPEC_32 = 2;
[[vk::constant_id(1033)]] const uint BANJO_BOOL_SPEC_33 = 2;
[[vk::constant_id(1034)]] const uint BANJO_BOOL_SPEC_34 = 2;
[[vk::constant_id(1035)]] const uint BANJO_BOOL_SPEC_35 = 2;
[[vk::constant_id(1036)]] const uint BANJO_BOOL_SPEC_36 = 2;
[[vk::constant_id(1037)]] const uint BANJO_BOOL_SPEC_37 = 2;
[[vk::constant_id(1038)]] const uint BANJO_BOOL_SPEC_38 = 2;
[[vk::constant_id(1039)]] const uint BANJO_BOOL_SPEC_39 = 2;
[[vk::constant_id(1040)]] const uint BANJO_BOOL_SPEC_40 = 2;
[[vk::constant_id(1041)]] const uint BANJO_BOOL_SPEC_41 = 2;
[[vk::constant_id(1042)]] const uint BANJO_BOOL_SPEC_42 = 2;
[[vk::constant_id(1043)]] const uint BANJO_BOOL_SPEC_43 = 2;
[[vk::constant_id(1044)]] const uint BANJO_BOOL_SPEC_44 = 2;
[[vk::constant_id(1045)]] const uint BANJO_BOOL_SPEC_45 = 2;
[[vk::constant_id(1046)]] const uint BANJO_BOOL_SPEC_46 = 2;
[[vk::constant_id(1047)]] const uint BANJO_BOOL_SPEC_47 = 2;
[[vk::constant_id(1048)]] const uint BANJO_BOOL_SPEC_48 = 2;
[[vk::constant_id(1049)]] const uint BANJO_BOOL_SPEC_49 = 2;
[[vk::constant_id(1050)]] const uint BANJO_BOOL_SPEC_50 = 2;
[[vk::constant_id(1051)]] const uint BANJO_BOOL_SPEC_51 = 2;
[[vk::constant_id(1052)]] const uint BANJO_BOOL_SPEC_52 = 2;
[[vk::constant_id(1053)]] const uint BANJO_BOOL_SPEC_53 = 2;
[[vk::constant_id(1054)]] const uint BANJO_BOOL_SPEC_54 = 2;
[[vk::constant_id(1055)]] const uint BANJO_BOOL_SPEC_55 = 2;
[[vk::constant_id(1056)]] const uint BANJO_BOOL_SPEC_56 = 2;
[[vk::constant_id(1057)]] const uint BANJO_BOOL_SPEC_57 = 2;
[[vk::constant_id(1058)]] const uint BANJO_BOOL_SPEC_58 = 2;
[[vk::constant_id(1059)]] const uint BANJO_BOOL_SPEC_59 = 2;
[[vk::constant_id(1060)]] const uint BANJO_BOOL_SPEC_60 = 2;
[[vk::constant_id(1061)]] const uint BANJO_BOOL_SPEC_61 = 2;
[[vk::constant_id(1062)]] const uint BANJO_BOOL_SPEC_62 = 2;
[[vk::constant_id(1063)]] const uint BANJO_BOOL_SPEC_63 = 2;
[[vk::constant_id(1064)]] const uint BANJO_BOOL_SPEC_64 = 2;
[[vk::constant_id(1065)]] const uint BANJO_BOOL_SPEC_65 = 2;
[[vk::constant_id(1066)]] const uint BANJO_BOOL_SPEC_66 = 2;
[[vk::constant_id(1067)]] const uint BANJO_BOOL_SPEC_67 = 2;
[[vk::constant_id(1068)]] const uint BANJO_BOOL_SPEC_68 = 2;
[[vk::constant_id(1069)]] const uint BANJO_BOOL_SPEC_69 = 2;
[[vk::constant_id(1070)]] const uint BANJO_BOOL_SPEC_70 = 2;
[[vk::constant_id(1071)]] const uint BANJO_BOOL_SPEC_71 = 2;
[[vk::constant_id(1072)]] const uint BANJO_BOOL_SPEC_72 = 2;
[[vk::constant_id(1073)]] const uint BANJO_BOOL_SPEC_73 = 2;
[[vk::constant_id(1074)]] const uint BANJO_BOOL_SPEC_74 = 2;
[[vk::constant_id(1075)]] const uint BANJO_BOOL_SPEC_75 = 2;
[[vk::constant_id(1076)]] const uint BANJO_BOOL_SPEC_76 = 2;
[[vk::constant_id(1077)]] const uint BANJO_BOOL_SPEC_77 = 2;
[[vk::constant_id(1078)]] const uint BANJO_BOOL_SPEC_78 = 2;
[[vk::constant_id(1079)]] const uint BANJO_BOOL_SPEC_79 = 2;
[[vk::constant_id(1080)]] const uint BANJO_BOOL_SPEC_80 = 2;
[[vk::constant_id(1081)]] const uint BANJO_BOOL_SPEC_81 = 2;
[[vk::constant_id(1082)]] const uint BANJO_BOOL_SPEC_82 = 2;
[[vk::constant_id(1083)]] const uint BANJO_BOOL_SPEC_83 = 2;
[[vk::constant_id(1084)]] const uint BANJO_BOOL_SPEC_84 = 2;
[[vk::constant_id(1085)]] const uint BANJO_BOOL_SPEC_85 = 2;
[[vk::constant_id(1086)]] const uint BANJO_BOOL_SPEC_86 = 2;
[[vk::constant_id(1087)]] const uint BANJO_BOOL_SPEC_87 = 2;
[[vk::constant_id(1088)]] const uint BANJO_BOOL_SPEC_88 = 2;
[[vk::constant_id(1089)]] const uint BANJO_BOOL_SPEC_89 = 2;
[[vk::constant_id(1090)]] const uint BANJO_BOOL_SPEC_90 = 2;
[[vk::constant_id(1091)]] const uint BANJO_BOOL_SPEC_91 = 2;
[[vk::constant_id(1092)]] const uint BANJO_BOOL_SPEC_92 = 2;
[[vk::constant_id(1093)]] const uint BANJO_BOOL_SPEC_93 = 2;
[[vk::constant_id(1094)]] const uint BANJO_BOOL_SPEC_94 = 2;
[[vk::constant_id(1095)]] const uint BANJO_BOOL_SPEC_95 = 2;
[[vk::constant_id(1096)]] const uint BANJO_BOOL_SPEC_96 = 2;
[[vk::constant_id(1097)]] const uint BANJO_BOOL_SPEC_97 = 2;
[[vk::constant_id(1098)]] const uint BANJO_BOOL_SPEC_98 = 2;
[[vk::constant_id(1099)]] const uint BANJO_BOOL_SPEC_99 = 2;
[[vk::constant_id(1100)]] const uint BANJO_BOOL_SPEC_100 = 2;
[[vk::constant_id(1101)]] const uint BANJO_BOOL_SPEC_101 = 2;
[[vk::constant_id(1102)]] const uint BANJO_BOOL_SPEC_102 = 2;
[[vk::constant_id(1103)]] const uint BANJO_BOOL_SPEC_103 = 2;
[[vk::constant_id(1104)]] const uint BANJO_BOOL_SPEC_104 = 2;
[[vk::constant_id(1105)]] const uint BANJO_BOOL_SPEC_105 = 2;
[[vk::constant_id(1106)]] const uint BANJO_BOOL_SPEC_106 = 2;
[[vk::constant_id(1107)]] const uint BANJO_BOOL_SPEC_107 = 2;
[[vk::constant_id(1108)]] const uint BANJO_BOOL_SPEC_108 = 2;
[[vk::constant_id(1109)]] const uint BANJO_BOOL_SPEC_109 = 2;
[[vk::constant_id(1110)]] const uint BANJO_BOOL_SPEC_110 = 2;
[[vk::constant_id(1111)]] const uint BANJO_BOOL_SPEC_111 = 2;
[[vk::constant_id(1112)]] const uint BANJO_BOOL_SPEC_112 = 2;
[[vk::constant_id(1113)]] const uint BANJO_BOOL_SPEC_113 = 2;
[[vk::constant_id(1114)]] const uint BANJO_BOOL_SPEC_114 = 2;
[[vk::constant_id(1115)]] const uint BANJO_BOOL_SPEC_115 = 2;
[[vk::constant_id(1116)]] const uint BANJO_BOOL_SPEC_116 = 2;
[[vk::constant_id(1117)]] const uint BANJO_BOOL_SPEC_117 = 2;
[[vk::constant_id(1118)]] const uint BANJO_BOOL_SPEC_118 = 2;
[[vk::constant_id(1119)]] const uint BANJO_BOOL_SPEC_119 = 2;
[[vk::constant_id(1120)]] const uint BANJO_BOOL_SPEC_120 = 2;
[[vk::constant_id(1121)]] const uint BANJO_BOOL_SPEC_121 = 2;
[[vk::constant_id(1122)]] const uint BANJO_BOOL_SPEC_122 = 2;
[[vk::constant_id(1123)]] const uint BANJO_BOOL_SPEC_123 = 2;
[[vk::constant_id(1124)]] const uint BANJO_BOOL_SPEC_124 = 2;
[[vk::constant_id(1125)]] const uint BANJO_BOOL_SPEC_125 = 2;
[[vk::constant_id(1126)]] const uint BANJO_BOOL_SPEC_126 = 2;
[[vk::constant_id(1127)]] const uint BANJO_BOOL_SPEC_127 = 2;
[[vk::constant_id(1128)]] const uint BANJO_BOOL_SPEC_128 = 2;
[[vk::constant_id(1129)]] const uint BANJO_BOOL_SPEC_129 = 2;
[[vk::constant_id(1130)]] const uint BANJO_BOOL_SPEC_130 = 2;
[[vk::constant_id(1131)]] const uint BANJO_BOOL_SPEC_131 = 2;
[[vk::constant_id(1132)]] const uint BANJO_BOOL_SPEC_132 = 2;
[[vk::constant_id(1133)]] const uint BANJO_BOOL_SPEC_133 = 2;
[[vk::constant_id(1134)]] const uint BANJO_BOOL_SPEC_134 = 2;
[[vk::constant_id(1135)]] const uint BANJO_BOOL_SPEC_135 = 2;
[[vk::constant_id(1136)]] const uint BANJO_BOOL_SPEC_136 = 2;
[[vk::constant_id(1137)]] const uint BANJO_BOOL_SPEC_137 = 2;
[[vk::constant_id(1138)]] const uint BANJO_BOOL_SPEC_138 = 2;
[[vk::constant_id(1139)]] const uint BANJO_BOOL_SPEC_139 = 2;
[[vk::constant_id(1140)]] const uint BANJO_BOOL_SPEC_140 = 2;
[[vk::constant_id(1141)]] const uint BANJO_BOOL_SPEC_141 = 2;
[[vk::constant_id(1142)]] const uint BANJO_BOOL_SPEC_142 = 2;
[[vk::constant_id(1143)]] const uint BANJO_BOOL_SPEC_143 = 2;
[[vk::constant_id(1144)]] const uint BANJO_BOOL_SPEC_144 = 2;
[[vk::constant_id(1145)]] const uint BANJO_BOOL_SPEC_145 = 2;
[[vk::constant_id(1146)]] const uint BANJO_BOOL_SPEC_146 = 2;
[[vk::constant_id(1147)]] const uint BANJO_BOOL_SPEC_147 = 2;
[[vk::constant_id(1148)]] const uint BANJO_BOOL_SPEC_148 = 2;
[[vk::constant_id(1149)]] const uint BANJO_BOOL_SPEC_149 = 2;
[[vk::constant_id(1150)]] const uint BANJO_BOOL_SPEC_150 = 2;
[[vk::constant_id(1151)]] const uint BANJO_BOOL_SPEC_151 = 2;
[[vk::constant_id(1152)]] const uint BANJO_BOOL_SPEC_152 = 2;
[[vk::constant_id(1153)]] const uint BANJO_BOOL_SPEC_153 = 2;
[[vk::constant_id(1154)]] const uint BANJO_BOOL_SPEC_154 = 2;
[[vk::constant_id(1155)]] const uint BANJO_BOOL_SPEC_155 = 2;
[[vk::constant_id(1156)]] const uint BANJO_BOOL_SPEC_156 = 2;
[[vk::constant_id(1157)]] const uint BANJO_BOOL_SPEC_157 = 2;
[[vk::constant_id(1158)]] const uint BANJO_BOOL_SPEC_158 = 2;
[[vk::constant_id(1159)]] const uint BANJO_BOOL_SPEC_159 = 2;
[[vk::constant_id(1160)]] const uint BANJO_BOOL_SPEC_160 = 2;
[[vk::constant_id(1161)]] const uint BANJO_BOOL_SPEC_161 = 2;
[[vk::constant_id(1162)]] const uint BANJO_BOOL_SPEC_162 = 2;
[[vk::constant_id(1163)]] const uint BANJO_BOOL_SPEC_163 = 2;
[[vk::constant_id(1164)]] const uint BANJO_BOOL_SPEC_164 = 2;
[[vk::constant_id(1165)]] const uint BANJO_BOOL_SPEC_165 = 2;
[[vk::constant_id(1166)]] const uint BANJO_BOOL_SPEC_166 = 2;
[[vk::constant_id(1167)]] const uint BANJO_BOOL_SPEC_167 = 2;
[[vk::constant_id(1168)]] const uint BANJO_BOOL_SPEC_168 = 2;
[[vk::constant_id(1169)]] const uint BANJO_BOOL_SPEC_169 = 2;
[[vk::constant_id(1170)]] const uint BANJO_BOOL_SPEC_170 = 2;
[[vk::constant_id(1171)]] const uint BANJO_BOOL_SPEC_171 = 2;
[[vk::constant_id(1172)]] const uint BANJO_BOOL_SPEC_172 = 2;
[[vk::constant_id(1173)]] const uint BANJO_BOOL_SPEC_173 = 2;
[[vk::constant_id(1174)]] const uint BANJO_BOOL_SPEC_174 = 2;
[[vk::constant_id(1175)]] const uint BANJO_BOOL_SPEC_175 = 2;
[[vk::constant_id(1176)]] const uint BANJO_BOOL_SPEC_176 = 2;
[[vk::constant_id(1177)]] const uint BANJO_BOOL_SPEC_177 = 2;
[[vk::constant_id(1178)]] const uint BANJO_BOOL_SPEC_178 = 2;
[[vk::constant_id(1179)]] const uint BANJO_BOOL_SPEC_179 = 2;
[[vk::constant_id(1180)]] const uint BANJO_BOOL_SPEC_180 = 2;
[[vk::constant_id(1181)]] const uint BANJO_BOOL_SPEC_181 = 2;
[[vk::constant_id(1182)]] const uint BANJO_BOOL_SPEC_182 = 2;
[[vk::constant_id(1183)]] const uint BANJO_BOOL_SPEC_183 = 2;
[[vk::constant_id(1184)]] const uint BANJO_BOOL_SPEC_184 = 2;
[[vk::constant_id(1185)]] const uint BANJO_BOOL_SPEC_185 = 2;
[[vk::constant_id(1186)]] const uint BANJO_BOOL_SPEC_186 = 2;
[[vk::constant_id(1187)]] const uint BANJO_BOOL_SPEC_187 = 2;
[[vk::constant_id(1188)]] const uint BANJO_BOOL_SPEC_188 = 2;
[[vk::constant_id(1189)]] const uint BANJO_BOOL_SPEC_189 = 2;
[[vk::constant_id(1190)]] const uint BANJO_BOOL_SPEC_190 = 2;
[[vk::constant_id(1191)]] const uint BANJO_BOOL_SPEC_191 = 2;
[[vk::constant_id(1192)]] const uint BANJO_BOOL_SPEC_192 = 2;
[[vk::constant_id(1193)]] const uint BANJO_BOOL_SPEC_193 = 2;
[[vk::constant_id(1194)]] const uint BANJO_BOOL_SPEC_194 = 2;
[[vk::constant_id(1195)]] const uint BANJO_BOOL_SPEC_195 = 2;
[[vk::constant_id(1196)]] const uint BANJO_BOOL_SPEC_196 = 2;
[[vk::constant_id(1197)]] const uint BANJO_BOOL_SPEC_197 = 2;
[[vk::constant_id(1198)]] const uint BANJO_BOOL_SPEC_198 = 2;
[[vk::constant_id(1199)]] const uint BANJO_BOOL_SPEC_199 = 2;
[[vk::constant_id(1200)]] const uint BANJO_BOOL_SPEC_200 = 2;
[[vk::constant_id(1201)]] const uint BANJO_BOOL_SPEC_201 = 2;
[[vk::constant_id(1202)]] const uint BANJO_BOOL_SPEC_202 = 2;
[[vk::constant_id(1203)]] const uint BANJO_BOOL_SPEC_203 = 2;
[[vk::constant_id(1204)]] const uint BANJO_BOOL_SPEC_204 = 2;
[[vk::constant_id(1205)]] const uint BANJO_BOOL_SPEC_205 = 2;
[[vk::constant_id(1206)]] const uint BANJO_BOOL_SPEC_206 = 2;
[[vk::constant_id(1207)]] const uint BANJO_BOOL_SPEC_207 = 2;
[[vk::constant_id(1208)]] const uint BANJO_BOOL_SPEC_208 = 2;
[[vk::constant_id(1209)]] const uint BANJO_BOOL_SPEC_209 = 2;
[[vk::constant_id(1210)]] const uint BANJO_BOOL_SPEC_210 = 2;
[[vk::constant_id(1211)]] const uint BANJO_BOOL_SPEC_211 = 2;
[[vk::constant_id(1212)]] const uint BANJO_BOOL_SPEC_212 = 2;
[[vk::constant_id(1213)]] const uint BANJO_BOOL_SPEC_213 = 2;
[[vk::constant_id(1214)]] const uint BANJO_BOOL_SPEC_214 = 2;
[[vk::constant_id(1215)]] const uint BANJO_BOOL_SPEC_215 = 2;
[[vk::constant_id(1216)]] const uint BANJO_BOOL_SPEC_216 = 2;
[[vk::constant_id(1217)]] const uint BANJO_BOOL_SPEC_217 = 2;
[[vk::constant_id(1218)]] const uint BANJO_BOOL_SPEC_218 = 2;
[[vk::constant_id(1219)]] const uint BANJO_BOOL_SPEC_219 = 2;
[[vk::constant_id(1220)]] const uint BANJO_BOOL_SPEC_220 = 2;
[[vk::constant_id(1221)]] const uint BANJO_BOOL_SPEC_221 = 2;
[[vk::constant_id(1222)]] const uint BANJO_BOOL_SPEC_222 = 2;
[[vk::constant_id(1223)]] const uint BANJO_BOOL_SPEC_223 = 2;
[[vk::constant_id(1224)]] const uint BANJO_BOOL_SPEC_224 = 2;
[[vk::constant_id(1225)]] const uint BANJO_BOOL_SPEC_225 = 2;
[[vk::constant_id(1226)]] const uint BANJO_BOOL_SPEC_226 = 2;
[[vk::constant_id(1227)]] const uint BANJO_BOOL_SPEC_227 = 2;
[[vk::constant_id(1228)]] const uint BANJO_BOOL_SPEC_228 = 2;
[[vk::constant_id(1229)]] const uint BANJO_BOOL_SPEC_229 = 2;
[[vk::constant_id(1230)]] const uint BANJO_BOOL_SPEC_230 = 2;
[[vk::constant_id(1231)]] const uint BANJO_BOOL_SPEC_231 = 2;
[[vk::constant_id(1232)]] const uint BANJO_BOOL_SPEC_232 = 2;
[[vk::constant_id(1233)]] const uint BANJO_BOOL_SPEC_233 = 2;
[[vk::constant_id(1234)]] const uint BANJO_BOOL_SPEC_234 = 2;
[[vk::constant_id(1235)]] const uint BANJO_BOOL_SPEC_235 = 2;
[[vk::constant_id(1236)]] const uint BANJO_BOOL_SPEC_236 = 2;
[[vk::constant_id(1237)]] const uint BANJO_BOOL_SPEC_237 = 2;
[[vk::constant_id(1238)]] const uint BANJO_BOOL_SPEC_238 = 2;
[[vk::constant_id(1239)]] const uint BANJO_BOOL_SPEC_239 = 2;
[[vk::constant_id(1240)]] const uint BANJO_BOOL_SPEC_240 = 2;
[[vk::constant_id(1241)]] const uint BANJO_BOOL_SPEC_241 = 2;
[[vk::constant_id(1242)]] const uint BANJO_BOOL_SPEC_242 = 2;
[[vk::constant_id(1243)]] const uint BANJO_BOOL_SPEC_243 = 2;
[[vk::constant_id(1244)]] const uint BANJO_BOOL_SPEC_244 = 2;
[[vk::constant_id(1245)]] const uint BANJO_BOOL_SPEC_245 = 2;
[[vk::constant_id(1246)]] const uint BANJO_BOOL_SPEC_246 = 2;
[[vk::constant_id(1247)]] const uint BANJO_BOOL_SPEC_247 = 2;
[[vk::constant_id(1248)]] const uint BANJO_BOOL_SPEC_248 = 2;
[[vk::constant_id(1249)]] const uint BANJO_BOOL_SPEC_249 = 2;
[[vk::constant_id(1250)]] const uint BANJO_BOOL_SPEC_250 = 2;
[[vk::constant_id(1251)]] const uint BANJO_BOOL_SPEC_251 = 2;
[[vk::constant_id(1252)]] const uint BANJO_BOOL_SPEC_252 = 2;
[[vk::constant_id(1253)]] const uint BANJO_BOOL_SPEC_253 = 2;
[[vk::constant_id(1254)]] const uint BANJO_BOOL_SPEC_254 = 2;
[[vk::constant_id(1255)]] const uint BANJO_BOOL_SPEC_255 = 2;
#define BANJO_BOOL(A)              ((BANJO_BOOL_SPEC_##A != 2u) ? BANJO_BOOL_SPEC_##A : BANJO_BOOL_RT(A))
#define g_SwappedTexcoords         (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(260) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260))
#define g_HalfPixelOffset          (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(264), MASSEFFECT_SHARED_FLOAT(268)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264))
#define g_AlphaThreshold           (MASSEFFECT_UBO ? MASSEFFECT_SHARED_FLOAT(272) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272))
// MASSEFFECT: alpha test function (RB_COLORCONTROL.alpha_func): 0 never, 1 <, 2 ==, 3 <=,
// 4 >, 5 !=, 6 >=, 7 always.
#define g_AlphaFunction            (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(276) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276))
// MASSEFFECT: position to host clip space, like ndc_scale/ndc_offset in the
// emulation (graphics/util/draw.cpp). (1, 1) and (0, 0) for normal draws;
// with Xenos clipping disabled it converts from pixels to NDC.
#define g_NdcScale                 (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(280), MASSEFFECT_SHARED_FLOAT(284)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 280))
#define g_NdcOffset                (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(288), MASSEFFECT_SHARED_FLOAT(292)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 288))
// MASSEFFECT: where each component of the vertex input at that location comes from
// (D3D patches the fetch swizzle according to the declaration). 0xFFF = as is.
#define g_InputRemap(LOC)          (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(296 + (LOC) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 296 + (LOC) * 4))
// banjo-nx: SV_VertexID + this = the guest vertex index, which a Xenos vertex shader finds in r0.x (byte 544).
#define g_VertexIndexBase          (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(544) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 544))

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;
[[vk::constant_id(2)]] const uint g_SpecConstants2 = 0;  // banjo-nx (package 25): the SPEC2_* old forms
// banjo_gpu_clamp_cbuf: the bounds of the Xenos clamps (the translator writes clamp(x, BANJO_FLT_LO, BANJO_FLT_HI)).
// (select, not ?:, which HLSL 2021 makes a branch per use in the SPIR-V until the pipeline folds it: the library grew by
// a fifth with it. Both fold the same once constant 2 is known.)
#define BANJO_FLT_LO select((g_SpecConstants2 & SPEC2_CLAMP_IMMEDIATE) != 0, FLT_MIN, MASSEFFECT_SHARED_FLOAT(784))
#define BANJO_FLT_HI select((g_SpecConstants2 & SPEC2_CLAMP_IMMEDIATE) != 0, FLT_MAX, MASSEFFECT_SHARED_FLOAT(788))
// banjo_gpu_vs_outputs_unwritten: the old form writes 0 to an output the pixel shader does not read.
#define BANJO_OUTPUTS_WRITE_ZERO ((g_SpecConstants2 & SPEC2_OUTPUTS_WRITE_ZERO) != 0)
// banjo_gpu_literals_cbuf: literal register N with words X..W; bit k of M = component k from the constant register
// (the translator sets it where the word has any of its low 12 bits set). and() and select(): no branch in the module.
#define BANJO_LIT_C(V, BIT, M, X) select(and(((M) & (BIT)) != 0u, (g_SpecConstants2 & SPEC2_LITERALS_IMMEDIATE) == 0u), (V), asfloat(X##u))
#define BANJO_LITERAL_PS(N, X, Y, Z, W, M) float4(BANJO_LIT_C(g_UboPixel.v[N].x, 1u, M, X), \
    BANJO_LIT_C(g_UboPixel.v[N].y, 2u, M, Y), BANJO_LIT_C(g_UboPixel.v[N].z, 4u, M, Z), BANJO_LIT_C(g_UboPixel.v[N].w, 8u, M, W))
#define BANJO_LITERAL_VS(N, X, Y, Z, W, M) float4(BANJO_LIT_C(g_UboVertex.v[N].x, 1u, M, X), \
    BANJO_LIT_C(g_UboVertex.v[N].y, 2u, M, Y), BANJO_LIT_C(g_UboVertex.v[N].z, 4u, M, Z), BANJO_LIT_C(g_UboVertex.v[N].w, 8u, M, W))

#define g_SpecConstants() g_SpecConstants

// banjo-nx (banjo_gpu_texture_signs_per_slot): bit r set = fetch constant r's texture may have packed signs. The
// renderer clears the bits of the registers whose texture words all have packed signs 0 in the draw, and their
// fetches fold to the path they take anyway at run time (masseffectApplyTextureSigns with 0: the texel as is).
[[vk::constant_id(61)]] const uint g_SignSlots = 0xFFFFFFFF;
#define BANJO_SLOT_SIGNS(REG) ((g_SignSlots & (1u << (REG))) != 0)
// banjo-nx (masseffect_native_vs_pruned_outputs): bit L set = the pipeline's pixel shader reads input location L
// (TEXCOORD0-15 = 0-15, COLOR0-5 = 16-21; 0 for a pipeline without one). A vertex shader sets the outputs it does not
// read to 0 at its exit, so the math that only fed them is removed when the pipeline is specialised.
[[vk::constant_id(60)]] const uint g_PsInputs = 0xFFFFFFFF;
#define BANJO_PS_READS(L) ((g_PsInputs & (1u << (L))) != 0)

// banjo-nx: vertex fetches indexed by a computed register (bone palettes; classifyVertexFetches in
// shader_recompiler.cpp). The renderer uploads the buffer of each such fetch constant K (0..1) in host word order
// and passes its device address at bytes 520 + 8K of the shared block and its size in words at bytes 536 + 4K.
// Formats decode like the emulation's vertex fetch (graphics/pipeline/shader/spirv_translator_fetch.cpp).
uint banjoVfetchWord(uint K, int dword)
{
    const uint2 address = MASSEFFECT_UBO ? uint2(MASSEFFECT_SHARED_UINT(520 + K * 8), MASSEFFECT_SHARED_UINT(524 + K * 8))
                                         : vk::RawBufferLoad<uint2>(g_PushConstants.SharedConstants + 520 + K * 8, 8);
    const uint words = MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(536 + K * 4)
                                      : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 536 + K * 4);
    if (dword < 0 || uint(dword) >= words)
        return 0;
    return vk::RawBufferLoad<uint>(((uint64_t(address.y) << 32) | uint64_t(address.x)) + uint64_t(uint(dword)) * 4, 4);
}

float4 banjoVfetchPacked(uint4 values, uint4 widths, uint isSigned, uint isInteger)
{
    float4 result;
    [unroll] for (uint i = 0; i < 4; i++)
    {
        const uint width = widths[i];
        if (width == 0)
        {
            result[i] = i == 3 ? 1.0 : 0.0;
            continue;
        }
        const uint value = values[i] & ((1u << width) - 1u);
        if (isSigned != 0)
        {
            const int signedValue = int(value << (32 - width)) >> (32 - width);
            result[i] = isInteger != 0 ? float(signedValue)
                                       : max(float(signedValue) / float((1u << (width - 1)) - 1u), -1.0);
        }
        else
        {
            result[i] = isInteger != 0 ? float(value) : float(value) / float((1u << width) - 1u);
        }
    }
    return result;
}

// banjo-nx: the words an element of the format reads (as the cases below; k_32_32_32_32 reads 3 there).
uint banjoVfetchCount(uint format)
{
    switch (format)
    {
    case 26: case 32: case 34: case 37: return 2;
    case 35: case 57: return 3;
    case 38: return 4;
    default: return 1;
    }
}

// banjo-nx (banjo_gpu_vfetch_block): the n words from dword with no branch. Each word is loaded at its index clamped
// into the buffer and replaced by 0 when its index is outside it (dword + i < 0 wraps to a large uint), which is what
// banjoVfetchWord returns for it. An empty buffer (0 words) still has a readable address (the renderer gives it the
// upload buffer's start): one word is loaded there and discarded. The old form put a test and a branch around every
// load (48 per vertex in the 4-bone skinning shaders), so no two loads were in flight together. With
// SPEC_CONSTANT_VFETCH_WORDWISE, banjoVfetchWord for each word as before.
uint4 banjoVfetchWords(uint K, int dword, uint n)
{
    // banjo_gpu_bone_padding (package 25): the renderer zero-fills the buffer from word `words` to the end of the
    // aligned block after it, so word `words` is 0 and an index clamped to it reads 0: what the select gave.
    [branch] if ((g_SpecConstants() & SPEC_CONSTANT_VFETCH_WORDWISE) == 0 && (g_SpecConstants2 & SPEC2_BONES_SELECT) == 0)
    {
        const uint2 address = uint2(MASSEFFECT_SHARED_UINT(520 + K * 8), MASSEFFECT_SHARED_UINT(524 + K * 8));
        const uint words = MASSEFFECT_SHARED_UINT(536 + K * 4);
        uint4 w = uint4(0, 0, 0, 0);
        const uint64_t base = (uint64_t(address.y) << 32) | uint64_t(address.x);
        [unroll] for (uint i = 0; i < 4; i++)
        {
            if (i < n)
                w[i] = vk::RawBufferLoad<uint>(base + uint64_t(min(uint(dword + int(i)), words) * 4u), 4);
        }
        return w;
    }
    [branch] if ((g_SpecConstants() & SPEC_CONSTANT_VFETCH_WORDWISE) == 0)
    {
        const uint2 address = MASSEFFECT_UBO ? uint2(MASSEFFECT_SHARED_UINT(520 + K * 8), MASSEFFECT_SHARED_UINT(524 + K * 8))
                                             : vk::RawBufferLoad<uint2>(g_PushConstants.SharedConstants + 520 + K * 8, 8);
        const uint words = MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(536 + K * 4)
                                          : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 536 + K * 4);
        uint4 w = uint4(0, 0, 0, 0);
        const uint64_t base = (uint64_t(address.y) << 32) | uint64_t(address.x);
        const uint last = max(words, 1u) - 1u;
        [unroll] for (uint i = 0; i < 4; i++)
        {
            if (i < n)
            {
                const uint index = uint(dword + int(i));
                // 32-bit byte offset: the renderer caps these buffers far below 4 GB (kIndexedFetchMaxBytes).
                const uint loaded = vk::RawBufferLoad<uint>(base + uint64_t(min(index, last) * 4u), 4);
                w[i] = select(index < words, loaded, 0u);
            }
        }
        return w;
    }
    uint4 w = uint4(banjoVfetchWord(K, dword), 0, 0, 0);
    if (n > 1) w.y = banjoVfetchWord(K, dword + 1);
    if (n > 2) w.z = banjoVfetchWord(K, dword + 2);
    if (n > 3) w.w = banjoVfetchWord(K, dword + 3);
    return w;
}

float4 banjoVfetch(uint K, int dword, uint format, uint isSigned, uint isInteger, int expAdjust)
{
    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    const uint4 words = banjoVfetchWords(K, dword, banjoVfetchCount(format));
    const uint w0 = words.x;
    switch (format)
    {
    case 6:   // k_8_8_8_8
        result = banjoVfetchPacked(uint4(w0, w0 >> 8, w0 >> 16, w0 >> 24), uint4(8, 8, 8, 8), isSigned, isInteger);
        break;
    case 7:   // k_2_10_10_10
        result = banjoVfetchPacked(uint4(w0, w0 >> 10, w0 >> 20, w0 >> 30), uint4(10, 10, 10, 2), isSigned, isInteger);
        break;
    case 16:  // k_10_11_11
        result = banjoVfetchPacked(uint4(w0, w0 >> 11, w0 >> 22, 0), uint4(11, 11, 10, 0), isSigned, isInteger);
        break;
    case 17:  // k_11_11_10
        result = banjoVfetchPacked(uint4(w0, w0 >> 10, w0 >> 21, 0), uint4(10, 11, 11, 0), isSigned, isInteger);
        break;
    case 25:  // k_16_16
        result = banjoVfetchPacked(uint4(w0, w0 >> 16, 0, 0), uint4(16, 16, 0, 0), isSigned, isInteger);
        break;
    case 26:  // k_16_16_16_16
    {
        const uint w1 = words.y;
        result = banjoVfetchPacked(uint4(w0, w0 >> 16, w1, w1 >> 16), uint4(16, 16, 16, 16), isSigned, isInteger);
        break;
    }
    case 31:  // k_16_16_FLOAT
        result.xy = f16tof32(uint2(w0, w0 >> 16));
        return result;
    case 32:  // k_16_16_16_16_FLOAT
    {
        const uint w1 = words.y;
        result = f16tof32(uint4(w0, w0 >> 16, w1, w1 >> 16));
        return result;
    }
    case 33:  // k_32
    case 34:  // k_32_32
    case 35:  // k_32_32_32_32
    {
        const uint count = format - 32;
        [unroll] for (uint i = 0; i < 4; i++)
        {
            if (i >= count)
                break;
            const uint w = words[i];
            float value = isSigned != 0 ? float(int(w)) : float(w);
            if (isInteger == 0)
                value = isSigned != 0 ? max(value / 2147483647.0, -1.0) : value / 4294967295.0;
            result[i] = value;
        }
        break;
    }
    case 36:  // k_32_FLOAT
        return float4(asfloat(w0), 0.0, 0.0, 1.0);
    case 37:  // k_32_32_FLOAT
        return float4(asfloat(w0), asfloat(words.y), 0.0, 1.0);
    case 57:  // k_32_32_32_FLOAT
        return float4(asfloat(w0), asfloat(words.y), asfloat(words.z), 1.0);
    case 38:  // k_32_32_32_32_FLOAT
        return asfloat(words);
    default:
        break;
    }
    return expAdjust != 0 ? result * exp2(float(expAdjust)) : result;
}

// banjo-nx (banjo_gpu_vfetch_block, 2026-10-16): a k_32_32_32_32_FLOAT element whose word index is a multiple of 4 (the
// translator emits this only when the fetch's stride and offset are: the 4-bone skinning shaders' bone rows) in ONE
// 16-byte load instead of four word loads, each with its own 64-bit address. The renderer gives every such buffer a
// 16-byte aligned address readable to the next multiple of 16 bytes, and keys a draw whose buffer it cannot place
// that way (a memexport stream, a reused copy aligned otherwise) with SPEC_CONSTANT_VFETCH_WORDWISE, which takes
// banjoVfetch: the branch folds when the pipeline is created, so each pipeline holds one of the two. The words are
// the ones banjoVfetchWords gives: each word past the buffer's end is 0 (index + i < words); an empty buffer reads
// its readable start and keeps none of it.
float4 banjoVfetchAligned4(uint K, int dword)
{
    // banjo_gpu_bone_padding (package 25): zeros from word `words` to 4 words past its aligned block end, so the
    // block at the aligned end is all 0 and an index clamped to it reads 0; a block that straddles `words` reads
    // its real words and then zeros. The same words as the select below gives.
    [branch] if ((g_SpecConstants() & SPEC_CONSTANT_VFETCH_WORDWISE) == 0 && (g_SpecConstants2 & SPEC2_BONES_SELECT) == 0)
    {
        const uint2 address = uint2(MASSEFFECT_SHARED_UINT(520 + K * 8), MASSEFFECT_SHARED_UINT(524 + K * 8));
        const uint words = MASSEFFECT_SHARED_UINT(536 + K * 4);
        const uint64_t base = (uint64_t(address.y) << 32) | uint64_t(address.x);
        const uint limit = (words + 3u) & ~3u;
        return asfloat(vk::RawBufferLoad<uint4>(base + uint64_t(min(uint(dword), limit) * 4u), 16));
    }
    [branch] if ((g_SpecConstants() & SPEC_CONSTANT_VFETCH_WORDWISE) == 0)
    {
        const uint2 address = MASSEFFECT_UBO ? uint2(MASSEFFECT_SHARED_UINT(520 + K * 8), MASSEFFECT_SHARED_UINT(524 + K * 8))
                                             : vk::RawBufferLoad<uint2>(g_PushConstants.SharedConstants + 520 + K * 8, 8);
        const uint words = MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(536 + K * 4)
                                          : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 536 + K * 4);
        const uint64_t base = (uint64_t(address.y) << 32) | uint64_t(address.x);
        const uint index = uint(dword);
        const uint last = (max(words, 1u) - 1u) & ~3u;  // the aligned block that holds the last word
        const uint4 loaded = vk::RawBufferLoad<uint4>(base + uint64_t(min(index, last) * 4u), 16);
        return asfloat(select(uint4(index, index + 1u, index + 2u, index + 3u) < words, loaded, 0u));
    }
    return banjoVfetch(K, dword, 38, 0, 0, 0);
}

// banjo-nx: memory export (Xenos memexport: the eA / eM0..4 exports of a vertex shader), as
// graphics/pipeline/shader/dxbc_translator_memexport.cpp without its 8- and 16-bit element formats. eA is the stream:
// x = 0x40000000 | (base address >> 2), y = 2^23 + element index (as a float), z = endianness (bits 0-2), format
// (8-13), signed (16), integer (17), red/blue swap (19), w = 2^23 + element count; eMk is element index + k. The
// renderer gives each stream of the draw a buffer (shared bytes 656 + 16S: guest base, size in bytes, device address)
// holding the guest words in host order (as a vertex upload with the stream's endianness leaves them): later draws of
// the same submission read it as their vertex buffer, and it is copied back to guest memory (swapped) once the GPU is
// done with it. The elements are therefore stored in host word order; 8-in-64 / 8-in-128 are taken as 8-in-32.
uint banjoMemexportSwap(uint v, uint endian)
{
    if (endian == 1u || endian == 2u)
        v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (endian == 2u || endian == 3u)
        v = (v << 16) | (v >> 16);
    return v;
}

uint banjoMemexportPack(float4 v, uint4 widths, uint isSigned, uint isInteger)
{
    uint packed = 0u;
    uint shift = 0u;
    [unroll] for (uint i = 0; i < 4; i++)
    {
        const uint width = widths[i];
        if (width == 0u)
            continue;
        const float x = isnan(v[i]) ? 0.0 : v[i];
        uint bits;
        if (isSigned != 0u)
        {
            const float maxValue = float((1u << (width - 1u)) - 1u);
            const float scaled = isInteger != 0u ? clamp(x, -1.0 - maxValue, maxValue) : clamp(x, -1.0, 1.0) * maxValue;
            bits = uint(int(round(scaled))) & ((1u << width) - 1u);
        }
        else
        {
            const float maxValue = float((1u << width) - 1u);
            bits = uint(round(isInteger != 0u ? clamp(x, 0.0, maxValue) : saturate(x) * maxValue));
        }
        packed |= bits << shift;
        shift += width;
    }
    return packed;
}

// banjo-nx: tessellated triangle patches (Xenos kTrianglePatch; Nuts & Bolts' water, adaptive). The renderer has no
// tessellation stages: it draws patches * 3 * N * N vertices as a triangle list and each vertex invocation is one
// corner of a uniform N-segment barycentric grid over its patch (N = the draw's largest edge factor, so neighbouring
// patches meet without cracks). As in the emulation's domain shader (spirv_translator.cpp kTriangleDomainPatchIndexed):
// r0.xyz = domain location zyx, r1.x = patch index, r1.y = 0. Shared bytes 720: N (bit 31: flip the winding),
// 724: first patch index, 728 / 732: patch index range.
// banjo_gpu_tess_grid (bit 30 of the word, 2026-10-16): the renderer draws the patches indexed instead, one vertex
// invocation per grid point ((N + 1)(N + 2) / 2 a patch, not 3 N^2: each point is shared by up to six triangles), and
// its index list names, for each corner of the list above, the grid point (a, b) that corner decodes to. The domain
// of a point is computed from (a, b) with the same operations, so every corner gets the same values.
#define g_TessLevel     (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(720) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 720))
#define g_TessPatchBase (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(724) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 724))
#define g_TessPatchMin  (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(728) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 728))
#define g_TessPatchMax  (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(732) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 732))

void banjoTessDomain(uint id, inout float4 r0, inout float4 r1)
{
    const uint word = g_TessLevel;
    const uint n = word & 0xFFFFu;
    uint patch, a, b;
    [branch] if ((word & 0x40000000u) != 0u)
    {
        // banjo_gpu_tess_grid: id is a grid point; point (a, b) of a patch is number a (a + 1) / 2 + b in it.
        const uint points = (n + 1u) * (n + 2u) / 2u;
        patch = id / points;
        const uint local = id - patch * points;
        uint row = uint((sqrt(8.0 * float(local) + 1.0) - 1.0) * 0.5);
        if ((row + 1u) * (row + 2u) / 2u <= local)
            row++;
        if (row * (row + 1u) / 2u > local)
            row--;
        a = row;
        b = local - row * (row + 1u) / 2u;
    }
    else
    {
    const uint perPatch = 3u * n * n;
    patch = id / perPatch;
    const uint local = id - patch * perPatch;
    const uint t = local / 3u;
    uint corner = local - t * 3u;
    if ((word >> 31) != 0u && corner != 0u)
        corner = 3u - corner;
    // Row i (from the apex) holds 2i + 1 triangles: t = i^2 + j.
    uint row = uint(sqrt(float(t)));
    if ((row + 1u) * (row + 1u) <= t)
        row++;
    if (row * row > t)
        row--;
    const uint j = t - row * row;
    const uint k = j >> 1;
    // Grid point (a, b), 0 <= b <= a <= n; both triangle kinds keep the same orientation in (a, b).
    a = row;
    b = k;
    if ((j & 1u) == 0u)
    {
        if (corner == 1u) { a = row + 1u; b = k; }
        else if (corner == 2u) { a = row + 1u; b = k + 1u; }
    }
    else
    {
        if (corner == 1u) { a = row + 1u; b = k + 1u; }
        else if (corner == 2u) { a = row; b = k + 1u; }
    }
    }
    const float inv = 1.0 / float(n);
    const float3 domain = float3(1.0 - float(a) * inv, float(a - b) * inv, float(b) * inv);
    r0 = float4(domain.z, domain.y, domain.x, 0.0);
    r1 = float4(float(clamp(patch + g_TessPatchBase, g_TessPatchMin, g_TessPatchMax)), 0.0, 0.0, 0.0);
}

void banjoMemexportStore(float4 eA, float4 e0, float4 e1, float4 e2, float4 e3, float4 e4, uint written)
{
    if (written == 0u)
        return;
    const uint4 a = asuint(eA);
    if ((a.x >> 30) != 1u || (a.y >> 23) != 0x96u || (a.z >> 23) != 0x96u || (a.w >> 23) != 0x96u)
        return;
    const uint base = (a.x & 0x3FFFFFFFu) << 2;
    uint64_t device = 0;
    uint limit = 0u;
    [unroll] for (uint s = 0; s < 4; s++)
    {
        const uint4 stream = MASSEFFECT_UBO
            ? uint4(MASSEFFECT_SHARED_UINT(656 + s * 16), MASSEFFECT_SHARED_UINT(660 + s * 16),
                    MASSEFFECT_SHARED_UINT(664 + s * 16), MASSEFFECT_SHARED_UINT(668 + s * 16))
            : vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 656 + s * 16, 16);
        if (stream.y != 0u && stream.x == base)
        {
            device = (uint64_t(stream.w) << 32) | uint64_t(stream.z);
            limit = stream.y;
        }
    }
    if (device == 0)
        return;
    const uint index = a.y & 0x7FFFFFu;
    const uint count = a.w & 0x7FFFFFu;
    const uint format = (a.z >> 8) & 0x3Fu;
    const uint isSigned = (a.z >> 16) & 1u;
    const uint isInteger = (a.z >> 17) & 1u;
    const uint endian = a.z & 7u;
    const bool swapRB = ((a.z >> 19) & 1u) != 0u;
    float4 elements[5] = {e0, e1, e2, e3, e4};
    [unroll] for (uint k = 0; k < 5; k++)
    {
        if (((written >> k) & 1u) == 0u || index + k >= count)
            continue;
        const float4 v = swapRB ? elements[k].zyxw : elements[k];
        uint4 words = uint4(0u, 0u, 0u, 0u);
        uint n = 0u;
        switch (format)
        {
        case 6u:   // k_8_8_8_8
        case 50u:  // k_8_8_8_8_AS_16_16_16_16
            words.x = banjoMemexportPack(v, uint4(8, 8, 8, 8), isSigned, isInteger);
            n = 1u;
            break;
        case 7u:   // k_2_10_10_10
        case 54u:  // k_2_10_10_10_AS_16_16_16_16
            words.x = banjoMemexportPack(v, uint4(10, 10, 10, 2), isSigned, isInteger);
            n = 1u;
            break;
        case 25u:  // k_16_16
            words.x = banjoMemexportPack(v, uint4(16, 16, 0, 0), isSigned, isInteger);
            n = 1u;
            break;
        case 26u:  // k_16_16_16_16
            words.x = banjoMemexportPack(v, uint4(16, 16, 0, 0), isSigned, isInteger);
            words.y = banjoMemexportPack(v.zwxy, uint4(16, 16, 0, 0), isSigned, isInteger);
            n = 2u;
            break;
        case 31u:  // k_16_16_FLOAT
            words.x = f32tof16(v.x) | (f32tof16(v.y) << 16);
            n = 1u;
            break;
        case 32u:  // k_16_16_16_16_FLOAT
            words.x = f32tof16(v.x) | (f32tof16(v.y) << 16);
            words.y = f32tof16(v.z) | (f32tof16(v.w) << 16);
            n = 2u;
            break;
        case 36u:  // k_32_FLOAT
            words.x = asuint(v.x);
            n = 1u;
            break;
        case 37u:  // k_32_32_FLOAT
            words.xy = asuint(v.xy);
            n = 2u;
            break;
        case 38u:  // k_32_32_32_32_FLOAT
            words = asuint(v);
            n = 4u;
            break;
        default:
            return;
        }
        const uint offset = (index + k) * n * 4u;
        if (offset + n * 4u > limit)
            continue;
        [unroll] for (uint i = 0; i < 4; i++)
        {
            if (i < n)
                vk::RawBufferStore<uint>(device + offset + i * 4u, words[i]);
        }
    }
}

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_AlphaFunction : packoffset(c17.y); \
    float2 g_NdcScale : packoffset(c17.z); \
    float2 g_NdcOffset : packoffset(c18.x);

uint g_SpecConstants();
static const uint g_SpecConstants2 = 0;
#define BANJO_FLT_LO FLT_MIN
#define BANJO_FLT_HI FLT_MAX
#define BANJO_OUTPUTS_WRITE_ZERO false
#define BANJO_LITERAL_PS(N, X, Y, Z, W, M) asfloat(uint4(X, Y, Z, W))
#define BANJO_LITERAL_VS(N, X, Y, Z, W, M) asfloat(uint4(X, Y, Z, W))
#define BANJO_SLOT_SIGNS(REG) true
#define BANJO_PS_READS(L) true

#define g_InputRemap(LOC) 0xFFF
#define BANJO_BOOL(A) 0
#define g_VertexIndexBase 0u
float4 banjoVfetch(uint K, int dword, uint format, uint isSigned, uint isInteger, int expAdjust) { return float4(0.0, 0.0, 0.0, 1.0); }
float4 banjoVfetchAligned4(uint K, int dword) { return float4(0.0, 0.0, 0.0, 0.0); }
void banjoMemexportStore(float4 eA, float4 e0, float4 e1, float4 e2, float4 e3, float4 e4, uint written) {}
#define g_TessLevel 0u
void banjoTessDomain(uint id, inout float4 r0, inout float4 r1) {}

#endif


// banjo-nx (banjo_gpu_branchless_select): the translator's conditional values without a branch. With HLSL 2021 a
// scalar ?: (and &&, ||) short-circuits: DXC writes a branch and a phi, and NAK keeps every branch (it flattens
// nothing), so each predicate update or kill test was a branch of its own. select() / and() / or() give the same
// values with no control flow (the operands have no side effects). SPEC_CONSTANT_SHORT_CIRCUIT: the old forms; the
// test on it is decided when the pipeline is specialised.
#define BANJO_SHORT_CIRCUIT ((g_SpecConstants() & SPEC_CONSTANT_SHORT_CIRCUIT) != 0)
#define banjoSel(C, A, B) (BANJO_SHORT_CIRCUIT ? ((C) ? (A) : (B)) : select((C), (A), (B)))
#define banjoAnd(A, B) (BANJO_SHORT_CIRCUIT ? ((A) && (B)) : and((A), (B)))
#define banjoOr(A, B) (BANJO_SHORT_CIRCUIT ? ((A) || (B)) : or((A), (B)))
float banjoSel0(bool c, float v) { return BANJO_SHORT_CIRCUIT ? (c ? 0.0 : v) : select(c, 0.0, v); }
float2 banjoSel0(bool c, float2 v)
{
    [branch] if (BANJO_SHORT_CIRCUIT) return c ? 0.0 : v;
    return select(bool2(c, c), float2(0.0, 0.0), v);
}
float3 banjoSel0(bool c, float3 v)
{
    [branch] if (BANJO_SHORT_CIRCUIT) return c ? 0.0 : v;
    return select(bool3(c, c, c), float3(0.0, 0.0, 0.0), v);
}
float4 banjoSel0(bool c, float4 v)
{
    [branch] if (BANJO_SHORT_CIRCUIT) return c ? 0.0 : v;
    return select(bool4(c, c, c, c), float4(0.0, 0.0, 0.0, 0.0), v);
}

// banjo-nx: the W of a MOVA's vector operand; the translator passes it already reduced by the write mask.
float banjoMovaW(float v) { return v; }
float banjoMovaW(float2 v) { return v.y; }
float banjoMovaW(float3 v) { return v.z; }
float banjoMovaW(float4 v) { return v.w; }
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

// The native renderer packs the four post-swizzle Xenos TextureSign values into bits 24-31 of the
// descriptor index. Values: 0 unsigned, 1 signed (the renderer still warns for it), 2 unsigned biased,
// 3 Xenos piecewise-linear gamma. Descriptor heaps use the low 24 bits.
// banjo-nx: the four components at once and without per-component branches. The old form (a loop of four
// if / else if, and four nested ifs in the gamma curve) unrolled into ~20 blocks per texture fetch, and a
// material pixel shader has dozens of fetches. Same operations on the same values: the comparisons pick the
// same scale and offset, and components whose sign is 0 or 1 pass through select() untouched. One branch skips
// all of it for textures without signs (almost all of them).
float4 masseffectGammaToLinear(float4 gamma)
{
    gamma = saturate(gamma);
    const bool4 high = gamma >= 96.0 / 255.0;
    const bool4 top = gamma >= 192.0 / 255.0;
    const bool4 mid = gamma >= 64.0 / 255.0;
    const float4 scale = select(high, select(top, 8.0 / 1024.0, 4.0 / 1024.0), select(mid, 2.0 / 1024.0, 1.0 / 1024.0));
    const float4 offset = select(high, select(top, -1024.0, -256.0), select(mid, -64.0, 0.0));
    float4 linearValue = gamma * ((255.0 * 1024.0) * scale) + offset;
    linearValue += trunc(linearValue * scale);
    return linearValue * (1.0 / 1023.0);
}

// banjo-nx: the per-register flags the translator passes to the fetches (sN_Signs: one name per fetch constant, so
// tools/early_alpha.py reads them like the other sN_ slot names).
#define s0_Signs BANJO_SLOT_SIGNS(0)
#define s1_Signs BANJO_SLOT_SIGNS(1)
#define s2_Signs BANJO_SLOT_SIGNS(2)
#define s3_Signs BANJO_SLOT_SIGNS(3)
#define s4_Signs BANJO_SLOT_SIGNS(4)
#define s5_Signs BANJO_SLOT_SIGNS(5)
#define s6_Signs BANJO_SLOT_SIGNS(6)
#define s7_Signs BANJO_SLOT_SIGNS(7)
#define s8_Signs BANJO_SLOT_SIGNS(8)
#define s9_Signs BANJO_SLOT_SIGNS(9)
#define s10_Signs BANJO_SLOT_SIGNS(10)
#define s11_Signs BANJO_SLOT_SIGNS(11)
#define s12_Signs BANJO_SLOT_SIGNS(12)
#define s13_Signs BANJO_SLOT_SIGNS(13)
#define s14_Signs BANJO_SLOT_SIGNS(14)
#define s15_Signs BANJO_SLOT_SIGNS(15)
#define s16_Signs BANJO_SLOT_SIGNS(16)
#define s17_Signs BANJO_SLOT_SIGNS(17)
#define s18_Signs BANJO_SLOT_SIGNS(18)
#define s19_Signs BANJO_SLOT_SIGNS(19)
#define s20_Signs BANJO_SLOT_SIGNS(20)
#define s21_Signs BANJO_SLOT_SIGNS(21)
#define s22_Signs BANJO_SLOT_SIGNS(22)
#define s23_Signs BANJO_SLOT_SIGNS(23)
#define s24_Signs BANJO_SLOT_SIGNS(24)
#define s25_Signs BANJO_SLOT_SIGNS(25)
#define s26_Signs BANJO_SLOT_SIGNS(26)
#define s27_Signs BANJO_SLOT_SIGNS(27)
#define s28_Signs BANJO_SLOT_SIGNS(28)
#define s29_Signs BANJO_SLOT_SIGNS(29)
#define s30_Signs BANJO_SLOT_SIGNS(30)
#define s31_Signs BANJO_SLOT_SIGNS(31)

// banjo_gpu_sign_mask_skip (package 25): the descriptor index without its packed signs. The renderer clears the top
// byte of a slot whose texture has no effective sign (2 or 3), so the mask is needed only where constant 61 says the
// slot may have one and the draw's whole-draw bit (SPEC_CONSTANT_NO_TEXTURE_SIGNS) is clear.
uint banjoIndexWithoutSigns(uint index, bool slotSigns)
{
    [branch] if ((g_SpecConstants2 & SPEC2_SIGN_MASK_ALWAYS) == 0 &&
                 (!slotSigns || (g_SpecConstants() & SPEC_CONSTANT_NO_TEXTURE_SIGNS) != 0))
        return index;
    return index & 0x00FFFFFF;
}

float4 masseffectApplyTextureSigns(float4 value, uint packedSigns)
{
    // sa2-nx (banjo_gpu_no_texture_signs): the renderer sets the bit only when every texture of the draw has packed
    // signs 0, which is the early return below: the test goes with it.
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_NO_TEXTURE_SIGNS)
        return value;
    [branch] if (packedSigns == 0)
        return value;
    const uint4 sign = (uint4(packedSigns, packedSigns, packedSigns, packedSigns) >> uint4(0, 2, 4, 6)) & 3;
    return select(sign == 2, value * 2.0 - 1.0, select(sign == 3, masseffectGammaToLinear(value), value));
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize,
               bool slotSigns = true)
{
    uint packedSigns = slotSigns ? resourceDescriptorIndex >> 24 : 0u;
    resourceDescriptorIndex = banjoIndexWithoutSigns(resourceDescriptorIndex, slotSigns);
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    // With the bit set, the size comes from a constant and the texture does not have to be queried. It
    // is the same computation with the same number: the renderer writes 1/size of the host image. With a
    // ternary, DXC evaluates both branches and the size query stays: an if with [branch] is needed for
    // the dead branch to disappear when the pipeline is specialized.
    // banjo-nx: always by constant (the renderer forces masseffect_native_inv_tex_size on): the branch is
    // decided at compile time, like MASSEFFECT_UBO.
    float2 displacement = offset * invSize;
    return masseffectApplyTextureSigns(
        texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + displacement), packedSigns);
}

// banjo-nx: a 1D texture is a row of the 2D heap (sN_Texture1DDescriptorIndex is the 2D index).
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], float2(texCoord, 0.5));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    resourceDescriptorIndex = banjoIndexWithoutSigns(resourceDescriptorIndex, true);
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset,
                       bool slotSigns = true)
{
    uint packedSigns = slotSigns ? resourceDescriptorIndex >> 24 : 0u;
    resourceDescriptorIndex = banjoIndexWithoutSigns(resourceDescriptorIndex, slotSigns);
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return masseffectApplyTextureSigns(r, packedSigns);
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, bool slotSigns = true)
{
    uint packedSigns = slotSigns ? resourceDescriptorIndex >> 24 : 0u;
    resourceDescriptorIndex = banjoIndexWithoutSigns(resourceDescriptorIndex, slotSigns);
    return masseffectApplyTextureSigns(
        g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord),
        packedSigns);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData,
                  bool slotSigns = true)
{
    uint packedSigns = slotSigns ? resourceDescriptorIndex >> 24 : 0u;
    resourceDescriptorIndex = banjoIndexWithoutSigns(resourceDescriptorIndex, slotSigns);
    return masseffectApplyTextureSigns(
        g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]),
        packedSigns);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    // sa2-nx (banjo_gpu_texcoords_unswapped): swappedTexcoords is always 0 in the native renderer.
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_TEXCOORDS_UNSWAPPED)
        return value;
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

// MASSEFFECT: 3 bits per component: 0-3 = data component, 4 = 0, 5 = 1, 7 = unchanged.
float4 remapInput(float4 value, uint code)
{
    // sa2-nx (banjo_gpu_identity_remap): set only when every code of the pipeline's vertex input is 0xFFF, the early
    // return below.
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_IDENTITY_REMAP)
        return value;
    [branch] if (code == 0xFFF)
        return value;

    // banjo-nx: selects instead of an if / else if chain per component (fewer blocks for the driver).
    const uint4 source = (uint4(code, code, code, code) >> uint4(0, 3, 6, 9)) & 7;
    float4 picked;
    [unroll] for (uint i = 0; i < 4; i++)
        picked[i] = value[min(source[i], 3u)];
    return select(source < 4, picked, select(source == 4, 0.0, select(source == 5, 1.0, value)));
}

// MASSEFFECT: Xenos alpha test with its comparison function. Positive if the pixel passes.
float alphaTestValue(float alpha)
{
    bool pass = true;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaThreshold; break;
    case 2: pass = alpha == g_AlphaThreshold; break;
    case 3: pass = alpha <= g_AlphaThreshold; break;
    case 4: pass = alpha > g_AlphaThreshold; break;
    case 5: pass = alpha != g_AlphaThreshold; break;
    case 6: pass = alpha >= g_AlphaThreshold; break;
    default: break;
    }
    return pass ? 1.0 : -1.0;
}

// banjo-nx (banjo_gpu_early_alpha): true only when the alpha test fails for every alpha within
// (|alpha| + |ref|) * 2^-12 of this one (plus 1e-30, beyond any denormal). The early copy runs the same operations on
// the same values as the final alpha, so it can differ from it only by a multiply-add the driver fused in one place and
// not the other (each fusion moves the result by at most half an ulp of the product: 2^-24 of it, inside the margin
// unless the sum cancels more than 2^11 times its terms), or a flushed denormal: it never fails a pixel the final
// test passes. NaN never fails early.
bool banjoAlphaFailsEarly(float alpha)
{
    const float t = g_AlphaThreshold;
    const float d = (abs(alpha) + abs(t)) * (1.0 / 4096.0) + 1.0e-30;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    case 0: return true;
    case 1: return alpha - d >= t;
    case 2: return abs(alpha - t) > d;
    case 3: return alpha - d > t;
    case 4: return alpha + d <= t;
    case 6: return alpha + d < t;
    default: return false;
    }
}

// banjo_gpu_early_alpha: discards the 2x2 quad when all four of its invocations (helpers included) fail early. A pixel
// is never discarded alone: the implicit-LOD fetches of the others in its quad still need it. Fine derivatives of a
// 0 / 1 value are exact. Call in uniform control flow.
void banjoEarlyAlphaQuad(bool fails)
{
    const float f = fails ? 1.0 : 0.0;
    const float dx = ddx_fine(f);
    const float pair = (f == 1.0 && dx == 0.0) ? 1.0 : 0.0;  // this pixel and its horizontal neighbour
    const float dy = ddy_fine(pair);
    if (pair == 1.0 && dy == 0.0) discard;  // and the pair above or below
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    resourceDescriptorIndex = banjoIndexWithoutSigns(resourceDescriptorIndex, true);
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
