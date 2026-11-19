# tw2de_nx (2026-11-03): vupkd3d128's "most negative field is a quiet NaN" (NORMSHORT2 -32768, NORMSHORT4 -32768,
# NORMPACKED32 -512 in x, y and z, NORMPACKED64 -524288 in x, y, z) and the NORMPACKED64 decode (the low
# doubleword, signed fields, 3.0+X / 1.0+W bit-pattern form). Expectations from a model of Xenia's x64 sequences
# (EmitSHORT_2 / EmitSHORT_4 / EmitUINT_2101010 / EmitULONG_4202020: shuffle, sign extend, add the bias, compare with
# the overflow constant, blend QNaN), tools/ppc_tests/own/gen_vupkd3d128_nan.py. The immediate is the type * 4.

test_vupkd3d128_short2_min:
  #_ REGISTER_IN v4 [11111111, 22222222, 33333333, 80008000]
  vupkd3d128 v3, v4, 4
  blr
  #_ REGISTER_OUT v3 [7FC00000, 7FC00000, 00000000, 3F800000]

test_vupkd3d128_short2_mixed:
  #_ REGISTER_IN v4 [00000000, 00000000, 00000000, 80007FFF]
  vupkd3d128 v3, v4, 4
  blr
  #_ REGISTER_OUT v3 [7FC00000, 40407FFF, 00000000, 3F800000]

test_vupkd3d128_short2_aliased_min:
  #_ REGISTER_IN v4 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 00018000]
  vupkd3d128 v3, v4, 4
  blr
  #_ REGISTER_OUT v3 [40400001, 7FC00000, 00000000, 3F800000]

test_vupkd3d128_short2_aliased_min_same:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 00018000]
  vupkd3d128 v3, v3, 4
  blr
  #_ REGISTER_OUT v3 [40400001, 7FC00000, 00000000, 3F800000]

test_vupkd3d128_short4_min:
  #_ REGISTER_IN v4 [00000000, 00000000, 80000001, FFFF8000]
  vupkd3d128 v3, v4, 16
  blr
  #_ REGISTER_OUT v3 [7FC00000, 40400001, 403FFFFF, 7FC00000]

test_vupkd3d128_short4_plain:
  #_ REGISTER_IN v4 [CDCDCDCD, CDCDCDCD, 7FFFFFFF, 007FFFF8]
  vupkd3d128 v3, v4, 16
  blr
  #_ REGISTER_OUT v3 [40407FFF, 403FFFFF, 4040007F, 403FFFF8]

test_vupkd3d128_packed32_min_y:
  #_ REGISTER_IN v4 [00000000, 00000000, 00000000, 00080005]
  vupkd3d128 v3, v4, 8
  blr
  #_ REGISTER_OUT v3 [40400005, 7FC00000, 40400000, 3F800000]

test_vupkd3d128_packed32_min_z:
  #_ REGISTER_IN v4 [00000000, 00000000, 00000000, E00001FF]
  vupkd3d128 v3, v4, 8
  blr
  #_ REGISTER_OUT v3 [404001FF, 40400000, 7FC00000, 3F800003]

test_vupkd3d128_packed32_min_xyz:
  #_ REGISTER_IN v4 [00000000, 00000000, 00000000, 20080200]
  vupkd3d128 v3, v4, 8
  blr
  #_ REGISTER_OUT v3 [7FC00000, 7FC00000, 7FC00000, 3F800000]

test_vupkd3d128_packed64_plain:
  #_ REGISTER_IN v4 [DEADBEEF, CAFEF00D, 50000300, 00400001]
  vupkd3d128 v3, v4, 24
  blr
  #_ REGISTER_OUT v3 [40400001, 40400004, 40400003, 3F800005]

test_vupkd3d128_packed64_min_x:
  #_ REGISTER_IN v4 [00000000, 00000000, 00000000, 00080000]
  vupkd3d128 v3, v4, 24
  blr
  #_ REGISTER_OUT v3 [7FC00000, 40400000, 40400000, 3F800000]

test_vupkd3d128_packed64_min_yz:
  #_ REGISTER_IN v4 [00000000, 00000000, 08000080, 00000000]
  vupkd3d128 v3, v4, 24
  blr
  #_ REGISTER_OUT v3 [40400000, 7FC00000, 7FC00000, 3F800000]
